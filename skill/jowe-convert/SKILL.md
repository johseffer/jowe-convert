---
name: jowe-convert
description: Convert and inspect media files locally with the jvconvert MCP server (jowe-convert). Use when the user wants to convert images (PNG, JPEG, WebP, TIFF, BMP, TGA, HEIC; read-only GIF, PSD, AVIF), audio (WAV, AIFF, CAF, FLAC, MP3, M4A/AAC), video (MP4/MOV remux, H.264/HEVC re-encode, resize, bitrate, frame rate, drop or re-encode the soundtrack) or 3D models (glTF, GLB, USDZ, USDA, OBJ, STL, PLY; read-only FBX, DAE, 3DS, USDC), to inspect a file (dimensions, codec, sample rate, duration, tracks, vertices, materials, units, up axis), to check which formats this machine can write, or to batch-convert a folder. Examples - "turn model.glb into USDZ for iOS", "convert this .mov to H.264 MP4 at 1080p", "make a 48 kHz WAV from this mp3", "convert these PNGs to WebP".
license: Apache-2.0
---

# jowe-convert

`jvconvert` is a single binary (C++17, Apache-2.0) that converts images, audio, video and 3D models, and
runs as an MCP server over stdio with three tools: `list_formats`, `probe` and `convert`. It works on
files on the user's own machine; see **Privacy** below.

Made by Jowe — jowe.app / jowe.ai. Source: https://github.com/johseffer/jowe-convert

## 1. Install

Pick the release binary for the platform. Every download below is a file of the official v0.3.0
GitHub release; always check its SHA-256 before running it.

| Platform | Release file | SHA-256 |
|---|---|---|
| Windows x64 | `jvconvert-v0.3.0-windows-x64.zip` | `c58f5ab55fdf25bc2ed81c2512fc7b6292481519389148f8ed3145f151861377` |
| macOS arm64 (Apple silicon) | `jvconvert-v0.3.0-macos-arm64.tar.gz` | `af43a2eed876cc10c30ce669957bf7f1520aa9f9c093bf310e1a709e019637e9` |
| Linux x86_64 (glibc 2.43+) | `jvconvert-v0.3.0-linux-x64.tar.gz` | `9d7fa56f04c0fb09e1f132ae1665ccad1b543bc4b9542eb6c8d11acd9603ec0e` |
| Linux arm64 (glibc 2.38+) | `jvconvert-v0.3.0-linux-arm64.tar.gz` | `bc739d2efa02d6bf02844d3909e9513b311b445dba5d4371183495751cce3010` |
| WebAssembly (Node.js CLI) | `jvconvert-v0.3.0-wasm.zip` | `05c920db583a1c91114cddcd771f7be51ed1b0f3c1983951a4c69910f443fc0d` |

Download URL: `https://github.com/johseffer/jowe-convert/releases/download/v0.3.0/<release file>`.
The release also has a `SHA256SUMS` file with the same values.

There is no prebuilt binary for Intel Macs, and the Linux x86_64 build needs a recent glibc
(`ldd --version`; Ubuntu 24.04 and Debian 13 are too old). In those cases build from source (section 1.4).

Before downloading anything, tell the user the URL and where the binary will go, and ask them to confirm.

### 1.1 Helper scripts (recommended)

The scripts in `scripts/` print the URL, the expected SHA-256 and the install folder, ask before
downloading, refuse to install on a checksum mismatch, and print the MCP command to use. If the
`scripts/` folder is not on disk (some marketplaces deliver only the instructions), use the manual
commands in 1.2, or get the scripts from the `skill/jowe-convert/scripts` folder of the GitHub repo.

Windows (PowerShell), installs to `%LOCALAPPDATA%\Programs\jvconvert`:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install.ps1            # add -Dest <folder> to choose
```

macOS / Linux, installs to `~/.local/bin`:

```sh
sh scripts/install.sh                     # or: sh scripts/install.sh /some/bin/dir
```

### 1.2 Manual install

Windows (PowerShell):

```powershell
$f = "jvconvert-v0.3.0-windows-x64.zip"
Invoke-WebRequest "https://github.com/johseffer/jowe-convert/releases/download/v0.3.0/$f" -OutFile $f
(Get-FileHash -Algorithm SHA256 $f).Hash   # must equal C58F5AB55FDF25BC2ED81C2512FC7B6292481519389148F8ED3145F151861377
Expand-Archive $f -DestinationPath "$env:LOCALAPPDATA\Programs\jvconvert"
```

macOS (use `shasum -a 256 -c`) and Linux (use `sha256sum -c`):

```sh
f=jvconvert-v0.3.0-macos-arm64.tar.gz
curl -fLO "https://github.com/johseffer/jowe-convert/releases/download/v0.3.0/$f"
echo "af43a2eed876cc10c30ce669957bf7f1520aa9f9c093bf310e1a709e019637e9  $f" | shasum -a 256 -c -
mkdir -p ~/.local/bin && tar -xzf "$f" -C ~/.local/bin ./jvconvert
~/.local/bin/jvconvert --version
```

The macOS binary is not notarised. Downloaded with `curl` it runs as is; if it was downloaded with a
browser and macOS blocks it, the user can run `xattr -d com.apple.quarantine ~/.local/bin/jvconvert`.

### 1.3 Linux video re-encoding (optional)

Remuxing MP4 and MOV works everywhere. Re-encoding video on Linux needs Cisco's OpenH264 2.6 binary,
which the user downloads from Cisco (Cisco's licence requires that; jowe-convert never ships it).
`scripts/install-openh264.sh` does that download from `ciscobinary.openh264.org` after asking, checks the
SHA-256 and prints the line to add:

```sh
export JOWE_CONVERT_OPENH264=~/.local/lib/libopenh264.so.8
```

OpenH264 gives H.264 only; HEVC on Linux is reported as unavailable. VA-API and V4L2 hardware is
detected but not used yet.

### 1.4 Build from source

Needs CMake 3.25+ and a C++17 compiler. CMake downloads the dependencies from their GitHub release
archives, pinned by SHA-256.

```sh
git clone --branch v0.3.0 https://github.com/johseffer/jowe-convert.git
cmake -S jowe-convert -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

The binary is `build/cli/jvconvert` (`build\cli\Release\jvconvert.exe` with Visual Studio).

### 1.5 Check the install

```sh
jvconvert --version          # jvconvert 0.3.0
jvconvert formats            # what this machine can read and write, with the reason for each "no"
python3 scripts/mcp_selftest.py /path/to/jvconvert
```

`mcp_selftest.py` (Python 3, standard library only) starts the MCP server on a temporary folder, makes
a PNG, a WAV and an OBJ cube, converts them to WebP, JPEG, FLAC, GLB and USDZ, checks the files and
checks that a path outside the folder is refused. It prints `PASS` when everything works.

## 2. Connect it as an MCP server

The server is `jvconvert mcp --allow <dir> [--allow <dir> ...]`. It only reads and writes inside the
`--allow` folders (symlinks are resolved first). With no `--allow`, every file access is refused.
Relative paths in tool calls are taken from the first `--allow` folder. Ask the user which folders to
allow; don't allow their whole home folder unless they ask for it.

**Claude Code**:

```sh
claude mcp add jvconvert -- /path/to/jvconvert mcp --allow "$HOME/Media" --allow "$HOME/Exports"
```

Windows:

```powershell
claude mcp add jvconvert -- "$env:LOCALAPPDATA\Programs\jvconvert\jvconvert.exe" mcp --allow "$env:USERPROFILE\Videos"
```

Or in the project's `.mcp.json`:

```json
{
  "mcpServers": {
    "jvconvert": {
      "command": "/path/to/jvconvert",
      "args": ["mcp", "--allow", "/Users/me/Media"]
    }
  }
}
```

**Claude Desktop**: add the same `mcpServers` entry to `claude_desktop_config.json`
(macOS: `~/Library/Application Support/Claude/`, Windows: `%APPDATA%\Claude\`) and restart the app.
On Windows, JSON needs doubled backslashes:

```json
{
  "mcpServers": {
    "jvconvert": {
      "command": "C:\\Users\\me\\AppData\\Local\\Programs\\jvconvert\\jvconvert.exe",
      "args": ["mcp", "--allow", "C:\\Users\\me\\Videos"]
    }
  }
}
```

On Linux with OpenH264, add `"env": {"JOWE_CONVERT_OPENH264": "/home/me/.local/lib/libopenh264.so.8"}`
to the entry (or `-e JOWE_CONVERT_OPENH264=...` to `claude mcp add`).

**Any other MCP client**: stdio transport, command `jvconvert`, arguments `mcp --allow <dir>`.
Newline-delimited JSON-RPC 2.0; protocol versions `2025-06-18`, `2025-03-26` and `2024-11-05`.

The WebAssembly build runs the CLI under Node.js (`node jvconvert.js photo.png photo.webp`). Use a
native binary for the MCP server: `node jvconvert.js mcp --allow <dir>` refused every folder in our
Windows test.

## 3. Tools

### `list_formats`

| Argument | Type | Notes |
|---|---|---|
| `kind` | `"image"` \| `"audio"` \| `"video"` \| `"model"` | optional filter |

Returns `formats`: one entry per format with `id`, `name`, `kind`, `extensions`, `read`, `write`,
`read_backend` / `write_backend`, and `read_reason` / `write_reason` when it can't. Video formats add
`transcode`: `available`, `backend`, `encoders` (`h264`, `hevc`), `reason`, and on Linux `backends` (one
line per candidate). The answer depends on the machine (OS codecs, OpenH264, plugins), so call it
before promising a conversion.

### `probe`

| Argument | Type | Notes |
|---|---|---|
| `path` | string, required | absolute, or relative to the first `--allow` folder |

Returns `format`, `kind`, `file_size`, then by kind:
image `width`, `height`, `channels`, `bit_depth`, `codec`;
audio `codec`, `sample_rate`, `channels`, `bit_depth`, `duration_seconds`, `bitrate_kbps`;
video `brand`, `duration_seconds`, `width`, `height`, `bitrate_kbps`, `tracks[]` (`type`, `codec`,
`duration_seconds`, `width`/`height` or `sample_rate`/`channels`, `samples`);
model `meshes`, `vertices`, `triangles`, `materials`, `textures`, `nodes`, `source_up_axis`,
`source_meters_per_unit`, `bounds_min` / `bounds_max` (always metres, Y-up).

### `convert`

Converts within one kind (image to image, audio to audio, video to video, model to model). The output
extension picks the format unless `format` is set.

| Argument | Type | Range / values | Applies to |
|---|---|---|---|
| `input` | string, required | | all |
| `output` | string, required | | all |
| `format` | string | a format `id` from `list_formats` | all |
| `overwrite` | boolean | default `true` | all |
| `quality` | integer | 1-100 (default 90) | JPEG, WebP, HEIC |
| `max_size` | integer | long side in px, never upscales | images |
| `lossless` | boolean | | WebP |
| `sample_rate` | integer | Hz, up to 384000 | audio |
| `channels` | integer | 1 mono, 2 stereo (up to 8) | audio |
| `bit_depth` | integer | 16, 24, 32 (FLAC: 16 or 24; 32 is written as 24) | WAV, AIFF, CAF, FLAC |
| `bitrate_kbps` | integer | up to 512 | AAC (M4A), MP3 |
| `up_axis` | `keep` \| `z_to_y` \| `y_to_z` | | 3D |
| `scale` | number | > 0, e.g. 0.01 for cm to m | 3D |
| `textures` | boolean | default `true` | 3D |
| `video_codec` | `keep` \| `h264` \| `hevc` | `keep` remuxes unless another video option needs a re-encode | video |
| `resolution` | `keep` \| `2160` \| `1080` \| `720` \| `480` | caps the short side, never upscales | video |
| `video_bitrate_kbps` | integer | up to 400000 | video |
| `video_quality` | `auto` \| `low` \| `medium` \| `high` | used when no bitrate is given | video |
| `max_fps` | number | 0-240, drops frames | video |
| `audio` | `keep` \| `aac` \| `drop` | soundtrack | video |
| `audio_bitrate_kbps` | integer | up to 512 | video soundtrack |
| `rotation` | `bake` \| `metadata` | on re-encode; default `bake` | video |

Any other argument is refused with `invalid_argument`. The result has `ok`, `status`, `message` (on
error), `input` and `output` (probe results), `files` (every file written: `.gltf` writes a `.bin`, OBJ
writes its `.mtl` and textures) and `warnings` (for example the encoder settings, or textures that were
dropped).

## 4. How to work

1. Call `list_formats` (with `kind`) when you are unsure the machine can write the target.
2. `probe` the input when the request depends on what it is (codec, size, units, up axis).
3. Write to a new file inside an allowed folder. Never overwrite the user's source file; pass
   `"overwrite": false` when the output name might already exist.
4. Report what changed using the returned `input` / `output` probes and any `warnings`.
5. Batch: there is no batch tool. List the folder with your own file tools, then call `convert` once per
   file, and give the user a summary of successes and failures. Large videos take time; do them one at a
   time.

Plain CLI (same options with dashes, e.g. `--max-size`, `--video-codec`; `--to` instead of `format`;
`--no-textures`, `--no-overwrite`):

```sh
jvconvert photo.png photo.webp --quality 80
jvconvert probe clip.mp4 --json
jvconvert formats --kind video
for f in *.png; do jvconvert "$f" "${f%.png}.webp" --quality 85; done
```

CLI exit codes: 0 ok, 1 conversion failed, 2 bad arguments, 4 access denied, 5 cancelled. The CLI has
no `--allow` sandbox: it reads and writes the paths it is given.

## 5. Worked examples

All of these ran on Windows 11 with the v0.3.0 release binary, except where noted.

**"Turn model.glb into USDZ for iOS"**

```json
{"name": "convert", "arguments": {"input": "model.glb", "output": "model.usdz"}}
```

The USDZ is an uncompressed, 64-byte-aligned zip holding a USDA layer and PNG / JPEG textures with
UsdPreviewSurface materials; units and up axis are kept. Animation and skinning are not carried over.
We checked the archive layout and re-read it, but did not open it in AR Quick Look in this test.
If the source is Z-up (many CAD and scan exports), add `"up_axis": "z_to_y"`; if it is in millimetres,
add `"scale": 0.001`.

**"Convert this .mov to .mp4, H.264, 1080p"**

```json
{"name": "convert", "arguments": {"input": "clip.mov", "output": "clip.mp4",
  "video_codec": "h264", "resolution": "1080"}}
```

Works when the MOV holds H.264 or HEVC video. ProRes and other codecs can't be re-encoded ("Re-encoding
needs H.264 or HEVC input"). `resolution` caps the short side and never upscales, so a 720p source stays
720p. Needs Media Foundation (Windows), VideoToolbox (macOS) or OpenH264 (Linux). If the user only
wants a different container and the MOV is already H.264, a plain remux is lossless and instant:
`{"input": "clip.mov", "output": "clip.mp4"}`.

**"Make a 48 kHz WAV from an mp3"**

```json
{"name": "convert", "arguments": {"input": "song.mp3", "output": "song.wav", "sample_rate": 48000}}
```

Add `"bit_depth": 24` for 24-bit PCM. MP3 decoding is built in on every platform.

**Other requests it handles**

- PNG to WebP: `{"input": "a.png", "output": "a.webp", "quality": 80}` (or `"lossless": true`).
- Shrink photos for the web: `{"input": "IMG_1.heic", "output": "IMG_1.jpg", "quality": 85, "max_size": 2048}`.
  HEIC input needs macOS or Windows with the HEIF and HEVC extensions.
- WAV to FLAC: `{"input": "take.wav", "output": "take.flac", "bit_depth": 24}`.
- Smaller video: `{"input": "clip.mov", "output": "small.mp4", "resolution": "720", "video_quality": "medium", "max_fps": 30}`.
- Mute a video: `{"input": "clip.mp4", "output": "muted.mp4", "audio": "drop"}` (remux, no quality loss).
- Scan in millimetres, Z-up, to glTF: `{"input": "scan.obj", "output": "scan.glb", "up_axis": "z_to_y", "scale": 0.001}`.
- FBX to GLB or USDZ: FBX is read; units come from the file.

**Requests it can't do (say so and suggest the closest thing)**

- Extract audio from a video, or make a GIF from a video: conversions stay within one kind
  (`kind_mismatch`).
- Write GIF, PSD, AVIF, M4V, FBX, DAE, 3DS: read only. Write PNG / WebP, MP4, GLB / USDZ instead.
- MKV, WebM, AVI, OGG, Opus, EXR: not supported.
- USDC output: Apple devices only; write USDZ or USDA.
- MP3 output outside Windows needs an encoder plugin; M4A (AAC) needs Windows or macOS.

## 6. Limits and errors

Errors come back with `isError: true` and a `status`:

| Status | Meaning | What to do |
|---|---|---|
| `access_denied` | path outside the `--allow` folders | ask the user to add the folder to the server's `--allow` list |
| `unreadable` | file missing or not decodable | check the path; `probe` it |
| `not_writable` | this format can't be written here | pick another format from `list_formats` |
| `kind_mismatch` | e.g. audio to image | not possible |
| `unsupported` | missing codec / encoder (reason in `message`) | follow the reason (OS extension, OpenH264, plugin) |
| `invalid_argument` | bad or unknown option | fix the argument (see section 3) |
| `cancelled`, `failed` | conversion stopped or failed | report `message` |

A failed conversion leaves no partial file. Other limits:

- Images and audio are decoded whole in memory; very long recordings need RAM accordingly.
- HEIC above about 35 megapixels is refused; HEIC read needs an OS decoder (Windows extensions or macOS).
- Video re-encoding is constant bitrate, no B-frames, a keyframe every 2 s. A re-encode stops if the
  output grows past four times the source's bitrate budget.
- 3D: skinned or animated USD is read at its rest pose; textures the image codecs can't read (EXR) are
  dropped with a warning; OBJ output has no metallic / roughness maps; glTF output is metres, Y-up.

## 7. Privacy

Local only, no network calls: jvconvert has no networking code (we checked the CLI, the MCP server and
the library sources for v0.3.0). It reads and writes local files only, and in MCP mode only inside the
`--allow` folders. What it does load at run time, all from the local machine:

- encoder plugins (shared libraries) from `JVCONVERT_PLUGIN_PATH` and from a `plugins` folder next to
  the binary, also in MCP mode, so only put trusted libraries there;
- on Linux, Cisco's `libopenh264.so.8` (from `JOWE_CONVERT_OPENH264` or the system path) and `libva`;
- the operating system's own codecs (Media Foundation and WIC on Windows; ImageIO, AudioToolbox and
  VideoToolbox on Apple).

The network is only used when the user runs the install scripts or the manual download commands above,
which fetch the pinned GitHub release file (and, if chosen, Cisco's OpenH264) and check its SHA-256.
Tool results include the absolute paths of the files involved.

## 8. Licence and credits

jowe-convert is Apache-2.0 (see `LICENSE`). This skill ships only text and scripts, no third-party
code. The binary itself contains Assimp (BSD-3-Clause), TinyUSDZ (Apache-2.0), stb (MIT / public domain),
libwebp (BSD-3-Clause), libFLAC (BSD-3-Clause), minimp3 (CC0), kvazaar (BSD-3-Clause) and OpenH264's API
headers (BSD-2-Clause); the full list is in the project README. Cisco's OpenH264 binary is under Cisco's
own licence.

Made by Jowe — jowe.app / jowe.ai
