# jowe-convert v0.3.0 format matrix

Any format that can be read converts to any format of the same kind that can be written (image to
image, audio to audio, video to video, 3D to 3D). What a machine can write is decided at run time:
`jvconvert formats` (or the `list_formats` tool) gives the real answer, with a reason for every "no".

R = read, W = write, - = no. "OS" means the operating system's codec is used.

## Images

| Format | Windows x64 | macOS arm64 | Linux x64 / arm64 | wasm (Node) | Notes |
|---|---|---|---|---|---|
| PNG | R W | R W | R W | R W | |
| JPEG | R W | R W | R W | R W | EXIF orientation applied; alpha flattened onto white |
| WebP | R W | R W | R W | R W | lossy or lossless |
| TIFF | R W | R W | R W | R W | reads uncompressed / LZW / Deflate / PackBits, 8 and 16 bit; writes Deflate |
| BMP, TGA | R W | R W | R W | R W | |
| HEIC / HEIF | R (OS: HEIF + HEVC extensions) W | R (OS) W | W only | - | write: kvazaar + built-in HEIF writer; over ~35 MP refused |
| GIF | R | R | R | R | first frame |
| PSD | R | R | R | R | composite |
| AVIF | R (OS: AV1 extension) | R (OS) | - | - | |

## Audio

| Format | Windows x64 | macOS arm64 | Linux x64 / arm64 | wasm (Node) | Notes |
|---|---|---|---|---|---|
| WAV, AIFF, CAF | R W | R W | R W | R W | 16 / 24 / 32-bit (WAV and CAF 32-bit = float) |
| FLAC | R W | R W | R W | R W | 16 / 24-bit |
| MP3 | R W (OS: Media Foundation) | R, W with a plugin | R, W with a plugin | R | decode built in (minimp3) |
| M4A (AAC) | R W (OS: Media Foundation) | R W (OS: AudioToolbox) | - | - | |

## Video

| Format | Windows x64 | macOS arm64 | Linux x64 / arm64 | wasm (Node) |
|---|---|---|---|---|
| MP4 | R W | R W | R W | R W |
| MOV | R W | R W | R W | R W |
| M4V | R | R | R | R |

Remux (MP4 to MOV and back, dropping the soundtrack) is built in everywhere and loses nothing.
Re-encoding (codec, resolution, bitrate, frame rate, AAC soundtrack, rotation) needs H.264 or HEVC
input and an encoder:

| Platform | Backend | H.264 out | HEVC out | AAC soundtrack |
|---|---|---|---|---|
| Windows x64 | Media Foundation (OS) | yes | when an HEVC encoder MFT is installed | yes |
| macOS arm64 | VideoToolbox (OS) | yes | where the Mac has an HEVC encoder | yes |
| Linux x64 / arm64 | Cisco OpenH264 2.6, downloaded by the user (`JOWE_CONVERT_OPENH264`) | yes | no | no (AAC needs an OS encoder; use `audio: keep` or `drop`) |
| wasm | none built in (a host can register one) | no | no | no |

VA-API and V4L2 devices on Linux are detected and listed but not used yet.

## 3D

| Format | Windows x64 | macOS arm64 | Linux x64 / arm64 | wasm (Node) | Notes |
|---|---|---|---|---|---|
| USDZ | R W | R W | R W | R W | write: uncompressed zip, 64-byte aligned, USDA + PNG / JPEG textures |
| USDA, USD | R W | R W | R W | R W | `.usd` is written as text |
| USDC | R | R W (Apple) | R | R | |
| glTF, GLB | R W | R W | R W | R W | output is metres, Y-up |
| OBJ (+MTL) | R W | R W | R W | R W | writes `.mtl` and textures next to it |
| STL, PLY | R W | R W | R W | R W | binary |
| FBX, DAE, 3DS | R | R | R | R | |
| Alembic (ABC) | - | Apple only | - | - | |

Materials keep base colour, normal, roughness, metallic, emissive, opacity and occlusion with their
textures (OBJ output: base colour, normal, emissive and opacity only). USD keeps `metersPerUnit` and
`upAxis`. Animation and skinning are not carried over.

## Not supported

MKV, WebM, AVI, FLV, Ogg files (Vorbis / Opus), EXR, RAW camera files, SVG, PDF, Blender `.blend`,
video to audio or video to GIF.

## Where this came from

The project README and CHANGELOG for v0.3.0, the source of `cli/` and `src/`, and `jvconvert formats`
on Windows 11 x64. The Windows column was run; the other columns follow the README and the source.
