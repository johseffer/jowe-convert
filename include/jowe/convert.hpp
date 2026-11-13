#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct jc_encoder_plugin;

namespace jowe::convert {

enum class MediaKind { Image, Audio, Video, Model };

enum class Status {
  Ok,
  Unreadable,
  NotWritable,
  KindMismatch,
  Unsupported,
  InvalidArgument,
  AccessDenied,
  Cancelled,
  Failed,
};

struct Result {
  Status status = Status::Ok;
  std::string message;

  explicit operator bool() const { return status == Status::Ok; }
  static Result ok() { return {}; }
  static Result error(Status s, std::string msg) { return {s, std::move(msg)}; }
};

struct FormatCapability {
  std::string id;
  std::string name;
  MediaKind kind = MediaKind::Image;
  std::vector<std::string> extensions;
  bool canRead = false;
  bool canWrite = false;
  std::string readBackend;
  std::string writeBackend;
  std::string readReason;
  std::string writeReason;
};

// Asked at run time: OS codecs and loaded plugins change the answer.
std::vector<FormatCapability> listFormats();
std::optional<FormatCapability> findFormat(std::string_view idOrExtension);
std::optional<FormatCapability> formatForPath(const std::filesystem::path& path);

struct TrackInfo {
  std::string type;
  std::string codec;
  double durationSeconds = 0;
  int width = 0;
  int height = 0;
  int sampleRate = 0;
  int channels = 0;
  uint64_t sampleCount = 0;
};

struct ModelStats {
  uint64_t meshes = 0;
  uint64_t vertices = 0;
  uint64_t triangles = 0;
  uint64_t materials = 0;
  uint64_t textures = 0;
  uint64_t nodes = 0;
  std::string upAxis;
  double metersPerUnit = 0;
  float boundsMin[3] = {0, 0, 0};
  float boundsMax[3] = {0, 0, 0};
};

struct ProbeInfo {
  std::string format;
  MediaKind kind = MediaKind::Image;
  uint64_t fileSize = 0;

  int width = 0;
  int height = 0;
  int channels = 0;
  int bitDepth = 0;

  std::string codec;
  double durationSeconds = 0;
  int sampleRate = 0;
  int bitrateKbps = 0;
  std::string brand;
  std::vector<TrackInfo> tracks;

  ModelStats model;
};

// Every file the library opens is passed through this first; returning false refuses it.
using AccessCheck = std::function<bool(const std::filesystem::path& path, bool forWrite)>;

struct ImageOptions {
  int quality = 90;
  int maxSize = 0;
  bool lossless = false;
};

struct AudioOptions {
  int sampleRate = 0;
  int channels = 0;
  int bitDepth = 0;
  int bitrateKbps = 0;
};

struct ModelOptions {
  // Keep honours the source's declared axis; the others rotate the model as asked.
  enum class UpAxis { Keep, ZToY, YToZ };
  UpAxis upAxis = UpAxis::Keep;
  double scale = 1.0;
  bool textures = true;
};

struct ConvertOptions {
  std::string targetFormat;
  ImageOptions image;
  AudioOptions audio;
  ModelOptions model;
  bool overwrite = true;
  AccessCheck access;
  std::function<void(double)> progress;
  const std::atomic<bool>* cancel = nullptr;
};

struct ConvertReport {
  Result result;
  ProbeInfo input;
  ProbeInfo output;
  std::vector<std::filesystem::path> writtenFiles;
  std::vector<std::string> warnings;
};

struct ProbeResult {
  Result result;
  ProbeInfo info;
};

ProbeResult probe(const std::filesystem::path& path, const AccessCheck& access = {});
ConvertReport convert(const std::filesystem::path& input, const std::filesystem::path& output,
                      const ConvertOptions& options = {});

// Encoder plugins (see jowe/convert_plugin.h). Returns the number of encoders registered.
Result loadPlugin(const std::filesystem::path& library, int* encodersAdded = nullptr);
int loadPluginsFrom(const std::filesystem::path& directory, std::vector<std::string>* errors = nullptr);
// For hosts that link an encoder in directly (for example a separate wasm module on the web).
Result registerEncoderPlugin(const jc_encoder_plugin* plugin, const std::string& source);

struct PluginInfo {
  std::string name;
  std::string version;
  std::string license;
  std::string formatId;
  std::string source;
};
std::vector<PluginInfo> loadedPlugins();

const char* version();
const char* toString(Status status);
const char* toString(MediaKind kind);

}  // namespace jowe::convert
