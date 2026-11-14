#include "backend.hpp"

#if !(defined(JOWE_CONVERT_OS_CODECS) &&       (defined(_WIN32) || defined(__APPLE__) || (defined(__linux__) && !defined(__ANDROID__))))

namespace jowe::convert::detail {

const jc_video_backend* builtinVideoBackend() { return nullptr; }

std::string noBuiltinVideoBackendReason() {
#if defined(__EMSCRIPTEN__)
  return "Video re-encoding on the web needs a WebCodecs backend registered by the host (registerVideoBackend)";
#elif defined(__ANDROID__)
  return "Video re-encoding on Android needs a MediaCodec backend registered by the host (registerVideoBackend)";
#elif defined(_WIN32) || defined(__APPLE__)
  return "This build has OS codecs turned off (JOWE_CONVERT_WITH_OS_CODECS=OFF)";
#else
  return "No video encoder on this platform: MP4 / MOV can be remuxed, not re-encoded";
#endif
}

std::vector<std::string> videoBackendNotes() { return {}; }

}  // namespace jowe::convert::detail

#endif
