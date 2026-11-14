# Changelog

## 0.2.1

- **HEVC / H.264 with B-frames from AVFoundation:** composition offsets (`ctts`) are read as signed in both box
  versions. A version-0 box holding negative offsets used to be clamped to about +2^31 ticks, which threw frame times
  hours into the future; with constant-bitrate encoding the re-encode then padded that gap (a 1.5 s clip grew to 9 GB).
- **Re-encode guard:** a re-encode stops with an error when its output passes four times the bitrate budget of the
  source's duration (plus 16 MB), or when a frame's time lies more than 5 s outside the source's duration.
- **Audio edit lists:** with `audio: keep` the source soundtrack keeps its edit list (AAC priming), so it no longer
  starts about 46 ms late. With `audio: aac` the OS encoder's own edit list is kept, rescaled to the output movie.
- **Rotation:** re-encoding now rotates the pixels upright by default (`VideoOptions::bakeRotation`, CLI
  `--rotation bake|metadata`, MCP `rotation`). `metadata` keeps the source's track matrix instead. A remux always keeps
  the matrix.
- **Version:** `jowe::convert::version()` and `jvconvert --version` come from the `project(... VERSION ...)` line in
  `CMakeLists.txt` (now 0.2.1); the release workflow refuses a tag that doesn't match it.
- Encoders that reorder frames: decode times are rebuilt from presentation times, so reordered output is muxed in
  the right order.

## 0.2.0

- Video re-encoding with Media Foundation (Windows) and VideoToolbox (Apple), and the `jc_video_backend` table for
  host backends (Android, web).

## 0.1.1

- ModelIO USD files with `color4f` colour inputs are read.
- **3D probe results report bounds in metres, Y-up for every format** (`ModelStats::boundsMin/Max`), with the
  file's own declaration in `sourceUpAxis` / `sourceMetersPerUnit`. This is a change in what is *reported*, not in
  what is *written*; see "Units" in the README for the exact rule.
- glTF opacity is no longer multiplied twice.

## 0.1.0

- First release: library, `jvconvert` CLI, MCP server.
