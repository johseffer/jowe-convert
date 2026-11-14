#include "backend.hpp"

#include <mutex>

namespace jowe::convert {

namespace {

struct VideoRegistry {
  std::mutex mutex;
  std::vector<const jc_video_backend*> backends;
};

VideoRegistry& videoRegistry() {
  static VideoRegistry r;
  return r;
}

}  // namespace

Result registerVideoBackend(const jc_video_backend* b) {
  if (!b) return Result::error(Status::InvalidArgument, "null video backend");
  if (b->abi_version != JC_VIDEO_BACKEND_ABI_VERSION)
    return Result::error(Status::Unsupported, "video backend ABI " + std::to_string(b->abi_version) + " (host is " +
                                                  std::to_string(JC_VIDEO_BACKEND_ABI_VERSION) + ")");
  if (b->struct_size < sizeof(jc_video_backend) || !b->name || !b->can_decode || !b->can_encode ||
      !b->decoder_create || !b->decoder_push || !b->decoder_flush || !b->decoder_destroy || !b->encoder_create ||
      !b->encoder_push || !b->encoder_flush || !b->encoder_config || !b->encoder_destroy)
    return Result::error(Status::InvalidArgument, "video backend table is incomplete");
  auto& reg = videoRegistry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  for (auto* x : reg.backends)
    if (x == b) return Result::ok();
  reg.backends.push_back(b);
  return Result::ok();
}

namespace detail {

std::vector<const jc_video_backend*> videoBackends() {
  std::vector<const jc_video_backend*> out;
  if (auto* b = builtinVideoBackend()) out.push_back(b);
  auto& reg = videoRegistry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  out.insert(out.end(), reg.backends.begin(), reg.backends.end());
  return out;
}

const jc_video_backend* pickVideoBackend(const std::string& dec, const std::string& enc, std::string* reason) {
  auto list = videoBackends();
  if (list.empty()) {
    if (reason) *reason = noBuiltinVideoBackendReason();
    return nullptr;
  }
  std::string last;
  for (auto* b : list) {
    char buf[256] = {0};
    if (!dec.empty() && !b->can_decode(dec.c_str(), buf, sizeof buf)) {
      last = std::string(b->name) + ": " + (buf[0] ? buf : "can't decode " + dec);
      continue;
    }
    buf[0] = 0;
    if (!enc.empty() && !b->can_encode(enc.c_str(), buf, sizeof buf)) {
      last = std::string(b->name) + ": " + (buf[0] ? buf : "can't encode " + enc);
      continue;
    }
    return b;
  }
  if (reason) *reason = last;
  return nullptr;
}

}  // namespace detail
}  // namespace jowe::convert
