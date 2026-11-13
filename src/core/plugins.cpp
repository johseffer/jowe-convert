#include "plugins.hpp"

#include <fstream>
#include <mutex>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <dlfcn.h>
#endif

namespace jowe::convert {

namespace {

struct Registry {
  std::mutex mutex;
  std::vector<detail::PluginEncoder> encoders;
};

Registry& registry() {
  static Registry r;
  return r;
}

Result validate(const jc_encoder_plugin* p) {
  if (!p) return Result::error(Status::InvalidArgument, "null plugin");
  if (p->abi_version != JC_PLUGIN_ABI_VERSION)
    return Result::error(Status::Unsupported, "plugin ABI " + std::to_string(p->abi_version) + " (host is " +
                                                  std::to_string(JC_PLUGIN_ABI_VERSION) + ")");
  if (p->struct_size < sizeof(jc_encoder_plugin)) return Result::error(Status::Unsupported, "plugin table too small");
  if (p->kind != JC_KIND_AUDIO) return Result::error(Status::Unsupported, "only audio encoder plugins are supported");
  if (!p->format_id || !*p->format_id || !p->name || !p->create || !p->write_f32 || !p->finish || !p->destroy)
    return Result::error(Status::InvalidArgument, "plugin table is incomplete");
  return Result::ok();
}

}  // namespace

Result registerEncoderPlugin(const jc_encoder_plugin* plugin, const std::string& source) {
  if (auto r = validate(plugin); !r) return r;
  auto& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  for (auto& e : reg.encoders)
    if (e.plugin == plugin) return Result::ok();
  reg.encoders.push_back({plugin, source});
  return Result::ok();
}

Result loadPlugin(const std::filesystem::path& library, int* added) {
  if (added) *added = 0;
#if defined(__EMSCRIPTEN__)
  (void)library;
  return Result::error(Status::Unsupported,
                       "The wasm build loads encoders with registerEncoderPlugin, not from a library file");
#else
#if defined(_WIN32)
  HMODULE h = LoadLibraryW(library.wstring().c_str());
  if (!h) return Result::error(Status::Unreadable, "Cannot load plugin " + library.string());
  auto fn = reinterpret_cast<jc_encoder_plugins_fn>(reinterpret_cast<void*>(GetProcAddress(h, JC_ENCODER_PLUGINS_SYMBOL)));
#else
  void* h = dlopen(library.string().c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!h) return Result::error(Status::Unreadable, std::string("Cannot load plugin: ") + dlerror());
  auto fn = reinterpret_cast<jc_encoder_plugins_fn>(dlsym(h, JC_ENCODER_PLUGINS_SYMBOL));
#endif
  auto unload = [&] {
#if defined(_WIN32)
    FreeLibrary(h);
#else
    dlclose(h);
#endif
  };
  if (!fn) {
    unload();
    return Result::error(Status::Unsupported, library.string() + " is not a jowe-convert plugin");
  }
  size_t count = 0;
  const jc_encoder_plugin* const* list = fn(JC_PLUGIN_ABI_VERSION, &count);
  if (!list || count == 0) {
    unload();
    return Result::error(Status::Unsupported, library.string() + " offers no encoder for this host ABI");
  }
  int n = 0;
  std::string errors;
  for (size_t i = 0; i < count; ++i) {
    if (auto r = registerEncoderPlugin(list[i], library.string()); r) ++n;
    else errors += (errors.empty() ? "" : "; ") + r.message;
  }
  if (!n) {
    unload();
    return Result::error(Status::Unsupported, errors);
  }
  if (added) *added = n;
  return Result::ok();
#endif
}

int loadPluginsFrom(const std::filesystem::path& dir, std::vector<std::string>* errors) {
  std::error_code ec;
  int total = 0;
  for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    std::string ext = detail::extensionOf(entry.path());
    if (ext != "dll" && ext != "so" && ext != "dylib") continue;
    int n = 0;
    if (auto r = loadPlugin(entry.path(), &n); r) total += n;
    else if (errors) errors->push_back(r.message);
  }
  return total;
}

std::vector<PluginInfo> loadedPlugins() {
  std::vector<PluginInfo> out;
  for (auto& e : detail::allPluginEncoders())
    out.push_back({e.plugin->name, e.plugin->version ? e.plugin->version : "", e.plugin->license ? e.plugin->license : "",
                   e.plugin->format_id, e.source});
  return out;
}

namespace detail {

std::vector<PluginEncoder> allPluginEncoders() {
  auto& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  return reg.encoders;
}

bool pluginUsable(const jc_encoder_plugin* p, std::string* reason) {
  if (!p->probe) return true;
  char buf[256] = {0};
  if (p->probe(buf, sizeof buf)) return true;
  if (reason) *reason = buf[0] ? buf : "the plugin reports it can't run here";
  return false;
}

std::vector<PluginEncoder> pluginEncodersFor(const std::string& formatId) {
  std::vector<PluginEncoder> usable, other;
  for (auto& e : allPluginEncoders()) {
    if (lower(e.plugin->format_id) != formatId) continue;
    (pluginUsable(e.plugin, nullptr) ? usable : other).push_back(e);
  }
  usable.insert(usable.end(), other.begin(), other.end());
  return usable;
}

void pluginNegotiate(const jc_encoder_plugin* p, int& rate, int& channels, int& kbps) {
  if (!p->negotiate) return;
  jc_audio_encoder_config cfg{sizeof(jc_audio_encoder_config), uint32_t(rate), uint32_t(channels), uint32_t(kbps), -1};
  char err[256] = {0};
  if (p->negotiate(&cfg, err, sizeof err) == JC_OK) {
    rate = int(cfg.sample_rate);
    channels = int(cfg.channels);
    kbps = int(cfg.bitrate_kbps);
  }
}

namespace {
struct Sink {
  std::ofstream* f;
};
int32_t sinkWrite(void* user, const uint8_t* data, size_t size) {
  auto* s = static_cast<Sink*>(user);
  s->f->write(reinterpret_cast<const char*>(data), std::streamsize(size));
  return *s->f ? JC_OK : JC_ERR_IO;
}
}  // namespace

Result pluginEncode(const Context& ctx, const jc_encoder_plugin* p, const Pcm& pcm, int kbps, const fs::path& out) {
  if (!ctx.io.canWrite(out)) return Result::error(Status::AccessDenied, "Access denied: " + out.string());
  std::ofstream f(out, std::ios::binary | std::ios::trunc);
  if (!f) return Result::error(Status::Failed, "Cannot create " + out.string());
  Sink sink{&f};
  jc_audio_encoder_config cfg{sizeof(jc_audio_encoder_config), uint32_t(pcm.sampleRate), uint32_t(pcm.channels),
                              uint32_t(kbps), -1};
  char err[512] = {0};
  jc_encoder* enc = p->create(&cfg, sinkWrite, &sink, err, sizeof err);
  if (!enc) return Result::error(Status::Failed, std::string(p->name) + ": " + (err[0] ? err : "create failed"));
  const size_t frames = pcm.frames(), chunk = 8192;
  int32_t rc = JC_OK;
  for (size_t at = 0; at < frames && rc == JC_OK; at += chunk) {
    if (ctx.cancelled()) {
      p->destroy(enc);
      return Result::error(Status::Cancelled, "Cancelled");
    }
    size_t n = std::min(chunk, frames - at);
    rc = p->write_f32(enc, pcm.samples.data() + at * pcm.channels, n, err, sizeof err);
    ctx.progress(0.5 + 0.45 * double(at + n) / double(std::max<size_t>(frames, 1)));
  }
  if (rc == JC_OK) rc = p->finish(enc, err, sizeof err);
  p->destroy(enc);
  f.close();
  if (rc != JC_OK || !f)
    return Result::error(Status::Failed, std::string(p->name) + ": " + (err[0] ? err : "encoding failed"));
  return Result::ok();
}

}  // namespace detail
}  // namespace jowe::convert
