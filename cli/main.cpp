#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "shared.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;
using namespace jvc;

namespace {

const char* kUsage =
    "jvconvert %s - convert images, audio, video and 3D\n"
    "\n"
    "Usage:\n"
    "  jvconvert convert <input> <output> [options]\n"
    "  jvconvert <input> <output> [options]\n"
    "  jvconvert probe <file> [--json]\n"
    "  jvconvert formats [--json] [--kind image|audio|video|model]\n"
    "  jvconvert plugins [--json]\n"
    "  jvconvert mcp --allow <dir> [--allow <dir> ...]\n"
    "\n"
    "Options:\n"
    "  --to <format>          output format (default: from the output extension)\n"
    "  --quality <1-100>      JPEG / WebP / HEIC quality (default 90)\n"
    "  --max-size <px>        image long side, never upscales\n"
    "  --lossless             lossless WebP\n"
    "  --sample-rate <hz>     audio sample rate\n"
    "  --channels <n>         audio channels (1 = mono, 2 = stereo)\n"
    "  --bit-depth <16|24|32> PCM / FLAC bit depth\n"
    "  --bitrate <kbps>       AAC / MP3 bitrate\n"
    "  --up-axis <keep|z-to-y|y-to-z>\n"
    "  --scale <factor>       3D scale (e.g. 0.01 for cm to m)\n"
    "  --video-codec <keep|h264|hevc>  re-encode video (keep = remux unless another video option needs it)\n"
    "  --resolution <keep|2160|1080|720|480>  cap the video short side, never upscaling\n"
    "  --video-bitrate <kbps> | --video-quality <auto|low|medium|high>\n"
    "  --max-fps <fps>        drop frames down to this rate\n"
    "  --audio <keep|aac|drop>  video soundtrack: copy, re-encode to AAC or remove\n"
    "  --audio-bitrate <kbps>\n"
    "  --no-textures          drop 3D textures\n"
    "  --no-overwrite         fail if the output exists\n"
    "  --plugin <library>     load an encoder plugin\n"
    "  --json                 machine-readable output\n"
    "\n"
    "Encoder plugins are also loaded from JVCONVERT_PLUGIN_PATH and from the 'plugins' folder next to jvconvert.\n";

void usage() { std::printf(kUsage, jc::version()); }

fs::path exeDir() {
#if defined(_WIN32)
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return n ? fs::path(std::wstring(buf, n)).parent_path() : fs::path();
#else
  std::error_code ec;
  auto p = fs::read_symlink("/proc/self/exe", ec);
  return ec ? fs::path() : p.parent_path();
#endif
}

void loadDefaultPlugins(std::vector<std::string>& errors) {
  if (const char* env = std::getenv("JVCONVERT_PLUGIN_PATH")) {
#if defined(_WIN32)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string s = env;
    size_t start = 0;
    while (start <= s.size()) {
      size_t end = s.find(sep, start);
      std::string part = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (!part.empty()) {
        std::error_code ec;
        if (fs::is_directory(part, ec)) jc::loadPluginsFrom(part, &errors);
        else if (auto r = jc::loadPlugin(part); !r) errors.push_back(r.message);
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  fs::path dir = exeDir() / "plugins";
  std::error_code ec;
  if (!dir.empty() && fs::is_directory(dir, ec)) jc::loadPluginsFrom(dir, &errors);
}

std::string human(uint64_t bytes) {
  char buf[32];
  if (bytes >= (1u << 20)) std::snprintf(buf, sizeof buf, "%.1f MB", double(bytes) / (1 << 20));
  else if (bytes >= 1024) std::snprintf(buf, sizeof buf, "%.0f KB", double(bytes) / 1024);
  else std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
  return buf;
}

std::string summary(const jc::ProbeInfo& p) {
  std::string s = p.format;
  for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  char buf[256];
  switch (p.kind) {
    case jc::MediaKind::Image:
      std::snprintf(buf, sizeof buf, " · %d × %d", p.width, p.height);
      s += buf;
      break;
    case jc::MediaKind::Audio:
      std::snprintf(buf, sizeof buf, " · %.2f s · %d Hz · %d ch · %s", p.durationSeconds, p.sampleRate, p.channels,
                    p.codec.c_str());
      s += buf;
      break;
    case jc::MediaKind::Video:
      std::snprintf(buf, sizeof buf, " · %.2f s · %d × %d · %zu tracks", p.durationSeconds, p.width, p.height,
                    p.tracks.size());
      s += buf;
      break;
    case jc::MediaKind::Model:
      std::snprintf(buf, sizeof buf, " · %llu vertices · %llu triangles · %llu materials · %llu textures",
                    static_cast<unsigned long long>(p.model.vertices), static_cast<unsigned long long>(p.model.triangles),
                    static_cast<unsigned long long>(p.model.materials), static_cast<unsigned long long>(p.model.textures));
      s += buf;
      break;
  }
  return s + " · " + human(p.fileSize);
}

int exitCode(jc::Status s) {
  switch (s) {
    case jc::Status::Ok: return 0;
    case jc::Status::InvalidArgument: return 2;
    case jc::Status::AccessDenied: return 4;
    case jc::Status::Cancelled: return 5;
    default: return 1;
  }
}

std::vector<std::string> utf8Args(int argc, char** argv) {
#if defined(_WIN32)
  (void)argc;
  (void)argv;
  int n = 0;
  LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
  std::vector<std::string> out;
  for (int i = 1; i < n; ++i) out.push_back(pathString(fs::path(w[i])));
  LocalFree(w);
  SetConsoleOutputCP(CP_UTF8);
  return out;
#else
  return std::vector<std::string>(argv + 1, argv + argc);
#endif
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args = utf8Args(argc, argv);
  if (args.empty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
    usage();
    return args.empty() ? 2 : 0;
  }
  if (args[0] == "--version" || args[0] == "version") {
    std::printf("jvconvert %s\n", jc::version());
    return 0;
  }

  std::vector<std::string> pluginErrors;
  loadDefaultPlugins(pluginErrors);

  const std::string cmd = args[0];
  if (cmd == "mcp") {
    std::vector<fs::path> allow;
    for (size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--allow" && i + 1 < args.size()) allow.emplace_back(fs::u8path(args[++i]));
      else {
        std::fprintf(stderr, "jvconvert mcp: unknown argument %s\n", args[i].c_str());
        return 2;
      }
    }
    return runMcp(allow);
  }

  bool json = false;
  std::string kind;
  std::vector<std::string> positional;
  jc::ConvertOptions opt;
  for (size_t i = (cmd == "convert" || cmd == "probe" || cmd == "formats" || cmd == "plugins") ? 1 : 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--json") json = true;
    else if (a == "--lossless") opt.image.lossless = true;
    else if (a == "--no-textures") opt.model.textures = false;
    else if (a == "--no-overwrite") opt.overwrite = false;
    else if (a == "--kind" && i + 1 < args.size()) kind = args[++i];
    else if (a == "--plugin" && i + 1 < args.size()) {
      if (auto r = jc::loadPlugin(fs::u8path(args[++i])); !r) {
        std::fprintf(stderr, "error: %s\n", r.message.c_str());
        return 1;
      }
    } else if (a.rfind("--", 0) == 0) {
      std::string name = a.substr(2), value;
      auto eq = name.find('=');
      if (eq != std::string::npos) {
        value = name.substr(eq + 1);
        name = name.substr(0, eq);
      } else if (i + 1 < args.size()) {
        value = args[++i];
      } else {
        std::fprintf(stderr, "error: --%s needs a value\n", name.c_str());
        return 2;
      }
      if (auto err = applyOption(opt, name, value); !err.empty()) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
      }
    } else {
      positional.push_back(a);
    }
  }

  if (cmd == "formats") {
    Json arr = formatsJson(kind);
    if (json) {
      std::printf("%s\n", arr.dump(2).c_str());
      return 0;
    }
    for (auto& f : arr.items()) {
      std::string exts;
      for (auto& e : f["extensions"].items()) exts += (exts.empty() ? "" : ",") + e.asString();
      std::printf("%-6s %-10s %-14s read:%-3s write:%-3s %s\n", f["kind"].asString().c_str(), f["id"].asString().c_str(),
                  exts.c_str(), f["read"].asBool() ? "yes" : "no", f["write"].asBool() ? "yes" : "no",
                  f["write"].asBool() ? f["write_backend"].asString().c_str() : f["write_reason"].asString().c_str());
      if (const Json* t = f.find("transcode")) {
        std::string enc;
        for (auto& e : (*t)["encoders"].items()) enc += (enc.empty() ? "" : ", ") + e.asString();
        if ((*t)["available"].asBool())
          std::printf("%-6s %-10s re-encode: %s (%s)\n", "", "", (*t)["backend"].asString().c_str(),
                      enc.empty() ? "decode only" : enc.c_str());
        else
          std::printf("%-6s %-10s re-encode: no (%s)\n", "", "", (*t)["reason"].asString().c_str());
      }
    }
    return 0;
  }
  if (cmd == "plugins") {
    Json arr = Json::array();
    for (auto& p : jc::loadedPlugins()) {
      Json j = Json::object();
      j.set("name", p.name).set("version", p.version).set("license", p.license).set("format", p.formatId).set("source", p.source);
      arr.push(j);
    }
    Json errs = Json::array();
    for (auto& e : pluginErrors) errs.push(e);
    Json out = Json::object();
    out.set("plugins", arr).set("errors", errs);
    if (json) std::printf("%s\n", out.dump(2).c_str());
    else {
      if (arr.items().empty()) std::printf("No encoder plugins loaded.\n");
      for (auto& p : arr.items())
        std::printf("%s %s (%s) -> %s  [%s]\n", p["name"].asString().c_str(), p["version"].asString().c_str(),
                    p["license"].asString().c_str(), p["format"].asString().c_str(), p["source"].asString().c_str());
      for (auto& e : pluginErrors) std::printf("error: %s\n", e.c_str());
    }
    return 0;
  }
  if (cmd == "probe") {
    if (positional.size() != 1) {
      usage();
      return 2;
    }
    auto r = jc::probe(fs::u8path(positional[0]));
    if (json) {
      Json j = r.result ? toJson(r.info) : Json::object();
      if (!r.result) j.set("ok", false).set("status", jc::toString(r.result.status)).set("message", r.result.message);
      std::printf("%s\n", j.dump(2).c_str());
    } else if (r.result) {
      std::printf("%s\n", summary(r.info).c_str());
    } else {
      std::fprintf(stderr, "error: %s\n", r.result.message.c_str());
    }
    return exitCode(r.result.status);
  }

  if (positional.size() != 2) {
    usage();
    return 2;
  }
  if (!json) {
    opt.progress = [](double p) {
      std::fprintf(stderr, "\r%3d%%", int(p * 100 + 0.5));
      std::fflush(stderr);
    };
  }
  auto rep = jc::convert(fs::u8path(positional[0]), fs::u8path(positional[1]), opt);
  if (!json) std::fprintf(stderr, "\r     \r");
  if (json) {
    std::printf("%s\n", toJson(rep).dump(2).c_str());
  } else if (rep.result) {
    std::printf("%s -> %s\n", summary(rep.input).c_str(), summary(rep.output).c_str());
    for (auto& w : rep.warnings) std::printf("warning: %s\n", w.c_str());
  } else {
    std::fprintf(stderr, "error (%s): %s\n", jc::toString(rep.result.status), rep.result.message.c_str());
  }
  return exitCode(rep.result.status);
}
