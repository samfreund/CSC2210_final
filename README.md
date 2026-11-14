# WebRTC H.264 file streamer

Streams a video file to a browser over WebRTC: RTP/SRTP over DTLS/UDP media,
WebSocket signaling.

## Sender (`src/sender.cpp`, libdatachannel)

- `rtc::WebSocketServer` (default port 8080) handles signaling: the browser
  sends an SDP offer + ICE candidates as JSON, the sender answers.
- An ffmpeg child process reads the file, re-encodes to **H.264 baseline,
  no B-frames** (`-profile:v baseline -bf 0 -tune zerolatency -r 30`) and
  writes an annex-B elementary stream to a pipe.
- The sender splits the stream into access units and feeds them to
  `rtc::H264RtpPacketizer` (+ `RtcpSrReporter`, `RtcpNackResponder`) attached
  to a send-only `rtc::Track`. libdatachannel carries the resulting RTP as
  SRTP over the DTLS/UDP transport.
- RTP headers are emitted with the extension bit zeroed (no abs-capture-time /
  video-orientation / mid extensions): those make browsers hold frames in a
  rendering buffer, which only adds latency here.
- The H.264 payload type and fmtp are taken from the offer (per RFC 3264 the
  answer must not renumber the offerer's codecs).
- One viewer at a time.

## Receiver (`www/receiver.html`, ~70 LOC)

`RTCPeerConnection` + `<video>`: offers over the WebSocket, applies the
answer, adds trickled candidates, plays the incoming track. Shows inbound RTP
stats every 2 s.

## Build & run

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/sender /path/to/video.mp4 8080

# serve the receiver page (any static file server works)
python3 -m http.server 8000 --directory www
# open http://localhost:8000/receiver.html in a browser
```

Requirements: cmake, a C++17 compiler, ffmpeg (runtime), OpenSSL.

## third_party

libdatachannel is vendored at `third_party/libdatachannel` (not committed).
Pin: v0.24.6, commit `2afeedc70878043bcd59d2d726e1476824f687c3`.

```sh
git clone https://github.com/paullouisageneau/libdatachannel.git third_party/libdatachannel
git -C third_party/libdatachannel checkout 2afeedc70878043bcd59d2d726e1476824f687c3
git -C third_party/libdatachannel submodule update --init --recursive
```

## Notes

- The stream is paced in real time (`ffmpeg -re`) and loops forever
  (`-stream_loop -1`); timestamps advance 3000 ticks (90 kHz clock) per frame.
- Access-unit boundaries are detected by VCL NAL types; SPS/PPS preceding an
  IDR belong to that IDR's access unit.
