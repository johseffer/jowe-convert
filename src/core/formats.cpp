#include "../audio/audio.hpp"
#include "../image/image.hpp"
#include "plugins.hpp"
#include "../video/transcode.hpp"

namespace jowe::convert {

using detail::lower;

namespace {

FormatCapability make(const char* id, const char* name, MediaKind kind, std::vector<std::string> ext,
                      const char* readBackend, const char* writeBackend, const char* writeReason = "") {
  FormatCapability f;
  f.id = id;
  f.name = name;
  f.kind = kind;
  f.extensions = std::move(ext);
  f.canRead = readBackend && *readBackend;
  f.readBackend = readBackend ? readBackend : "";
  f.canWrite = writeBackend && *writeBackend;
  f.writeBackend = writeBackend ? writeBackend : "";
  if (!f.canWrite) f.writeReason = writeReason;
  return f;
}

void audioEncoder(FormatCapability& f, const char* noneReason) {
  std::string reason;
  if (detail::osAudioCanEncode(f.id, &reason)) {
    f.canWrite = true;
#if defined(_WIN32)
    f.writeBackend = "Media Foundation";
#else
    f.writeBackend = "AudioToolbox";
#endif
    return;
  }
  for (auto& e : detail::pluginEncodersFor(f.id)) {
    std::string why;
    if (detail::pluginUsable(e.plugin, &why)) {
      f.canWrite = true;
      f.writeBackend = std::string("plugin: ") + e.plugin->name + " (" + (e.plugin->license ? e.plugin->license : "?") + ")";
      f.writeReason.clear();
      return;
    }
    reason = std::string(e.plugin->name) + ": " + why;
  }
  f.canWrite = false;
  f.writeReason = reason.empty() || std::string(noneReason).size() ? noneReason : reason;
}

}  // namespace

std::vector<FormatCapability> listFormats() {
  const auto I = MediaKind::Image, A = MediaKind::Audio, V = MediaKind::Video, M = MediaKind::Model;
  std::vector<FormatCapability> v;
  v.push_back(make("png", "PNG", I, {"png"}, "stb_image", "stb_image_write"));
  v.push_back(make("jpeg", "JPEG", I, {"jpg", "jpeg"}, "stb_image", "stb_image_write"));
  v.push_back(make("webp", "WebP", I, {"webp"}, "libwebp", "libwebp"));
  v.push_back(make("tiff", "TIFF", I, {"tif", "tiff"}, "built-in", "built-in (Deflate)"));
  v.push_back(make("bmp", "BMP", I, {"bmp"}, "stb_image", "stb_image_write"));
  v.push_back(make("tga", "TGA", I, {"tga"}, "stb_image", "stb_image_write"));
  {
    auto f = make("heic", "HEIC", I, {"heic", "heif"}, nullptr, nullptr);
    std::string reason;
    f.canRead = detail::osImageCanDecode("heic", &reason);
#if defined(_WIN32)
    f.readBackend = f.canRead ? "WIC (HEIF Image Extensions)" : "";
#elif defined(__APPLE__)
    f.readBackend = f.canRead ? "ImageIO" : "";
#endif
    if (!f.canRead) f.readReason = reason;
    reason.clear();
    f.canWrite = detail::heicEncoderAvailable(&reason);
    f.writeBackend = f.canWrite ? "kvazaar + built-in HEIF writer" : "";
    f.writeReason = f.canWrite ? "" : reason;
    v.push_back(f);
  }
  v.push_back(make("gif", "GIF", I, {"gif"}, "stb_image (first frame)", nullptr, "GIF is read only (first frame)"));
  v.push_back(make("psd", "PSD", I, {"psd"}, "stb_image (composite)", nullptr, "PSD is read only"));
  {
    auto f = make("avif", "AVIF", I, {"avif"}, nullptr, nullptr, "AVIF is read only");
    std::string reason;
    f.canRead = detail::osImageCanDecode("avif", &reason);
    f.readBackend = f.canRead ? "OS decoder" : "";
    if (!f.canRead) f.readReason = reason;
    v.push_back(f);
  }

  v.push_back(make("wav", "WAV", A, {"wav", "wave"}, "built-in", "built-in"));
  v.push_back(make("aiff", "AIFF", A, {"aif", "aiff", "aifc"}, "built-in", "built-in"));
  v.push_back(make("caf", "CAF", A, {"caf"}, "built-in (PCM)", "built-in"));
  v.push_back(make("flac", "FLAC", A, {"flac"}, "libFLAC", "libFLAC"));
  {
    auto f = make("mp3", "MP3", A, {"mp3"}, "minimp3", nullptr);
    audioEncoder(f, "MP3 can be read, not written, on this device (no MP3 encoder plugin is installed)");
    v.push_back(f);
  }
  {
    auto f = make("m4a", "M4A (AAC)", A, {"m4a"}, nullptr, nullptr);
    std::string reason;
    f.canRead = detail::osAudioCanDecode("m4a", &reason);
    if (f.canRead) {
#if defined(_WIN32)
      f.readBackend = "Media Foundation";
#else
      f.readBackend = "AudioToolbox";
#endif
    } else {
      f.readReason = reason;
    }
    audioEncoder(f, "");
    if (!f.canWrite && f.writeReason.empty()) detail::osAudioCanEncode("m4a", &f.writeReason);
    v.push_back(f);
  }

  {
    const auto cap = detail::videoTranscodeCapability();
    for (auto f : {make("mp4", "MP4", V, {"mp4"}, "built-in demuxer", "built-in muxer"),
                   make("mov", "MOV", V, {"mov", "qt"}, "built-in demuxer", "built-in muxer"),
                   make("m4v", "M4V", V, {"m4v"}, "built-in demuxer", nullptr, "M4V is read only: write MP4")}) {
      f.canTranscode = cap.available;
      f.transcodeBackend = cap.backend;
      f.transcodeReason = cap.available ? "" : cap.reason;
      if (f.canWrite) f.videoEncoders = cap.encoders;
      v.push_back(f);
    }
  }

  v.push_back(make("usdz", "USDZ", M, {"usdz"}, "TinyUSDZ", "built-in USDZ writer"));
  v.push_back(make("usda", "USDA", M, {"usda"}, "TinyUSDZ", "built-in USDA writer"));
  v.push_back(make("usdc", "USDC", M, {"usdc"}, "TinyUSDZ", nullptr,
                   "USDC is written on Apple devices only: write USDA or USDZ"));
  v.push_back(make("usd", "USD", M, {"usd"}, "TinyUSDZ", "built-in USDA writer (text)"));
  v.push_back(make("obj", "OBJ", M, {"obj"}, "Assimp", "Assimp"));
  v.push_back(make("gltf", "glTF", M, {"gltf"}, "Assimp", "Assimp"));
  v.push_back(make("glb", "GLB", M, {"glb"}, "Assimp", "Assimp"));
  v.push_back(make("stl", "STL", M, {"stl"}, "Assimp", "Assimp (binary)"));
  v.push_back(make("ply", "PLY", M, {"ply"}, "Assimp", "Assimp (binary)"));
  v.push_back(make("fbx", "FBX", M, {"fbx"}, "Assimp", nullptr, "FBX can be read, not written"));
  v.push_back(make("dae", "Collada", M, {"dae"}, "Assimp", nullptr, "Collada can be read, not written"));
  v.push_back(make("3ds", "3DS", M, {"3ds"}, "Assimp", nullptr, "3DS can be read, not written"));
  {
    auto f = make("abc", "Alembic", M, {"abc"}, nullptr, nullptr,
                  "Alembic is read and written on Apple devices only");
    f.readReason = f.writeReason;
    v.push_back(f);
  }

  for (auto& e : detail::allPluginEncoders()) {
    std::string id = lower(e.plugin->format_id);
    bool known = false;
    for (auto& f : v) known = known || f.id == id;
    if (known) continue;
    FormatCapability f;
    f.id = id;
    f.name = e.plugin->name;
    f.kind = MediaKind::Audio;
    std::string exts = e.plugin->extensions ? e.plugin->extensions : id.c_str();
    size_t start = 0;
    while (start <= exts.size()) {
      size_t comma = exts.find(',', start);
      std::string x = lower(exts.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
      if (!x.empty()) f.extensions.push_back(x);
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    f.readReason = "Read is not provided by the plugin";
    audioEncoder(f, "");
    v.push_back(f);
  }
  return v;
}

std::optional<FormatCapability> findFormat(std::string_view idOrExt) {
  std::string key = lower(idOrExt);
  if (!key.empty() && key[0] == '.') key.erase(0, 1);
  if (key == "jpg") key = "jpeg";
  for (auto& f : listFormats()) {
    if (f.id == key) return f;
    for (auto& e : f.extensions)
      if (e == key) return f;
  }
  return std::nullopt;
}

std::optional<FormatCapability> formatForPath(const std::filesystem::path& path) {
  std::string ext = detail::extensionOf(path);
  if (ext.empty()) return std::nullopt;
  return findFormat(ext);
}

const char* version() { return JOWE_CONVERT_VERSION; }

const char* toString(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::Unreadable: return "unreadable";
    case Status::NotWritable: return "not_writable";
    case Status::KindMismatch: return "kind_mismatch";
    case Status::Unsupported: return "unsupported";
    case Status::InvalidArgument: return "invalid_argument";
    case Status::AccessDenied: return "access_denied";
    case Status::Cancelled: return "cancelled";
    case Status::Failed: return "failed";
  }
  return "failed";
}

const char* toString(MediaKind k) {
  switch (k) {
    case MediaKind::Image: return "image";
    case MediaKind::Audio: return "audio";
    case MediaKind::Video: return "video";
    case MediaKind::Model: return "model";
  }
  return "image";
}

}  // namespace jowe::convert
