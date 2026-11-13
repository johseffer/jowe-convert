#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

#include "../image/image.hpp"
#include "scene.hpp"

namespace jowe::convert::detail {

namespace {

std::string num(double v) {
  if (!std::isfinite(v)) v = 0;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.9g", v);
  return buf;
}

std::string ident(const std::string& raw, std::set<std::string>& used, const char* fallback) {
  std::string s;
  for (char c : raw) s += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
  if (s.empty()) s = fallback;
  if (std::isdigit(static_cast<unsigned char>(s[0]))) s = "_" + s;
  std::string out = s;
  for (int n = 1; used.count(out); ++n) out = s + "_" + std::to_string(n);
  used.insert(out);
  return out;
}

struct UsdaWriter {
  const Scene& scene;
  const std::vector<std::string>& texPaths;
  std::string out;
  std::vector<std::string> materialPaths;

  void line(int depth, const std::string& s) {
    out.append(size_t(depth) * 4, ' ');
    out += s;
    out += '\n';
  }

  std::string texShader(int depth, const std::string& matPath, std::set<std::string>& names, const std::string& role,
                        const TexRef& ref, bool raw, const std::array<float, 4>& scale,
                        const std::array<float, 4>& bias, const std::string& outType) {
    std::string name = ident(role + "Texture", names, "Texture");
    line(depth, "def Shader \"" + name + "\"");
    line(depth, "{");
    line(depth + 1, "uniform token info:id = \"UsdUVTexture\"");
    line(depth + 1, "asset inputs:file = @" + texPaths[size_t(ref.texture)] + "@");
    line(depth + 1, "float2 inputs:st.connect = <" + matPath + "/stReader.outputs:result>");
    line(depth + 1, std::string("token inputs:sourceColorSpace = \"") + (raw ? "raw" : "sRGB") + "\"");
    line(depth + 1, "token inputs:wrapS = \"repeat\"");
    line(depth + 1, "token inputs:wrapT = \"repeat\"");
    if (scale != std::array<float, 4>{1, 1, 1, 1})
      line(depth + 1, "float4 inputs:scale = (" + num(scale[0]) + ", " + num(scale[1]) + ", " + num(scale[2]) + ", " +
                          num(scale[3]) + ")");
    if (bias != std::array<float, 4>{0, 0, 0, 0})
      line(depth + 1, "float4 inputs:bias = (" + num(bias[0]) + ", " + num(bias[1]) + ", " + num(bias[2]) + ", " +
                          num(bias[3]) + ")");
    line(depth + 1, outType);
    line(depth, "}");
    return matPath + "/" + name;
  }

  void material(int depth, const SceneMaterial& m, const std::string& path, const std::string& name) {
    line(depth, "def Material \"" + name + "\"");
    line(depth, "{");
    const int d = depth + 1;
    line(d, "token outputs:surface.connect = <" + path + "/PreviewSurface.outputs:surface>");
    std::set<std::string> names{"PreviewSurface", "stReader"};
    auto has = [&](const TexRef& r) {
      return r.valid() && size_t(r.texture) < texPaths.size() && !texPaths[size_t(r.texture)].empty();
    };
    std::vector<std::string> inputs;
    std::string body;
    auto scalarChannel = [](const std::string& c) { return c == "rgb" || c == "rgba" ? std::string("r") : c; };
    auto chanScale = [](const std::string& c, float v) {
      std::array<float, 4> s{1, 1, 1, 1};
      int i = c == "g" ? 1 : c == "b" ? 2 : c == "a" ? 3 : 0;
      s[size_t(i)] = v;
      return s;
    };

    std::string saved = std::move(out);
    out.clear();
    if (has(m.baseColorTex)) {
      auto p = texShader(d, path, names, "diffuse", m.baseColorTex, false,
                         {m.baseColor[0], m.baseColor[1], m.baseColor[2], 1.0f}, {0, 0, 0, 0}, "float3 outputs:rgb");
      inputs.push_back("color3f inputs:diffuseColor.connect = <" + p + ".outputs:rgb>");
    } else {
      inputs.push_back("color3f inputs:diffuseColor = (" + num(m.baseColor[0]) + ", " + num(m.baseColor[1]) + ", " +
                       num(m.baseColor[2]) + ")");
    }
    if (has(m.metallicTex)) {
      std::string c = scalarChannel(m.metallicTex.channel);
      auto p = texShader(d, path, names, "metallic", m.metallicTex, true, chanScale(c, m.metallic), {0, 0, 0, 0},
                         "float outputs:" + c);
      inputs.push_back("float inputs:metallic.connect = <" + p + ".outputs:" + c + ">");
    } else {
      inputs.push_back("float inputs:metallic = " + num(m.metallic));
    }
    if (has(m.roughnessTex)) {
      std::string c = scalarChannel(m.roughnessTex.channel);
      auto p = texShader(d, path, names, "roughness", m.roughnessTex, true, chanScale(c, m.roughness), {0, 0, 0, 0},
                         "float outputs:" + c);
      inputs.push_back("float inputs:roughness.connect = <" + p + ".outputs:" + c + ">");
    } else {
      inputs.push_back("float inputs:roughness = " + num(m.roughness));
    }
    if (has(m.normalTex)) {
      auto p = texShader(d, path, names, "normal", m.normalTex, true, {2, 2, 2, 1}, {-1, -1, -1, 0},
                         "float3 outputs:rgb");
      inputs.push_back("normal3f inputs:normal.connect = <" + p + ".outputs:rgb>");
    }
    if (has(m.emissiveTex)) {
      auto p = texShader(d, path, names, "emissive", m.emissiveTex, false,
                         {m.emissive[0], m.emissive[1], m.emissive[2], 1.0f}, {0, 0, 0, 0}, "float3 outputs:rgb");
      inputs.push_back("color3f inputs:emissiveColor.connect = <" + p + ".outputs:rgb>");
    } else if (m.emissive != std::array<float, 3>{0, 0, 0}) {
      inputs.push_back("color3f inputs:emissiveColor = (" + num(m.emissive[0]) + ", " + num(m.emissive[1]) + ", " +
                       num(m.emissive[2]) + ")");
    }
    if (has(m.opacityTex) && (m.blend || m.opacityThreshold > 0)) {
      std::string c = scalarChannel(m.opacityTex.channel);
      auto p = texShader(d, path, names, "opacity", m.opacityTex, true, chanScale(c, m.opacity), {0, 0, 0, 0},
                         "float outputs:" + c);
      inputs.push_back("float inputs:opacity.connect = <" + p + ".outputs:" + c + ">");
    } else if (m.opacity < 1.0f) {
      inputs.push_back("float inputs:opacity = " + num(m.opacity));
    }
    if (m.opacityThreshold > 0) inputs.push_back("float inputs:opacityThreshold = " + num(m.opacityThreshold));
    if (has(m.occlusionTex)) {
      std::string c = scalarChannel(m.occlusionTex.channel);
      auto p = texShader(d, path, names, "occlusion", m.occlusionTex, true, {1, 1, 1, 1}, {0, 0, 0, 0},
                         "float outputs:" + c);
      inputs.push_back("float inputs:occlusion.connect = <" + p + ".outputs:" + c + ">");
    }
    body = std::move(out);
    out = std::move(saved);

    line(d, "def Shader \"PreviewSurface\"");
    line(d, "{");
    line(d + 1, "uniform token info:id = \"UsdPreviewSurface\"");
    for (auto& i : inputs) line(d + 1, i);
    line(d + 1, "int inputs:useSpecularWorkflow = 0");
    line(d + 1, "token outputs:surface");
    line(d, "}");
    if (!body.empty()) {
      line(d, "def Shader \"stReader\"");
      line(d, "{");
      line(d + 1, "uniform token info:id = \"UsdPrimvarReader_float2\"");
      line(d + 1, "string inputs:varname = \"st\"");
      line(d + 1, "float2 outputs:result");
      line(d, "}");
      out += body;
    }
    line(depth, "}");
  }

  template <class F>
  void array(int depth, const std::string& decl, size_t n, F&& item, const std::string& meta = "") {
    out.append(size_t(depth) * 4, ' ');
    out += decl + " = [";
    for (size_t i = 0; i < n; ++i) {
      if (i) out += ", ";
      item(i);
    }
    out += "]";
    if (!meta.empty()) out += " (\n" + std::string(size_t(depth + 1) * 4, ' ') + meta + "\n" +
                               std::string(size_t(depth) * 4, ' ') + ")";
    out += '\n';
  }

  void mesh(int depth, const SceneMesh& m, const std::string& name) {
    line(depth, "def Mesh \"" + name + "\" (");
    line(depth + 1, "prepend apiSchemas = [\"MaterialBindingAPI\"]");
    line(depth, ")");
    line(depth, "{");
    const int d = depth + 1;
    const size_t nv = m.vertexCount();
    float mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0};
    for (size_t v = 0; v < nv; ++v)
      for (int k = 0; k < 3; ++k) {
        float x = m.positions[v * 3 + k];
        if (v == 0 || x < mn[k]) mn[k] = x;
        if (v == 0 || x > mx[k]) mx[k] = x;
      }
    line(d, "float3[] extent = [(" + num(mn[0]) + ", " + num(mn[1]) + ", " + num(mn[2]) + "), (" + num(mx[0]) + ", " +
                num(mx[1]) + ", " + num(mx[2]) + ")]");
    const size_t faces = m.indices.size() / 3;
    array(d, "int[] faceVertexCounts", faces, [&](size_t) { out += '3'; });
    array(d, "int[] faceVertexIndices", m.indices.size(), [&](size_t i) { out += std::to_string(m.indices[i]); });
    bool doubleSided = m.material >= 0 && size_t(m.material) < scene.materials.size() &&
                       scene.materials[size_t(m.material)].doubleSided;
    if (doubleSided) line(d, "uniform bool doubleSided = 1");
    if (m.material >= 0 && size_t(m.material) < materialPaths.size())
      line(d, "rel material:binding = <" + materialPaths[size_t(m.material)] + ">");
    if (m.normals.size() == m.positions.size())
      array(d, "normal3f[] normals", nv, [&](size_t i) {
        out += "(" + num(m.normals[i * 3]) + ", " + num(m.normals[i * 3 + 1]) + ", " + num(m.normals[i * 3 + 2]) + ")";
      }, "interpolation = \"vertex\"");
    array(d, "point3f[] points", nv, [&](size_t i) {
      out += "(" + num(m.positions[i * 3]) + ", " + num(m.positions[i * 3 + 1]) + ", " + num(m.positions[i * 3 + 2]) + ")";
    });
    if (m.colors.size() == nv * 4)
      array(d, "color3f[] primvars:displayColor", nv, [&](size_t i) {
        out += "(" + num(m.colors[i * 4]) + ", " + num(m.colors[i * 4 + 1]) + ", " + num(m.colors[i * 4 + 2]) + ")";
      }, "interpolation = \"vertex\"");
    if (m.uvs.size() == nv * 2)
      array(d, "texCoord2f[] primvars:st", nv, [&](size_t i) {
        out += "(" + num(m.uvs[i * 2]) + ", " + num(m.uvs[i * 2 + 1]) + ")";
      }, "interpolation = \"vertex\"");
    line(d, "uniform token subdivisionScheme = \"none\"");
    line(depth, "}");
  }

  std::string matrix(const Matrix& m) {
    std::string s = "(";
    for (int r = 0; r < 4; ++r) {
      s += r ? ", (" : "(";
      for (int c = 0; c < 4; ++c) s += (c ? ", " : "") + num(m[size_t(r * 4 + c)]);
      s += ")";
    }
    return s + ")";
  }

  void node(int depth, int idx, const std::string& name, bool root) {
    const auto& n = scene.nodes[size_t(idx)];
    if (root) {
      line(depth, "def Xform \"" + name + "\" (");
      line(depth + 1, "kind = \"component\"");
      line(depth, ")");
    } else {
      line(depth, "def Xform \"" + name + "\"");
    }
    line(depth, "{");
    const int d = depth + 1;
    if (n.matrix != identityMatrix()) {
      line(d, "matrix4d xformOp:transform = " + matrix(n.matrix));
      line(d, "uniform token[] xformOpOrder = [\"xformOp:transform\"]");
    }
    std::set<std::string> used;
    if (root) {
      used.insert("Materials");
      if (!scene.materials.empty()) {
        line(d, "def Scope \"Materials\"");
        line(d, "{");
        std::set<std::string> matNames;
        std::vector<std::string> names;
        for (size_t i = 0; i < scene.materials.size(); ++i) {
          std::string mn = ident(scene.materials[i].name, matNames, "Material");
          names.push_back(mn);
        }
        for (size_t i = 0; i < scene.materials.size(); ++i)
          material(d + 1, scene.materials[i], materialPaths[i], names[i]);
        line(d, "}");
      }
    }
    for (int mi : n.meshes) mesh(d, scene.meshes[size_t(mi)], ident(scene.meshes[size_t(mi)].name, used, "Mesh"));
    for (int c : n.children) node(d, c, ident(scene.nodes[size_t(c)].name, used, "Node"), false);
    line(depth, "}");
  }
};

}  // namespace

std::string buildUsda(const Scene& scene, const std::vector<std::string>& texturePaths) {
  UsdaWriter w{scene, texturePaths, {}, {}};
  std::set<std::string> matNames;
  for (auto& m : scene.materials) w.materialPaths.push_back("/Root/Materials/" + ident(m.name, matNames, "Material"));
  w.out = "#usda 1.0\n(\n    customLayerData = {\n        string creator = \"jowe-convert " + std::string(version()) +
          "\"\n    }\n    defaultPrim = \"Root\"\n    metersPerUnit = " + num(scene.metersPerUnit) +
          "\n    upAxis = \"" + (scene.upAxis == "Z" ? "Z" : "Y") + "\"\n)\n\n";
  if (scene.nodes.empty()) {
    w.out += "def Xform \"Root\"\n{\n}\n";
  } else {
    w.node(0, 0, "Root", true);
  }
  return w.out;
}

Bytes buildStoredZip(const std::vector<std::pair<std::string, const Bytes*>>& files, size_t alignment) {
  ByteWriter w;
  struct Central { std::string name; uint32_t crc, size, offset; };
  std::vector<Central> cd;
  for (auto& [name, data] : files) {
    const uint32_t offset = uint32_t(w.size());
    const uint32_t crc = crc32(data->data(), data->size());
    size_t headerEnd = w.size() + 30 + name.size();
    size_t pad = alignment ? (alignment - headerEnd % alignment) % alignment : 0;
    if (pad > 0 && pad < 4) pad += alignment;
    w.le32(0x04034b50);
    w.le16(20);
    w.le16(0);
    w.le16(0);
    w.le16(0);
    w.le16(0x21);
    w.le32(crc);
    w.le32(uint32_t(data->size()));
    w.le32(uint32_t(data->size()));
    w.le16(uint16_t(name.size()));
    w.le16(uint16_t(pad));
    w.str(name);
    if (pad) {
      w.le16(0x1986);
      w.le16(uint16_t(pad - 4));
      w.zeros(pad - 4);
    }
    w.bytes(*data);
    cd.push_back({name, crc, uint32_t(data->size()), offset});
  }
  const uint32_t cdStart = uint32_t(w.size());
  for (auto& c : cd) {
    w.le32(0x02014b50);
    w.le16(20);
    w.le16(20);
    w.le16(0);
    w.le16(0);
    w.le16(0);
    w.le16(0x21);
    w.le32(c.crc);
    w.le32(c.size);
    w.le32(c.size);
    w.le16(uint16_t(c.name.size()));
    w.le16(0);
    w.le16(0);
    w.le16(0);
    w.le16(0);
    w.le32(0);
    w.le32(c.offset);
    w.str(c.name);
  }
  const uint32_t cdSize = uint32_t(w.size() - cdStart);
  w.le32(0x06054b50);
  w.le16(0);
  w.le16(0);
  w.le16(uint16_t(cd.size()));
  w.le16(uint16_t(cd.size()));
  w.le32(cdSize);
  w.le32(cdStart);
  w.le16(0);
  return std::move(w.data);
}

bool listZip(const Bytes& zip, std::vector<ZipEntry>& entries, std::string& err) {
  if (zip.size() < 22) { err = "too small for a zip"; return false; }
  size_t eocd = std::string::npos;
  for (size_t i = zip.size() - 22 + 1; i-- > 0 && zip.size() - i < 65557;) {
    if (zip[i] == 0x50 && zip[i + 1] == 0x4b && zip[i + 2] == 5 && zip[i + 3] == 6) { eocd = i; break; }
  }
  if (eocd == std::string::npos) { err = "no end of central directory"; return false; }
  ByteReader r(zip);
  r.seek(eocd + 10);
  uint16_t count = r.le16();
  r.le32();
  uint32_t cdAt = r.le32();
  r.seek(cdAt);
  for (uint16_t i = 0; i < count; ++i) {
    if (r.le32() != 0x02014b50) { err = "bad central directory"; return false; }
    r.skip(6);
    ZipEntry e;
    e.method = r.le16();
    r.skip(4);
    e.crc = r.le32();
    r.le32();
    e.size = r.le32();
    uint16_t nameLen = r.le16(), extraLen = r.le16(), commentLen = r.le16();
    r.skip(8);
    uint32_t local = r.le32();
    if (r.remaining() < nameLen) { err = "truncated name"; return false; }
    e.name.assign(reinterpret_cast<const char*>(r.ptr()), nameLen);
    r.skip(size_t(nameLen) + extraLen + commentLen);
    ByteReader lr(zip);
    lr.seek(local);
    if (lr.le32() != 0x04034b50) { err = "bad local header"; return false; }
    lr.seek(local + 26);
    uint16_t ln = lr.le16(), le = lr.le16();
    e.dataOffset = uint64_t(local) + 30 + ln + le;
    if (!r.ok() || !lr.ok()) { err = "truncated zip"; return false; }
    entries.push_back(e);
  }
  return true;
}

namespace {

// USDZ may only carry PNG / JPEG; other textures are re-encoded to PNG.
std::vector<std::pair<std::string, Bytes>> packTextures(const Context& ctx, const Scene& scene,
                                                        const std::string& prefix, std::vector<std::string>& paths) {
  std::vector<std::pair<std::string, Bytes>> out;
  std::set<std::string> used;
  paths.assign(scene.textures.size(), "");
  if (!ctx.options->model.textures) return out;
  for (size_t i = 0; i < scene.textures.size(); ++i) {
    const auto& t = scene.textures[i];
    Bytes data;
    std::string ext;
    if (t.format == "png" || t.format == "jpeg") {
      data = t.data;
      ext = t.format == "jpeg" ? "jpg" : "png";
    } else if (transcodeImageBytes(t.data, "png", data)) {
      ext = "png";
    } else {
      ctx.warn("Dropped a texture that can't be decoded (" + t.format + ")");
      continue;
    }
    std::string base = ident(t.name, used, "texture");
    std::string name = prefix + base + "." + ext;
    paths[i] = name;
    out.emplace_back(name, std::move(data));
  }
  return out;
}

}  // namespace

Result saveUsdz(const Context& ctx, const Scene& scene, const fs::path& path) {
  std::vector<std::string> paths;
  auto textures = packTextures(ctx, scene, "textures/", paths);
  std::string usda = buildUsda(scene, paths);
  Bytes layer(usda.begin(), usda.end());
  std::vector<std::pair<std::string, const Bytes*>> files;
  std::string layerName = path.stem().string();
  std::set<std::string> dummy;
  layerName = ident(layerName, dummy, "model") + ".usda";
  files.emplace_back(layerName, &layer);
  for (auto& [n, d] : textures) files.emplace_back(n, &d);
  uint64_t total = 0;
  for (auto& f : files) total += f.second->size() + 200;
  if (total > 0xFFFFFFF0ull) return Result::error(Status::Failed, "USDZ larger than 4 GB is not supported");
  Bytes zip = buildStoredZip(files, 64);
  PendingFile pending(ctx.io, path);
  if (auto r = ctx.io.writeFile(pending.tempPath(), zip); !r) return r;
  if (auto r = pending.commit(); !r) return r;
  if (ctx.written) ctx.written->push_back(path);
  return Result::ok();
}

Result saveUsda(const Context& ctx, const Scene& scene, const fs::path& path) {
  std::vector<std::string> paths;
  auto textures = packTextures(ctx, scene, path.stem().string() + "_", paths);
  std::string usda = buildUsda(scene, paths);
  std::vector<fs::path> written;
  auto cleanup = [&] {
    std::error_code ec;
    for (auto& w : written) fs::remove(w, ec);
  };
  for (auto& [n, d] : textures) {
    fs::path p = path.parent_path() / fs::u8path(n);
    if (auto r = ctx.io.writeFile(p, d); !r) {
      cleanup();
      return r;
    }
    written.push_back(p);
  }
  PendingFile pending(ctx.io, path);
  if (auto r = ctx.io.writeFile(pending.tempPath(), usda.data(), usda.size()); !r) {
    cleanup();
    return r;
  }
  if (auto r = pending.commit(); !r) {
    cleanup();
    return r;
  }
  written.push_back(path);
  if (ctx.written)
    for (auto& w : written) ctx.written->push_back(w);
  return Result::ok();
}

}  // namespace jowe::convert::detail
