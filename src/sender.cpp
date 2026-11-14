// SRTP video sender: streams a video file to a browser over WebRTC.
//
// Signaling: libdatachannel WebSocketServer (port 8080). The browser sends an
// SDP offer, we answer. Media: H.264 baseline, no B-frames, read from disk by
// an ffmpeg child process (annex-B elementary stream on stdout), packetized
// into raw RTP and handed to a send-only Track. libdatachannel then carries it
// as SRTP over the DTLS/UDP transport.
//
// The RTP headers we emit have the extension bit (X) hard-zeroed: extension
// headers (abs-capture-time, video-orientation, ...) make browsers hold frames
// in a jitter/rendering buffer before display, which adds latency for no
// benefit here since we run our own pacing.

#include <rtc/rtc.hpp>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <optional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using std::shared_ptr;
using std::string;
using std::vector;

namespace {

// ---- minimal JSON string helpers (signaling messages only) -----------------

string jsonEscape(const string &s) {
	string out;
	out.reserve(s.size() + 16);
	for (char c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default: out += c;
		}
	}
	return out;
}

// Extract and unescape the JSON string value of `key` from `msg`.
// Returns empty optional if not found.
std::optional<string> jsonGet(const string &msg, const string &key) {
	string needle = "\"" + key + "\"";
	size_t pos = msg.find(needle);
	if (pos == string::npos)
		return std::nullopt;
	pos = msg.find(':', pos + needle.size());
	if (pos == string::npos)
		return std::nullopt;
	++pos;
	while (pos < msg.size() && isspace(static_cast<unsigned char>(msg[pos])))
		++pos;
	if (pos >= msg.size() || msg[pos] != '"')
		return std::nullopt;
	++pos;
	string out;
	while (pos < msg.size()) {
		char c = msg[pos++];
		if (c == '"')
			return out;
		if (c == '\\' && pos < msg.size()) {
			char e = msg[pos++];
			switch (e) {
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case 'u': out += '?'; pos += 3; break;
			default: out += e;
			}
		} else {
			out += c;
		}
	}
	return std::nullopt;
}

// ---- H.264 annex-B parsing --------------------------------------------------

struct Nal {
	size_t begin; // offset of NAL header byte in the stream buffer
	size_t end;   // offset one past the last payload byte
};

// Finds complete NALs in `buf` and appends them to `nals`. A NAL starts after
// a 00 00 01 or 00 00 00 01 start code and runs until the next start code.
// Returns how many leading bytes of `buf` the caller may erase once it is done
// reading the NALs: everything up to the start code of a still-unterminated
// trailing NAL is retained so NALs split across reads are not lost. `buf` is
// left unmodified so `nals` offsets stay valid.
size_t extractNals(const vector<uint8_t> &buf, vector<Nal> &nals) {
	size_t i = 0;
	std::optional<size_t> nalStart;
	while (i + 2 < buf.size()) {
		if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
			size_t prefix = (i > 0 && buf[i - 1] == 0) ? i - 1 : i;
			if (nalStart)
				nals.push_back({*nalStart, prefix});
			nalStart = i + 3;
			i += 3;
		} else {
			++i;
		}
	}
	if (!nalStart) {
		// No start code yet: consume everything except a trailing partial
		// start code, which may complete on the next read.
		size_t zeros = 0;
		while (zeros < buf.size() && zeros < 3 && buf[buf.size() - 1 - zeros] == 0)
			++zeros;
		return buf.size() - zeros;
	}
	return *nalStart >= 4 ? *nalStart - 4 : 0;
}

// ---- RTP packetization ------------------------------------------------------

constexpr uint8_t PAYLOAD_TYPE = 96;
constexpr uint32_t CLOCK = 90000;
constexpr uint32_t TICKS_PER_FRAME = CLOCK / 30; // ffmpeg is forced to 30 fps

// RTP headers are built by libdatachannel's H264RtpPacketizer, which by
// default emits no header extensions (extension bit X=0). Extension headers
// such as abs-capture-time or video-orientation make browsers hold frames in
// a jitter/rendering buffer before display, adding latency for no benefit
// here, so they are kept off.

// ---- ffmpeg child process ---------------------------------------------------

struct FFmpeg {
	pid_t pid = -1;
	int fd = -1;

	bool open(const string &path) {
		int fds[2];
		if (pipe(fds) != 0)
			return false;
		pid = fork();
		if (pid == 0) { // child
			dup2(fds[1], STDOUT_FILENO);
			::close(fds[0]);
			::close(fds[1]);
			execlp("ffmpeg", "ffmpeg", "-nostdin", "-loglevel", "error",
			       "-re", "-stream_loop", "-1", "-i", path.c_str(), "-an",
			       "-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency",
			       "-profile:v", "baseline", "-bf", "0", "-r", "30", "-g", "60",
			       // zerolatency + bf 0 makes x264 default to sliced threads on
			       // multi-core machines, emitting N slices (one VCL NAL each)
			       // per frame. Our annex-B access-unit splitter keys on VCL
			       // NALs, so multi-slice frames would be torn into N
			       // single-slice "frames". Force whole-frame threading so
			       // every access unit has exactly one VCL NAL.
			       "-x264-params", "sliced-threads=0",
			       "-pix_fmt", "yuv420p", "-f", "h264", "pipe:1",
			       static_cast<char *>(nullptr));
			_exit(127);
		}
		::close(fds[1]);
		fd = fds[0];
		return pid > 0;
	}

	void close() {
		// Order matters: kill the child first so its exit closes the pipe
		// write end, which unblocks the reader thread's read() with EOF.
		// Closing our end first would leave read() blocked forever (and the
		// subsequent join() would deadlock).
		if (pid > 0) {
			kill(pid, SIGKILL); // no trailer to flush on a pipe; keeps stderr quiet
			waitpid(pid, nullptr, 0);
			pid = -1;
		}
		if (fd >= 0) {
			::close(fd);
			fd = -1;
		}
	}

	~FFmpeg() { close(); }
};

// ---- peer -------------------------------------------------------------------

struct Peer {
	shared_ptr<rtc::PeerConnection> pc;
	shared_ptr<rtc::WebSocket> ws;
	shared_ptr<rtc::Track> track;
	FFmpeg ffmpeg;
	std::thread streamThread;
	std::atomic<bool> streaming{false};
	string mediaPath;
	uint32_t ssrc = 0;
	shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;

	void sendJson(const string &msg) {
		try {
			ws->send(msg);
		} catch (const std::exception &e) {
			std::cerr << "ws send failed: " << e.what() << "\n";
		}
	}

	// One access unit (annex-B, NALs prefixed with 00 00 00 01) -> RTP packets.
	void sendFrame(const vector<uint8_t> &frame) {
		while (track->bufferedAmount() > 256 * 1024)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		rtc::binary sample(frame.size());
		std::memcpy(sample.data(), frame.data(), frame.size());
		track->send(std::move(sample));
		rtpConfig->timestamp += TICKS_PER_FRAME;
	}

	void startStream() {
		if (streaming.exchange(true))
			return;
		streamThread = std::thread([this] {
			if (!ffmpeg.open(mediaPath)) {
				std::cerr << "failed to spawn ffmpeg\n";
				streaming = false;
				return;
			}
			std::cout << "streaming " << mediaPath << "\n";
			vector<uint8_t> buf;
			vector<Nal> nals;
			vector<uint8_t> frame; // annex-B access unit under construction
			bool frameHasVcl = false;
			std::array<uint8_t, 65536> chunk;
			while (streaming) {
				ssize_t n = read(ffmpeg.fd, chunk.data(), chunk.size());
				if (n <= 0)
					break; // ffmpeg died or pipe closed
				buf.insert(buf.end(), chunk.begin(), chunk.begin() + n);
				nals.clear();
				size_t consumed = extractNals(buf, nals);
				for (const Nal &nal : nals) {
					uint8_t type = buf[nal.begin] & 0x1F;
					// An access unit is complete once its VCL NAL is seen:
					// anything after it (next slice, or SPS/PPS of the next
					// keyframe) belongs to a new access unit. SPS/PPS before
					// the first VCL accumulate into the same access unit.
					if (frameHasVcl) {
						sendFrame(frame);
						frame.clear();
						frameHasVcl = false;
					}
					frame.insert(frame.end(), {0, 0, 0, 1});
					frame.insert(frame.end(), buf.begin() + nal.begin, buf.begin() + nal.end);
					if (type >= 1 && type <= 5)
						frameHasVcl = true;
				}
				buf.erase(buf.begin(), buf.begin() + consumed);
			}
			if (streaming)
				std::cerr << "ffmpeg stream ended unexpectedly\n";
			ffmpeg.close();
			streaming = false;
		});
	}

	void stopStream() {
		if (!streaming.exchange(false))
			return;
		ffmpeg.close(); // unblocks read(); thread exits, then join
		if (streamThread.joinable())
			streamThread.join();
	}

	~Peer() { stopStream(); }
};

std::optional<string> g_mediaPath;
shared_ptr<Peer> g_peer;
std::mutex g_mutex;

// Removes the current peer, destroying it OUTSIDE the mutex: destroying a
// PeerConnection runs its callbacks (state change etc.), which also take the
// mutex, and may run on threads that must not block (e.g. libdatachannel's
// poll service, which would otherwise stall all connections).
void dropPeer() {
	shared_ptr<Peer> doomed;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		doomed = std::move(g_peer);
	}
}

void handleOffer(shared_ptr<rtc::WebSocket> ws, const string &sdp) {
	bool busy;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		busy = (g_peer != nullptr);
	}
	if (busy) {
		std::cerr << "already serving one viewer, closing new client\n";
		try {
			ws->close(); // triggers our onClosed; not holding the lock here
		} catch (...) {
		}
		return;
	}

	auto peer = std::make_shared<Peer>();
	peer->ws = ws;
	peer->mediaPath = *g_mediaPath;
	peer->ssrc = uint32_t(rand()) + 1;

	rtc::Configuration cfg;
	cfg.disableAutoNegotiation = true; // we drive setLocalDescription() ourselves
	peer->pc = std::make_shared<rtc::PeerConnection>(cfg);

	// The peer is captured by these callbacks, so it stays alive while the
	// connection winds down even after dropPeer() releases it.
	peer->pc->onLocalDescription([peer](const rtc::Description &desc) {
		peer->sendJson(string("{\"type\":\"") + desc.typeString() + "\",\"sdp\":\"" +
		               jsonEscape(string(desc)) + "\"}");
	});
	peer->pc->onLocalCandidate([peer](const rtc::Candidate &cand) {
		peer->sendJson(string("{\"type\":\"candidate\",\"candidate\":\"") +
		               jsonEscape(cand.candidate()) + "\",\"sdpMid\":\"" +
		               jsonEscape(cand.mid()) + "\"}");
	});
	peer->pc->onStateChange([](rtc::PeerConnection::State state) {
		std::cout << "peer connection state: " << state << "\n";
		if (state == rtc::PeerConnection::State::Disconnected ||
		    state == rtc::PeerConnection::State::Failed ||
		    state == rtc::PeerConnection::State::Closed)
			dropPeer();
	});

	try {
		peer->pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Offer));

		// The track must carry the remote m-line mid, otherwise the answerer
		// track is not matched with the incoming media line and never opens.
		string mid = "0";
		if (auto remote = peer->pc->remoteDescription()) {
			for (int i = 0; i < remote->mediaCount(); ++i) {
				auto m = remote->media(i);
				if (std::holds_alternative<rtc::Description::Media *>(m)) {
					mid = std::get<rtc::Description::Media *>(m)->mid();
					break;
				}
			}
		}

		// Reuse the H.264 payload type (and fmtp) from the offer: the answer
		// must not renumber codecs the offerer advertised, otherwise the
		// offerer cannot map the payload type back to a codec.
		uint8_t payloadType = PAYLOAD_TYPE;
		string fmtp = "packetization-mode=1";
		{
			size_t pos = 0;
			while ((pos = sdp.find("a=rtpmap:", pos)) != string::npos) {
				size_t end = sdp.find('\n', pos);
				string line = sdp.substr(pos, end - pos);
				if (line.find("H264/") != string::npos) {
					payloadType = uint8_t(std::atoi(line.c_str() + 9));
					string needle = "a=fmtp:" + std::to_string(payloadType) + " ";
					size_t fpos = sdp.find(needle);
					if (fpos != string::npos) {
						size_t fend = sdp.find('\n', fpos);
						fmtp = sdp.substr(fpos + needle.size(), fend - fpos - needle.size());
						if (!fmtp.empty() && fmtp.back() == '\r')
							fmtp.pop_back();
					}
					break;
				}
				pos = end;
			}
		}

		rtc::Description::Video video(mid, rtc::Description::Direction::SendOnly);
		video.addH264Codec(payloadType, fmtp);
		video.addSSRC(peer->ssrc, "video-send");
		peer->track = peer->pc->addTrack(video);

		// H.264 packetizer + RTCP sender reports + NACK responses; emits
		// RTP headers with no extensions (X=0, see comment above).
		peer->rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
		    peer->ssrc, "video-send", payloadType, CLOCK);
		// Fragment size 1200 (not the library default 1220): the default
		// forgets the 10-byte SRTP auth tag, so over IPv6 (40-byte header,
		// e.g. Tailscale's 1280 MTU) each full-size datagram is 1290 bytes
		// and juice fails every one with EMSGSIZE. 1200 keeps the worst
		// case at 1270 bytes: 40 + 8 + 12 + 1200 + 10.
		auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
		    rtc::NalUnit::Separator::LongStartSequence, peer->rtpConfig, 1200);
		auto srReporter = std::make_shared<rtc::RtcpSrReporter>(peer->rtpConfig);
		packetizer->addToChain(srReporter);
		packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
		peer->track->setMediaHandler(packetizer);

		peer->track->onOpen([weak = std::weak_ptr<Peer>(peer)] {
			if (auto p = weak.lock())
				p->startStream();
		});
		peer->track->onClosed([weak = std::weak_ptr<Peer>(peer)] {
			if (auto p = weak.lock())
				p->stopStream();
		});

		peer->pc->setLocalDescription();
	} catch (const std::exception &e) {
		std::cerr << "failed to handle offer: " << e.what() << "\n";
		return; // local peer dies here; never published to g_peer
	}

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_peer = peer;
	}
	std::cout << "offer received, answering\n";
}

void onWsMessage(shared_ptr<rtc::WebSocket> ws, rtc::message_variant data) {
	if (!std::holds_alternative<string>(data))
		return;
	const string msg = std::get<string>(data);
	auto type = jsonGet(msg, "type");
	if (!type) {
		std::cerr << "bad signaling message: " << msg << "\n";
		return;
	}
	if (*type == "offer") {
		if (auto sdp = jsonGet(msg, "sdp"))
			handleOffer(ws, *sdp);
	} else if (*type == "candidate") {
		shared_ptr<rtc::PeerConnection> pc;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_peer)
				pc = g_peer->pc;
		}
		if (pc) {
			auto cand = jsonGet(msg, "candidate");
			auto mid = jsonGet(msg, "sdpMid");
			if (cand && mid)
				pc->addRemoteCandidate(rtc::Candidate(*cand, *mid));
		}
	}
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		std::cerr << "usage: " << argv[0] << " <video-file> [ws-port]\n";
		return 1;
	}
	g_mediaPath = string(argv[1]);
	uint16_t port = argc > 2 ? uint16_t(std::atoi(argv[2])) : 8080;

	srand(uint32_t(time(nullptr)) ^ getpid());
	std::cout << std::unitbuf; // logging goes to a pipe, keep it unbuffered
	rtc::InitLogger(rtc::LogLevel::Warning);

	rtc::WebSocketServer::Configuration wsCfg;
	wsCfg.port = port;
	rtc::WebSocketServer server(wsCfg);
	server.onClient([](shared_ptr<rtc::WebSocket> ws) {
		ws->onMessage([ws](rtc::message_variant data) { onWsMessage(ws, std::move(data)); });
		ws->onClosed([ws] {
			bool ours;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				ours = g_peer && g_peer->ws == ws;
			}
			if (ours)
				dropPeer(); // viewer gone, accept a new one
		});
	});
	std::cout << "sender listening for viewers on ws port " << port << "\n";

	while (true)
		std::this_thread::sleep_for(std::chrono::seconds(1));
	return 0;
}
