# jowe-convert

Convert images, audio, video and 3D (USDZ, glTF, OBJ, FBX...) on any OS: a C++17 library, a CLI (`jvconvert`) and an
MCP server (`jvconvert mcp`) so AI agents can use it too.

It is the engine behind the Media › CONVERT feature of JoweVisuals, and it is built for Windows, macOS, Linux
(x86_64 and arm64) and WebAssembly. Everything in the core is permissively licensed (Apache-2.0, BSD, MIT, zlib, CC0).

## Formats

What a machine can write is asked at run time: OS codecs and encoder plugins change the answer. Run
`jvconvert formats` to see the list for your machine, with the reason next to every format it can't write.

| Kind | Format | Read | Write | Notes |
|---|---|---|---|---|
| Image | PNG | yes | yes | stb_image / stb_image_write |
| Image | JPEG | yes | yes | EXIF orientation applied to the pixels; alpha flattened onto white |
| Image | WebP | yes | yes | libwebp, lossy or `--lossless` |
| Image | TIFF | yes | yes | built-in; reads uncompressed / LZW / Deflate / PackBits, 8 and 16 bit; writes Deflate |
| Image | BMP, TGA | yes | yes | stb |
| Image | HEIC | Windows (HEIF + HEVC extensions), macOS | yes (not wasm) | encode: kvazaar + our own HEIF writer |
| Image | GIF, PSD | yes | no | first frame / composite |
| Image | AVIF | Windows (AV1 extension), macOS | no | |
| Audio | WAV, AIFF, CAF | yes | yes | built-in; 16 / 24 / 32-bit (WAV and CAF 32-bit = float) |
| Audio | FLAC | yes | yes | libFLAC, 16 / 24-bit |
| Audio | MP3 | yes | Windows; elsewhere through an encoder plugin | decode: minimp3; encode: Media Foundation or a plugin |
| Audio | M4A (AAC) | Windows, macOS | Windows, macOS | OS codecs only (Media Foundation / AudioToolbox); unsupported on Linux and wasm for now |
| Video | MP4, MOV | yes | yes | remux with our ISO-BMFF muxer; re-encode to H.264 (HEVC where the OS has an encoder) on Windows and Apple |
| Video | M4V | yes | no | write MP4 instead |
| 3D | USDZ | yes | yes | read: TinyUSDZ; write: ours (uncompressed zip, 64-byte aligned, USDA + PNG / JPEG textures, UsdPreviewSurface) |
| 3D | USDA, USD | yes | yes | USD is written as text (USDA) |
| 3D | USDC | yes | no | written on Apple devices only |
| 3D | OBJ (+MTL), glTF, GLB, STL, PLY | yes | yes | Assimp; OBJ writes its `.mtl` and textures next to it |
| 3D | FBX, Collada (DAE), 3DS | yes | no | Assimp |
| 3D | Alembic (ABC) | no | no | Apple devices only |

3D conversions keep the node tree, materials (base colour, normal, roughness, metallic, emissive, opacity, occlusion)
with their textures, and units / up axis: USD keeps `metersPerUnit` and `upAxis`; glTF output is converted to Y-up
metres as the glTF spec requires.

### Units

What is **written** (unchanged since 0.1.0):

| Output | Geometry | Unit / axis metadata |
|---|---|---|
| glTF, GLB | converted to metres, Y-up: scaled by the source's metres-per-unit when the source declares one, rotated when it is Z-up or X-up | none (glTF is metres, Y-up by definition) |
| USD, USDA, USDZ | the source's own numbers | `metersPerUnit` and `upAxis` copied from the source (FBX in centimetres → `metersPerUnit = 0.01`) |
| OBJ, STL, PLY | the source's own numbers (an FBX in centimetres stays in centimetres) | none |

`--scale` and `--up-axis` apply on top. FBX units come from Assimp's `UnitScaleFactor` (centimetres per unit):
metres per unit = `UnitScaleFactor / 100`. OBJ, STL and PLY carry no unit and are taken as metres.

What is **reported** changed in 0.1.1:
3D probe results (and the `input` / `output` probes of a conversion) report bounds in **metres, Y-up** for every
format, so a centimetre FBX, a Z-up USD and a glTF of the same object give the same numbers. What the file itself
declares stays visible as `sourceUpAxis` / `sourceMetersPerUnit` (`source_up_axis` / `source_meters_per_unit` in
JSON and MCP output; `null` when the format has no unit, such as OBJ, STL or PLY, where metres are assumed).

USD written by ModelIO authors UsdPreviewSurface colours (`diffuseColor`, `emissiveColor`, `specularColor`) as
`color4f` / `float4`. TinyUSDZ only accepts `color3f`, so such files are read again with those inputs rewritten to
RGB; the alpha is dropped (opacity comes from `inputs:opacity` as usual).

## Build

Needs CMake 3.25+ and a C++17 compiler. Dependencies are downloaded by CMake (FetchContent) from their official
GitHub release archives, pinned by tag and SHA-256.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

WebAssembly (the tests run under node):

```sh
emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm --parallel
ctest --test-dir build-wasm --output-on-failure
```

Options: `JOWE_CONVERT_BUILD_CLI` (ON), `JOWE_CONVERT_BUILD_TESTS` (ON when top level),
`JOWE_CONVERT_WITH_HEIC_ENCODER` (ON, OFF on wasm), `JOWE_CONVERT_WITH_OS_CODECS` (ON).

Use it from another CMake project:

```cmake
include(FetchContent)
FetchContent_Declare(jowe_convert GIT_REPOSITORY https://github.com/johseffer/jowe-convert.git GIT_TAG v0.1.0)
FetchContent_MakeAvailable(jowe_convert)
target_link_libraries(my_app PRIVATE jowe::convert)
```

### Swift Package Manager (iOS, iPadOS, Mac Catalyst, macOS)

Every CI run builds `jowe-convert-apple.xcframework` (static library + headers + module map) with these slices:
iOS device arm64, iOS simulator arm64 + x86_64, Mac Catalyst arm64 + x86_64 and macOS arm64 + x86_64. It is
uploaded as the `jowe-convert-apple-xcframework` artefact together with its SwiftPM checksum.

The root `Package.swift` declares it as a `binaryTarget`. Its URL and checksum are placeholders until the first
release: for a release `vX.Y.Z`, set `releaseTag = "vX.Y.Z"` and `xcframeworkChecksum` to the value the release
workflow prints (the same as `swift package compute-checksum jowe-convert-apple.xcframework.zip`), then tag.

```swift
.package(url: "https://github.com/johseffer/jowe-convert.git", exact: "0.1.0"),
// ...
.target(name: "MyApp", dependencies: [.product(name: "JoweConvert", package: "jowe-convert")],
        swiftSettings: [.interoperabilityMode(.Cxx)])
```

`import JoweConvert` exposes the C++ API (Swift / C++ interop), `import JoweConvertC` the plugin C ABI. The
`JoweConvert` product also links the system libraries the static library needs (libc++, libz, AudioToolbox,
CoreFoundation, CoreGraphics, ImageIO, VideoToolbox, CoreMedia, CoreVideo).

### Releases

Pushing a `v*` tag runs `.github/workflows/release.yml`: it runs the full CI, then attaches the xcframework zip,
the jvconvert binaries for Windows, macOS, Linux x86_64 / arm64 and wasm, and a `SHA256SUMS` file to the GitHub
release of that tag.

## Library

```cpp
#include <jowe/convert.hpp>
namespace jc = jowe::convert;

for (auto& f : jc::listFormats())            // what this machine can read / write, with reasons
  if (!f.canWrite) printf("%s: %s\n", f.id.c_str(), f.writeReason.c_str());

auto info = jc::probe("chair.usdz");         // size, codec, duration, tracks, vertices...

jc::ConvertOptions opt;
opt.image.quality = 85;
opt.model.upAxis = jc::ModelOptions::UpAxis::ZToY;
opt.progress = [](double p) { /* 0..1, off the calling thread's UI */ };
opt.access = [](const std::filesystem::path& p, bool write) { return true; };  // optional sandbox hook
auto rep = jc::convert("chair.usdz", "chair.glb", opt);
if (!rep.result) printf("%s: %s\n", jc::toString(rep.result.status), rep.result.message.c_str());
```

Errors carry a status (`unreadable`, `not_writable`, `kind_mismatch`, `unsupported`, `invalid_argument`,
`access_denied`, `cancelled`, `failed`) and a message. A failed or cancelled conversion leaves no partial file.

## CLI

```sh
jvconvert formats [--json] [--kind image|audio|video|model]
jvconvert probe chair.usdz --json
jvconvert photo.heic photo.jpg --quality 85 --max-size 2048
jvconvert song.wav song.flac --bit-depth 24 --sample-rate 48000
jvconvert clip.mp4 clip.mov
jvconvert clip.mov small.mp4 --resolution 720 --video-bitrate 2500 --max-fps 30 --audio aac
jvconvert scan.obj scan.usdz --up-axis z-to-y --scale 0.001
jvconvert plugins
```

Exit codes: 0 ok, 1 conversion failed, 2 bad arguments, 4 access denied, 5 cancelled.

## MCP server

`jvconvert mcp` speaks the Model Context Protocol over stdio and offers three tools: `list_formats`, `probe` and
`convert`. It only reads and writes inside the directories given with `--allow` (symlinks are resolved before the
check; relative paths are taken from the first allowed directory). With no `--allow` every file access is refused.
It never opens a network connection.

Claude Code (`.mcp.json` in your project, or `claude mcp add`):

```json
{
  "mcpServers": {
    "jvconvert": {
      "command": "/path/to/jvconvert",
      "args": ["mcp", "--allow", "/Users/me/Media", "--allow", "/Users/me/Exports"]
    }
  }
}
```

```sh
claude mcp add jvconvert -- /path/to/jvconvert mcp --allow "$HOME/Media"
```

## Encoder plugins

Encoders that can't live in the Apache-2.0 core (for example an LGPL MP3 encoder) are separate shared libraries,
loaded at run time and never linked into the core. The plugin ABI is the small C header
[`include/jowe/convert_plugin.h`](include/jowe/convert_plugin.h): one exported function, `jc_encoder_plugins`,
returns a table with `probe`, `negotiate`, `create`, `write_f32`, `finish` and `destroy`. A loaded plugin shows up
in `listFormats()` / `jvconvert formats` with its name and licence as the write backend.

`jvconvert` loads plugins from `JVCONVERT_PLUGIN_PATH` (folders or files), from a `plugins` folder next to the
executable, and from `--plugin <library>`. Library users call `jowe::convert::loadPlugin()` or, where there is no
dynamic loading (wasm), `registerEncoderPlugin()` with a table linked in by the host. `tests/plugin/test_encoder.c`
is a complete minimal plugin.

## Video re-encoding

MP4 / MOV / M4V are remuxed (no quality loss) unless a video option asks for re-encoding:

| Option | CLI | MCP | Values |
|---|---|---|---|
| Codec | `--video-codec` | `video_codec` | `keep` (remux when nothing else changes), `h264`, `hevc` |
| Resolution | `--resolution` | `resolution` | `keep`, `2160`, `1080`, `720`, `480` (short side, never upscales) |
| Bitrate | `--video-bitrate` / `--video-quality` | `video_bitrate_kbps` / `video_quality` | kbit/s, or `low` / `medium` / `high` (0.05 / 0.10 / 0.16 bit per pixel per frame, HEVC 0.6×) |
| Frame rate | `--max-fps` | `max_fps` | frames are dropped down to this rate |
| Soundtrack | `--audio` | `audio` | `keep` (copied with its edit list / AAC priming), `aac` (re-encoded with the OS encoder), `drop` |
| Rotation | `--rotation` | `rotation` | `bake` (default: pixels rotated upright, identity matrix) or `metadata` (source matrix kept); a remux always keeps the matrix |

The library demuxes the source, decodes with the OS decoder, scales (box filter) and drops frames itself, encodes
with the OS encoder (constant bitrate, no B-frames, a keyframe every 2 s) and writes the result with its own
ISO-BMFF muxer, so MOV output works on Windows too. `listFormats()` reports `canTranscode`, the backend, the
encoders (`h264`, `hevc`) and the reason when re-encoding is not available.

| Platform | Backend | H.264 | HEVC |
|---|---|---|---|
| Windows | Media Foundation (synchronous MFTs) | yes | when a synchronous HEVC encoder MFT is installed |
| macOS, iOS, Mac Catalyst | VideoToolbox | yes | where the device has an HEVC encoder |
| Linux | none | unsupported (remux only) | unsupported |
| Android, web | host-supplied | through `registerVideoBackend()` | through `registerVideoBackend()` |

Android (MediaCodec) and the web (WebCodecs, through a JS bridge) plug in with the C table `jc_video_backend` in
[`include/jowe/convert_plugin.h`](include/jowe/convert_plugin.h): decode MP4-style samples to NV12 frames and
encode NV12 frames to MP4-style samples. The calls are synchronous, so a WebCodecs bridge has to wait for its
promises (Asyncify, or a worker with `Atomics.wait`).

## Licences of the dependencies

| Library | Used for | Licence |
|---|---|---|
| [Assimp](https://github.com/assimp/assimp) v6.0.5 | OBJ / glTF / GLB / FBX / DAE / STL / PLY / 3DS read, OBJ / glTF / GLB / STL / PLY write | BSD-3-Clause (bundled zlib: zlib; RapidJSON: MIT; pugixml: MIT; utf8cpp: BSL-1.0; poly2tri: BSD-3; clipper: BSL-1.0; unzip: zlib) |
| [TinyUSDZ](https://github.com/lighttransport/tinyusdz) v0.9.1 | USD / USDA / USDC / USDZ read | Apache-2.0 (bundled LZ4: BSD-2; pugixml: MIT) |
| stb_image, stb_image_write, stb_image_resize2 (shipped inside TinyUSDZ) | PNG / JPEG / BMP / TGA / GIF / PSD, resize | MIT or public domain |
| [libwebp](https://github.com/webmproject/libwebp) v1.6.0 | WebP | BSD-3-Clause |
| [libFLAC](https://github.com/xiph/flac) 1.5.0 | FLAC | BSD-3-Clause |
| [minimp3](https://github.com/lieff/minimp3) (commit `ea99364`, the project has no tags) | MP3 decode | CC0-1.0 |
| [kvazaar](https://github.com/ultravideo/kvazaar) v2.3.2 | HEVC encode for HEIC | BSD-3-Clause |
| OS codecs (Media Foundation, WIC, AudioToolbox, ImageIO, VideoToolbox) | AAC, MP3 encode on Windows, HEIC / AVIF decode, H.264 / HEVC video | part of the OS |

Our own code covers the ISO-BMFF (MP4 / MOV) demuxer and muxer, the HEIF writer, the USDA / USDZ writers, the zip
writer, WAV / AIFF / CAF, the TIFF codec and the resampler. Nothing GPL or LGPL is built into the library.

## Status (v0.1)

- Video re-encoding needs an OS codec: Media Foundation (Windows) or VideoToolbox (macOS / iOS). Linux reports it
  as unsupported; Android and the web need a host backend (see below).
- Audio and images are decoded whole in memory; very long recordings need RAM accordingly.
- HEIC read needs an OS decoder (Windows with the HEIF and HEVC extensions, macOS); Linux and wasm report it as
  unsupported. HEIC images above ~35 megapixels are refused (no grid tiling yet).
- M4A/AAC read and write use OS codecs only; MP3 write needs Media Foundation (Windows) or an encoder plugin.
- Skinned / animated USD is read as its rest pose; animation is not carried over.
- 3D textures the image codecs can't decode (EXR, for example) are dropped with a warning.
- OBJ output writes the base colour, normal, emissive and opacity maps Assimp's OBJ exporter supports; metallic /
  roughness go to USD and glTF only.
- glTF output packs metallic and roughness into one texture (glTF's metallicRoughness layout).

## Licence

Apache-2.0. See [LICENSE](LICENSE).
