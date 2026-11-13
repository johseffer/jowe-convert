#include <cmath>
#include <functional>
#include <map>

#include "../image/image.hpp"
#include "scene.hpp"

namespace jowe::convert::detail {

Matrix identityMatrix() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Matrix multiply(const Matrix& a, const Matrix& b) {
  Matrix r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += a[size_t(i * 4 + k)] * b[size_t(k * 4 + j)];
      r[size_t(i * 4 + j)] = s;
    }
  return r;
}

ModelStats sceneStats(const Scene& s) {
  ModelStats st;
  st.meshes = s.meshes.size();
  for (auto& m : s.meshes) {
    st.vertices += m.vertexCount();
    st.triangles += m.indices.size() / 3;
  }
  st.materials = s.materials.size();
  st.textures = s.textures.size();
  st.nodes = s.nodes.size();
  st.upAxis = s.upAxis;
  st.metersPerUnit = s.unitsKnown ? s.metersPerUnit : 0;
  bool first = true;
  std::function<void(int, const Matrix&)> walk = [&](int idx, const Matrix& parent) {
    const auto& n = s.nodes[size_t(idx)];
    Matrix world = multiply(n.matrix, parent);
    for (int mi : n.meshes) {
      const auto& m = s.meshes[size_t(mi)];
      for (size_t v = 0; v < m.vertexCount(); ++v) {
        double p[3];
        for (int c = 0; c < 3; ++c)
          p[c] = m.positions[v * 3] * world[size_t(c)] + m.positions[v * 3 + 1] * world[size_t(4 + c)] +
                 m.positions[v * 3 + 2] * world[size_t(8 + c)] + world[size_t(12 + c)];
        for (int c = 0; c < 3; ++c) {
          if (first || p[c] < st.boundsMin[c]) st.boundsMin[c] = float(p[c]);
          if (first || p[c] > st.boundsMax[c]) st.boundsMax[c] = float(p[c]);
        }
        first = false;
      }
    }
    for (int c : n.children) walk(c, world);
  };
  if (!s.nodes.empty()) walk(0, identityMatrix());
  return st;
}

namespace {

bool isUsd(const std::string& f) { return f == "usd" || f == "usda" || f == "usdc" || f == "usdz"; }

Result loadScene(const Context& ctx, const fs::path& path, const std::string& format, Scene& scene) {
  if (isUsd(format)) return loadUsd(ctx, path, format, scene);
  return loadAssimp(ctx, path, format, scene);
}

void applyRoot(Scene& s, const Matrix& t) {
  if (s.nodes.empty()) return;
  s.nodes[0].matrix = multiply(s.nodes[0].matrix, t);
}

const Matrix kZToY = {1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1};
const Matrix kYToZ = {1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1};

Matrix scaleMatrix(double k) { return {k, 0, 0, 0, 0, k, 0, 0, 0, 0, k, 0, 0, 0, 0, 1}; }

int channelIndex(const std::string& c) { return c == "g" ? 1 : c == "b" ? 2 : c == "a" ? 3 : 0; }

void combineMetallicRoughness(const Context& ctx, Scene& s) {
  std::map<std::pair<std::pair<int, std::string>, std::pair<int, std::string>>, int> cache;
  for (auto& m : s.materials) {
    const bool r = m.roughnessTex.valid(), mt = m.metallicTex.valid();
    if (!r && !mt) continue;
    if (r && mt && m.roughnessTex.texture == m.metallicTex.texture && m.roughnessTex.channel == "g" &&
        m.metallicTex.channel == "b")
      continue;
    auto key = std::make_pair(std::make_pair(r ? m.roughnessTex.texture : -1, m.roughnessTex.channel),
                              std::make_pair(mt ? m.metallicTex.texture : -1, m.metallicTex.channel));
    if (auto it = cache.find(key); it != cache.end()) {
      m.roughnessTex = {it->second, "g"};
      m.metallicTex = {it->second, "b"};
      continue;
    }
    Image rough, metal;
    bool okR = r && decodeImage(s.textures[size_t(m.roughnessTex.texture)].data, "", rough);
    bool okM = mt && decodeImage(s.textures[size_t(m.metallicTex.texture)].data, "", metal);
    if (!okR && !okM) {
      ctx.warn("Metallic / roughness textures could not be decoded and were dropped");
      m.roughnessTex = {};
      m.metallicTex = {};
      continue;
    }
    const Image& ref = okR ? rough : metal;
    Image out;
    out.width = ref.width;
    out.height = ref.height;
    out.hasAlpha = false;
    out.rgba.assign(size_t(out.width) * out.height * 4, 255);
    Image scaled;
    auto fill = [&](Image& src, int channel, int dst) {
      Image* use = &src;
      if (src.width != out.width || src.height != out.height) {
        resizeRgba(src, out.width, out.height, scaled);
        use = &scaled;
      }
      for (size_t i = 0, n = size_t(out.width) * out.height; i < n; ++i) out.rgba[i * 4 + dst] = use->rgba[i * 4 + channel];
    };
    if (okR) fill(rough, channelIndex(m.roughnessTex.channel), 1);
    if (okM) fill(metal, channelIndex(m.metallicTex.channel), 2);
    SceneTexture t;
    t.format = "png";
    t.name = (m.name.empty() ? std::string("material") : m.name) + "_metallicRoughness";
    if (!stbEncode(out, "png", 90, t.data)) continue;
    s.textures.push_back(std::move(t));
    int idx = int(s.textures.size() - 1);
    cache[key] = idx;
    m.roughnessTex = {idx, "g"};
    m.metallicTex = {idx, "b"};
  }
}

}  // namespace

Result probeModel(const Context& ctx, const fs::path& path, const std::string& format, ProbeInfo& info) {
  Scene scene;
  if (auto r = loadScene(ctx, path, format, scene); !r) return r;
  info.format = format;
  info.model = sceneStats(scene);
  return Result::ok();
}

Result convertModel(const Context& ctx, const fs::path& in, const std::string& inFormat, const fs::path& out,
                    const std::string& outFormat, ProbeInfo& outInfo) {
  const ModelOptions& opt = ctx.options->model;
  if (!(opt.scale > 0) || !std::isfinite(opt.scale)) return Result::error(Status::InvalidArgument, "scale must be > 0");
  Scene scene;
  if (auto r = loadScene(ctx, in, inFormat, scene); !r) return r;
  if (scene.meshes.empty()) return Result::error(Status::Unreadable, "The model holds no meshes");
  if (ctx.input) ctx.input->model = sceneStats(scene);
  if (ctx.cancelled()) return Result::error(Status::Cancelled, "Cancelled");
  ctx.progress(0.4);

  if (opt.upAxis == ModelOptions::UpAxis::ZToY) {
    applyRoot(scene, kZToY);
    scene.upAxis = "Y";
  } else if (opt.upAxis == ModelOptions::UpAxis::YToZ) {
    applyRoot(scene, kYToZ);
    scene.upAxis = "Z";
  }
  if (opt.scale != 1.0) applyRoot(scene, scaleMatrix(opt.scale));

  if (!opt.textures) {
    scene.textures.clear();
    for (auto& m : scene.materials)
      for (TexRef* t : {&m.baseColorTex, &m.metallicTex, &m.roughnessTex, &m.normalTex, &m.emissiveTex, &m.opacityTex,
                        &m.occlusionTex})
        *t = {};
  }

  Result r;
  if (outFormat == "gltf" || outFormat == "glb") {
    if (scene.upAxis == "Z") applyRoot(scene, kZToY);
    if (scene.unitsKnown && scene.metersPerUnit != 1.0) applyRoot(scene, scaleMatrix(scene.metersPerUnit));
    scene.upAxis = "Y";
    scene.metersPerUnit = 1.0;
    combineMetallicRoughness(ctx, scene);
    r = saveAssimp(ctx, scene, out, outFormat);
  } else if (outFormat == "obj" || outFormat == "stl" || outFormat == "ply") {
    r = saveAssimp(ctx, scene, out, outFormat);
  } else if (outFormat == "usdz") {
    r = saveUsdz(ctx, scene, out);
  } else if (outFormat == "usda" || outFormat == "usd") {
    r = saveUsda(ctx, scene, out);
  } else {
    return Result::error(Status::NotWritable, "3D models can't be written as " + outFormat);
  }
  if (!r) return r;
  ctx.progress(0.9);

  std::error_code ec;
  outInfo.kind = MediaKind::Model;
  outInfo.format = outFormat;
  outInfo.fileSize = fs::file_size(out, ec);
  Context quiet = ctx;
  quiet.warnings = nullptr;
  quiet.written = nullptr;
  quiet.input = nullptr;
  ConvertOptions probeOpts = *ctx.options;
  probeOpts.model = ModelOptions{};
  quiet.options = &probeOpts;
  std::string probeFormat = outFormat == "usd" ? "usda" : outFormat;
  probeModel(quiet, out, probeFormat, outInfo);
  outInfo.format = outFormat;
  ctx.progress(1.0);
  return Result::ok();
}

}  // namespace jowe::convert::detail
