#include <cctype>
#include <cstdio>
#include <iostream>
#include <string>

#include "shared.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace fs = std::filesystem;

namespace jvc {

namespace {

const char* kProtocolVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

std::string norm(const fs::path& p) {
  std::string s = pathString(p.lexically_normal());
#if defined(_WIN32)
  for (auto& c : s) {
    if (c == '\\') c = '/';
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
#endif
  while (s.size() > 1 && s.back() == '/') s.pop_back();
  return s;
}

class Sandbox {
 public:
  std::vector<fs::path> roots;
  std::vector<std::string> rootKeys;

  bool add(const fs::path& dir, std::string& err) {
    std::error_code ec;
    fs::path c = fs::canonical(dir, ec);
    if (ec || !fs::is_directory(c, ec)) {
      err = "--allow " + pathString(dir) + " is not an existing directory";
      return false;
    }
    roots.push_back(c);
    rootKeys.push_back(norm(c));
    return true;
  }

  // Relative paths are taken from the first allowed directory.
  fs::path resolve(const std::string& raw) const {
    fs::path p = fs::u8path(raw);
    if (p.is_relative() && !roots.empty()) p = roots[0] / p;
    return p;
  }

  // Symlinks are resolved before the check, so a link inside an allowed folder can't reach outside it.
  bool allows(const fs::path& p) const {
    if (roots.empty()) return false;
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) return false;
    fs::path c = fs::weakly_canonical(abs, ec);
    if (ec) return false;
    const std::string key = norm(c);
    for (auto& r : rootKeys) {
      if (key == r) return true;
      if (key.size() > r.size() && key.compare(0, r.size(), r) == 0 && (key[r.size()] == '/' || r.back() == '/'))
        return true;
    }
    return false;
  }
};

Json schema(Json props, std::vector<std::string> required) {
  Json req = Json::array();
  for (auto& r : required) req.push(r);
  Json s = Json::object();
  s.set("type", "object").set("properties", std::move(props));
  if (!required.empty()) s.set("required", req);
  s.set("additionalProperties", false);
  return s;
}

Json prop(const char* type, const char* description) {
  Json p = Json::object();
  p.set("type", type).set("description", description);
  return p;
}

Json toolList() {
  Json tools = Json::array();
  {
    Json props = Json::object();
    Json kind = prop("string", "Only formats of this kind");
    Json e = Json::array();
    for (auto* k : {"image", "audio", "video", "model"}) e.push(k);
    kind.set("enum", e);
    props.set("kind", kind);
    Json t = Json::object();
    t.set("name", "list_formats")
        .set("title", "List formats")
        .set("description",
             "Lists every format jvconvert knows, with whether this machine can read and write it right now "
             "(OS codecs and plugins are checked at run time) and the reason when it can't.")
        .set("inputSchema", schema(props, {}));
    Json ann = Json::object();
    ann.set("readOnlyHint", true).set("openWorldHint", false);
    t.set("annotations", ann);
    tools.push(t);
  }
  {
    Json props = Json::object();
    props.set("path", prop("string", "File to inspect (absolute, or relative to the first allowed directory)"));
    Json t = Json::object();
    t.set("name", "probe")
        .set("title", "Probe a media file")
        .set("description",
             "Reads a file's header and reports its format: size and channels for images; codec, sample rate and "
             "duration for audio; tracks for video; meshes, vertices, materials, textures, up axis and units for 3D.")
        .set("inputSchema", schema(props, {"path"}));
    Json ann = Json::object();
    ann.set("readOnlyHint", true).set("openWorldHint", false);
    t.set("annotations", ann);
    tools.push(t);
  }
  {
    Json props = Json::object();
    props.set("input", prop("string", "Source file"));
    props.set("output", prop("string", "Destination file; its extension picks the format unless 'format' is set"));
    props.set("format", prop("string", "Output format id (see list_formats)"));
    props.set("quality", prop("integer", "Image quality 1-100 (JPEG, WebP, HEIC)"));
    props.set("max_size", prop("integer", "Image long side in pixels; never upscales"));
    props.set("lossless", prop("boolean", "Lossless WebP"));
    props.set("sample_rate", prop("integer", "Audio sample rate in Hz"));
    props.set("channels", prop("integer", "Audio channels (1 mono, 2 stereo)"));
    props.set("bit_depth", prop("integer", "PCM / FLAC bit depth: 16, 24 or 32"));
    props.set("bitrate_kbps", prop("integer", "AAC / MP3 bitrate"));
    Json up = prop("string", "3D up axis handling");
    Json e = Json::array();
    for (auto* k : {"keep", "z_to_y", "y_to_z"}) e.push(k);
    up.set("enum", e);
    props.set("up_axis", up);
    props.set("scale", prop("number", "3D scale factor, e.g. 0.01 for centimetres to metres"));
    props.set("textures", prop("boolean", "Keep 3D textures (default true)"));
    props.set("overwrite", prop("boolean", "Replace an existing output (default true)"));
    auto enumProp = [&](const char* type, const char* desc, std::initializer_list<const char*> values) {
      Json p = prop(type, desc);
      Json e = Json::array();
      for (auto* v : values) e.push(v);
      p.set("enum", e);
      return p;
    };
    props.set("video_codec", enumProp("string", "Video codec; keep remuxes unless another video option needs re-encoding",
                                      {"keep", "h264", "hevc"}));
    props.set("resolution", enumProp("string", "Cap the video's short side (never upscales)",
                                     {"keep", "2160", "1080", "720", "480"}));
    props.set("video_bitrate_kbps", prop("integer", "Target video bitrate"));
    props.set("video_quality", enumProp("string", "Bitrate preset when no bitrate is given",
                                        {"auto", "low", "medium", "high"}));
    props.set("max_fps", prop("number", "Drop frames down to this frame rate"));
    props.set("audio", enumProp("string", "Video soundtrack: copy it, re-encode it to AAC, or remove it",
                                {"keep", "aac", "drop"}));
    props.set("audio_bitrate_kbps", prop("integer", "AAC bitrate when the soundtrack is re-encoded"));
    props.set("rotation", enumProp("string", "On re-encode: rotate the pixels upright (bake, default) or keep the "
                                             "source's rotation as track metadata",
                                   {"bake", "metadata"}));
    Json t = Json::object();
    t.set("name", "convert")
        .set("title", "Convert a media file")
        .set("description",
             "Converts an image, audio, video or 3D file to another format of the same kind. Video is remuxed "
             "between MP4 and MOV, or re-encoded with the OS encoder when a video option asks for it (see "
             "list_formats: transcode). Returns the probe of the input and the output.")
        .set("inputSchema", schema(props, {"input", "output"}));
    Json ann = Json::object();
    ann.set("readOnlyHint", false).set("destructiveHint", true).set("idempotentHint", true).set("openWorldHint", false);
    t.set("annotations", ann);
    tools.push(t);
  }
  return tools;
}

Json textResult(const Json& payload, bool isError) {
  Json content = Json::array();
  Json text = Json::object();
  text.set("type", "text").set("text", payload.dump(2));
  content.push(text);
  Json r = Json::object();
  r.set("content", content);
  if (payload.isObject()) r.set("structuredContent", payload);
  else {
    Json wrap = Json::object();
    wrap.set("formats", payload);
    r.set("structuredContent", wrap);
  }
  r.set("isError", isError);
  return r;
}

Json errorPayload(const std::string& status, const std::string& message) {
  Json j = Json::object();
  j.set("ok", false).set("status", status).set("message", message);
  return j;
}

Json callTool(const Sandbox& box, const std::string& name, const Json& args) {
  jc::AccessCheck access = [&box](const fs::path& p, bool) { return box.allows(p); };
  if (name == "list_formats") return textResult(formatsJson(args["kind"].asString()), false);
  if (name == "probe") {
    const std::string& raw = args["path"].asString();
    if (raw.empty()) return textResult(errorPayload("invalid_argument", "'path' is required"), true);
    fs::path p = box.resolve(raw);
    if (!box.allows(p))
      return textResult(errorPayload("access_denied", "Path is outside the allowed directories: " + raw), true);
    auto r = jc::probe(p, access);
    if (!r.result) return textResult(errorPayload(jc::toString(r.result.status), r.result.message), true);
    Json j = toJson(r.info);
    j.set("ok", true);
    return textResult(j, false);
  }
  if (name == "convert") {
    const std::string& in = args["input"].asString();
    const std::string& out = args["output"].asString();
    if (in.empty() || out.empty())
      return textResult(errorPayload("invalid_argument", "'input' and 'output' are required"), true);
    fs::path ip = box.resolve(in), op = box.resolve(out);
    if (!box.allows(ip))
      return textResult(errorPayload("access_denied", "Input is outside the allowed directories: " + in), true);
    if (!box.allows(op))
      return textResult(errorPayload("access_denied", "Output is outside the allowed directories: " + out), true);
    jc::ConvertOptions opt;
    opt.access = access;
    for (auto& [k, v] : args.members()) {
      if (k == "input" || k == "output") continue;
      if (k == "lossless") { opt.image.lossless = v.asBool(); continue; }
      if (k == "textures") { opt.model.textures = v.asBool(true); continue; }
      if (k == "overwrite") { opt.overwrite = v.asBool(true); continue; }
      std::string value = v.isString() ? v.asString() : v.isNumber() ? v.dump() : "";
      if (auto err = applyOption(opt, k, value); !err.empty())
        return textResult(errorPayload("invalid_argument", err), true);
    }
    auto rep = jc::convert(ip, op, opt);
    return textResult(toJson(rep), !rep.result);
  }
  return Json();
}

Json response(const Json& id, Json result) {
  Json r = Json::object();
  r.set("jsonrpc", "2.0").set("id", id).set("result", std::move(result));
  return r;
}

Json errorResponse(const Json& id, int code, const std::string& message) {
  Json e = Json::object();
  e.set("code", code).set("message", message);
  Json r = Json::object();
  r.set("jsonrpc", "2.0").set("id", id).set("error", e);
  return r;
}

Json handle(const Sandbox& box, const Json& msg, bool& hasReply) {
  hasReply = false;
  if (!msg.isObject()) {
    hasReply = true;
    return errorResponse(Json(), -32600, "Invalid Request");
  }
  const Json* idp = msg.find("id");
  const std::string& method = msg["method"].asString();
  if (!idp) return Json();
  hasReply = true;
  const Json& id = *idp;
  const Json& params = msg["params"];
  if (method.empty()) return errorResponse(id, -32600, "Invalid Request");
  if (method == "initialize") {
    std::string requested = params["protocolVersion"].asString();
    std::string version = kProtocolVersions[0];
    for (auto* v : kProtocolVersions)
      if (requested == v) version = v;
    Json caps = Json::object();
    Json tools = Json::object();
    tools.set("listChanged", false);
    caps.set("tools", tools);
    Json info = Json::object();
    info.set("name", "jvconvert").set("title", "jowe-convert").set("version", jc::version());
    Json r = Json::object();
    std::string allowed;
    for (auto& root : box.roots) allowed += (allowed.empty() ? "" : ", ") + pathString(root);
    r.set("protocolVersion", version)
        .set("capabilities", caps)
        .set("serverInfo", info)
        .set("instructions",
             "Converts images, audio, video (MP4/MOV remux) and 3D models. Call list_formats first to see what this "
             "machine can write. Files can only be read and written inside: " +
                 (allowed.empty() ? std::string("(no directory allowed)") : allowed) + ".");
    return response(id, r);
  }
  if (method == "ping") return response(id, Json::object());
  if (method == "tools/list") {
    Json r = Json::object();
    r.set("tools", toolList());
    return response(id, r);
  }
  if (method == "tools/call") {
    const std::string& name = params["name"].asString();
    Json result = callTool(box, name, params["arguments"]);
    if (result.isNull()) return errorResponse(id, -32602, "Unknown tool: " + name);
    return response(id, result);
  }
  return errorResponse(id, -32601, "Method not found: " + method);
}

void send(const Json& j) {
  std::string s = j.dump();
  s += '\n';
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
}

}  // namespace

int runMcp(const std::vector<fs::path>& allow) {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  Sandbox box;
  for (auto& a : allow) {
    std::string err;
    if (!box.add(a, err)) {
      std::fprintf(stderr, "jvconvert mcp: %s\n", err.c_str());
      return 2;
    }
  }
  if (box.roots.empty()) std::fprintf(stderr, "jvconvert mcp: no --allow directory; every file access will be refused\n");
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find_first_not_of(" \t") == std::string::npos) continue;
    Json msg;
    try {
      msg = Json::parse(line);
    } catch (const std::exception& e) {
      send(errorResponse(Json(), -32700, std::string("Parse error: ") + e.what()));
      continue;
    }
    if (msg.isArray()) {
      Json batch = Json::array();
      for (auto& m : msg.items()) {
        bool reply = false;
        Json r = handle(box, m, reply);
        if (reply) batch.push(r);
      }
      if (!batch.items().empty()) send(batch);
      continue;
    }
    bool reply = false;
    Json r = handle(box, msg, reply);
    if (reply) send(r);
  }
  return 0;
}

}  // namespace jvc
