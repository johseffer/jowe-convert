# Agensi listing: jowe-convert

Upload file: `skill/dist/jowe-convert-skill-v0.3.1.zip` (SKILL.md, references/formats.md, scripts/, LICENSE).

Suggested price: **Free**. The skill and the binary are Apache-2.0 and public on GitHub, so a paid
listing would compete with the free copy (and Agensi rejects repackaged free skills). The goal is
brand visibility for Jowe.

## English

**Title:** jowe-convert: local media and 3D converter (MCP)

**Pitch:** Convert images, audio, video and 3D models (GLB, USDZ, OBJ) on your own machine, from any agent.

**Description:**

jowe-convert gives your agent a local converter for images, audio, video and 3D through the open-source
`jvconvert` MCP server (three tools: `list_formats`, `probe`, `convert`).

- Images: PNG, JPEG, WebP, TIFF, BMP, TGA, HEIC (read GIF, PSD, AVIF), with quality and max size.
- Audio: WAV, AIFF, CAF, FLAC, MP3, M4A/AAC, with sample rate, channels and bit depth.
- Video: lossless MP4/MOV remux; H.264/HEVC re-encode, resolution caps, bitrate, frame rate, mute or
  AAC soundtrack (Windows and macOS codecs; Linux H.264 with Cisco's OpenH264).
- 3D: glTF, GLB, USDZ, USDA, OBJ, STL, PLY (read FBX, DAE, 3DS, USDC), keeping materials, textures,
  units and up axis. GLB to USDZ for iOS in one call.
- Inspect any supported file: dimensions, codec, duration, tracks, vertices, materials, bounds.

The skill covers install on Windows, macOS (Apple silicon) and Linux from the pinned GitHub release
with SHA-256 checks, MCP setup for Claude Code, Claude Desktop and other clients, every tool argument,
worked examples, limits and error handling.

Privacy: files never leave your machine. jvconvert makes no network calls, and the MCP server only
touches the folders you allow with `--allow`. The install scripts download only the pinned release
file, after showing the URL and asking you.

Apache-2.0. Made by Jowe — jowe.app / jowe.ai

**Tags:** media-conversion, image-converter, audio-converter, video, 3d, usdz, gltf, glb, mcp, offline

## Português (Brasil)

**Título:** jowe-convert: conversor local de mídia e 3D (MCP)

**Chamada:** Converta imagens, áudio, vídeo e modelos 3D (GLB, USDZ, OBJ) na sua própria máquina, por qualquer agente.

**Descrição:**

O jowe-convert dá ao seu agente um conversor local de imagens, áudio, vídeo e 3D por meio do servidor
MCP open source `jvconvert` (três ferramentas: `list_formats`, `probe` e `convert`).

- Imagens: PNG, JPEG, WebP, TIFF, BMP, TGA, HEIC (lê GIF, PSD, AVIF), com qualidade e tamanho máximo.
- Áudio: WAV, AIFF, CAF, FLAC, MP3, M4A/AAC, com taxa de amostragem, canais e profundidade de bits.
- Vídeo: remux MP4/MOV sem perda; recodificação H.264/HEVC, limite de resolução, bitrate, taxa de
  quadros, sem áudio ou áudio AAC (codecs do Windows e do macOS; no Linux, H.264 com o OpenH264 da Cisco).
- 3D: glTF, GLB, USDZ, USDA, OBJ, STL, PLY (lê FBX, DAE, 3DS, USDC), mantendo materiais, texturas,
  unidades e eixo vertical. GLB para USDZ para iOS numa chamada só.
- Inspeção de qualquer arquivo suportado: dimensões, codec, duração, faixas, vértices, materiais, limites.

A skill cobre a instalação no Windows, no macOS (Apple silicon) e no Linux a partir da release fixada no
GitHub, com verificação SHA-256, a configuração do MCP no Claude Code, no Claude Desktop e em outros
clientes, todos os argumentos das ferramentas, exemplos práticos, limites e tratamento de erros.

Privacidade: seus arquivos não saem da sua máquina. O jvconvert não faz chamadas de rede, e o servidor
MCP só mexe nas pastas que você liberar com `--allow`. Os scripts de instalação baixam só o arquivo
fixado da release, depois de mostrar a URL e pedir sua confirmação.

Apache-2.0. Feito pela Jowe — jowe.app / jowe.ai

**Tags:** conversao-de-midia, conversor-de-imagens, conversor-de-audio, video, 3d, usdz, gltf, glb, mcp, offline
