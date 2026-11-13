#include <cstdint>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "tinyusdz.hh"
#include "asset-resolution.hh"
#include "tydra/render-data.hh"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <functional>
#include <map>

#include "../image/image.hpp"
#include "scene.hpp"

namespace jowe::convert::detail {

namespace {

namespace ty = tinyusdz::tydra;

struct LoaderState {
  const Context* ctx;
  bool usdz;
  bool textures;
};

bool loadTextureRaw(const tinyusdz::value::AssetPath& assetPath, const tinyusdz::AssetInfo&,
                    const tinyusdz::AssetResolutionResolver& resolver, ty::TextureImage* imageOut,
                    std::vector<uint8_t>* imageData, void* userdata, std::string* warn, std::string* err) {
  auto* st = static_cast<LoaderState*>(userdata);
  if (!st->textures) return false;
  const std::string name = assetPath.GetAssetPath();
  std::string resolved = resolver.resolve(name);
  if (resolved.empty()) {
    if (warn) *warn += "texture not found: " + name + "\n";
    return false;
  }
  if (!st->usdz && !st->ctx->io.canRead(fs::u8path(resolved))) {
    if (err) *err += "access denied: " + resolved + "\n";
    return false;
  }
  tinyusdz::Asset asset;
  if (!resolver.open_asset(resolved, name, &asset, warn, err)) return false;
  imageData->assign(asset.data(), asset.data() + asset.size());
  imageOut->asset_identifier = resolved;
  imageOut->decoded = false;
  ImageHeader h;
  if (stbInfo(*imageData, h) || webpInfo(*imageData, h)) {
    imageOut->width = h.width;
    imageOut->height = h.height;
    imageOut->channels = h.channels;
  }
  return true;
}

std::string channelName(ty::UVTexture::Channel c) {
  switch (c) {
    case ty::UVTexture::Channel::R: return "r";
    case ty::UVTexture::Channel::G: return "g";
    case ty::UVTexture::Channel::B: return "b";
    case ty::UVTexture::Channel::A: return "a";
    case ty::UVTexture::Channel::RGBA: return "rgba";
    default: return "rgb";
  }
}

template <class T>
const T* attrData(const ty::VertexAttribute& a, size_t& count) {
  count = a.vertex_count();
  return reinterpret_cast<const T*>(a.data.data());
}

}  // namespace

Result loadUsd(const Context& ctx, const fs::path& path, const std::string& format, Scene& scene) {
  Bytes data;
  if (auto r = ctx.io.readFile(path, data); !r) return r;
  std::string warn, err;
  tinyusdz::Stage stage;
  tinyusdz::USDLoadOptions opts;
  opts.max_memory_limit_in_mb = 8192;
  const std::string filename = path.u8string();
  if (!tinyusdz::LoadUSDFromMemory(data.data(), data.size(), filename, &stage, &warn, &err, opts))
    return Result::error(Status::Unreadable, "This model can't be read: " + err);

  const bool usdz = data.size() > 4 && data[0] == 'P' && data[1] == 'K' && data[2] == 3 && data[3] == 4;
  ty::RenderSceneConverterEnv env(stage);
  env.mesh_config.triangulate = true;
  env.mesh_config.build_vertex_indices = true;
  env.mesh_config.compute_tangents_and_binormals = false;
  env.mesh_config.compute_normals = false;
  // Tydra marks images from a custom loader as decoded; ours hands back the raw file bytes.
  env.scene_config.load_texture_assets = true;
  LoaderState state{&ctx, usdz, ctx.options ? ctx.options->model.textures : true};
  env.material_config.texture_image_loader_function = loadTextureRaw;
  env.material_config.texture_image_loader_function_userdata = &state;
  env.material_config.preserve_texel_bitdepth = true;
  tinyusdz::USDZAsset usdzAsset;
  if (usdz) {
    if (!tinyusdz::ReadUSDZAssetInfoFromMemory(data.data(), data.size(), false, &usdzAsset, &warn, &err))
      return Result::error(Status::Unreadable, "This USDZ can't be read: " + err);
    tinyusdz::AssetResolutionResolver arr;
    if (!tinyusdz::SetupUSDZAssetResolution(arr, &usdzAsset))
      return Result::error(Status::Unreadable, "This USDZ can't be read: asset table");
    env.asset_resolver = arr;
  } else {
    env.set_search_paths({path.parent_path().u8string()});
  }
  env.usd_filename = filename;

  ty::RenderScene rs;
  ty::RenderSceneConverter conv;
  std::string convWarning;
  if (conv.ConvertToRenderScene(env, &rs)) {
    convWarning = conv.GetWarning();
  } else {
    // Tydra's vertex re-indexing rejects some valid meshes; face-varying data is expanded below instead.
    env.mesh_config.build_vertex_indices = false;
    rs = ty::RenderScene();
    ty::RenderSceneConverter retry;
    if (!retry.ConvertToRenderScene(env, &rs))
      return Result::error(Status::Unreadable, "This model can't be read: " + retry.GetError().substr(0, 600));
    convWarning = retry.GetWarning();
  }
  if (!convWarning.empty()) ctx.warn("USD: " + convWarning.substr(0, 400));

  const auto& metas = stage.metas();
  const auto axis = metas.upAxis.get_value();
  scene.upAxis = axis == tinyusdz::Axis::Z ? "Z" : (axis == tinyusdz::Axis::X ? "X" : "Y");
  const double mpu = metas.metersPerUnit.get_value();
  scene.metersPerUnit = mpu > 0 ? mpu : 1.0;
  scene.unitsKnown = true;

  std::vector<int> imageToTexture(rs.images.size(), -1);
  for (size_t i = 0; i < rs.images.size(); ++i) {
    const auto& img = rs.images[i];
    if (img.buffer_id < 0 || size_t(img.buffer_id) >= rs.buffers.size()) continue;
    SceneTexture t;
    t.data = rs.buffers[size_t(img.buffer_id)].data;
    if (t.data.empty()) continue;
    t.format = sniffImageFormat(t.data);
    if (t.format.empty()) t.format = "png";
    std::string id = img.asset_identifier;
    auto slash = id.find_last_of("/\\[]");
    std::string base = slash == std::string::npos ? id : id.substr(slash + 1);
    t.name = fs::u8path(base).stem().u8string();
    imageToTexture[i] = int(scene.textures.size());
    scene.textures.push_back(std::move(t));
  }

  auto texRef = [&](int32_t textureId, const char* fallbackChannel) {
    TexRef r;
    if (textureId < 0 || size_t(textureId) >= rs.textures.size()) return r;
    const auto& uv = rs.textures[size_t(textureId)];
    if (uv.texture_image_id < 0 || size_t(uv.texture_image_id) >= imageToTexture.size()) return r;
    r.texture = imageToTexture[size_t(uv.texture_image_id)];
    r.channel = channelName(uv.connectedOutputChannel);
    if (r.channel == "rgb" && std::string(fallbackChannel) != "rgb") r.channel = fallbackChannel;
    return r;
  };
  for (const auto& m : rs.materials) {
    SceneMaterial out;
    out.name = m.name;
    const auto& s = m.surfaceShader;
    out.baseColor = {s.diffuseColor.value[0], s.diffuseColor.value[1], s.diffuseColor.value[2], 1.0f};
    out.baseColorTex = texRef(s.diffuseColor.texture_id, "rgb");
    if (out.baseColorTex.valid()) {
      const auto& uv = rs.textures[size_t(s.diffuseColor.texture_id)];
      out.baseColor = {uv.scale[0], uv.scale[1], uv.scale[2], 1.0f};
    }
    out.metallic = s.metallic.value;
    out.metallicTex = texRef(s.metallic.texture_id, "r");
    if (out.metallicTex.valid()) out.metallic = 1.0f;
    out.roughness = s.roughness.value;
    out.roughnessTex = texRef(s.roughness.texture_id, "r");
    if (out.roughnessTex.valid()) out.roughness = 1.0f;
    out.normalTex = texRef(s.normal.texture_id, "rgb");
    out.emissive = {s.emissiveColor.value[0], s.emissiveColor.value[1], s.emissiveColor.value[2]};
    out.emissiveTex = texRef(s.emissiveColor.texture_id, "rgb");
    if (out.emissiveTex.valid() && out.emissive == std::array<float, 3>{0, 0, 0}) out.emissive = {1, 1, 1};
    out.opacity = s.opacity.value;
    out.opacityTex = texRef(s.opacity.texture_id, "a");
    out.opacityThreshold = s.opacityThreshold.value;
    out.occlusionTex = texRef(s.occlusion.texture_id, "r");
    out.blend = (out.opacityTex.valid() || out.opacity < 1.0f) && out.opacityThreshold <= 0.0f;
    scene.materials.push_back(std::move(out));
  }

  std::vector<std::vector<int>> meshParts(rs.meshes.size());
  for (size_t mi = 0; mi < rs.meshes.size(); ++mi) {
    const auto& rm = rs.meshes[mi];
    const auto& idx = rm.faceVertexIndices();
    if (rm.points.empty() || idx.empty()) continue;
    for (auto c : rm.faceVertexCounts())
      if (c != 3) return Result::error(Status::Unreadable, "This model can't be read: a mesh was not triangulated");

    const ty::VertexAttribute* uvAttr = nullptr;
    if (auto it = rm.texcoords.find(0); it != rm.texcoords.end() && !it->second.empty()) uvAttr = &it->second;
    const bool faceVaryingN = !rm.normals.empty() && rm.normals.variability == ty::VertexVariability::FaceVarying;
    const bool faceVaryingUv = uvAttr && uvAttr->variability == ty::VertexVariability::FaceVarying;
    const bool expand = faceVaryingN || faceVaryingUv;

    SceneMesh base;
    base.name = rm.prim_name;
    const size_t corners = idx.size();
    const size_t nv = expand ? corners : rm.points.size();
    base.positions.resize(nv * 3);
    for (size_t v = 0; v < nv; ++v) {
      const auto& p = rm.points[expand ? idx[v] : v];
      base.positions[v * 3] = p[0];
      base.positions[v * 3 + 1] = p[1];
      base.positions[v * 3 + 2] = p[2];
    }
    if (!rm.normals.empty() && rm.normals.format == ty::VertexAttributeFormat::Vec3) {
      size_t n = 0;
      const float* src = attrData<float>(rm.normals, n);
      base.normals.resize(nv * 3);
      for (size_t v = 0; v < nv; ++v) {
        size_t s = faceVaryingN ? v : (expand ? idx[v] : v);
        if (s >= n) s = 0;
        for (int k = 0; k < 3; ++k) base.normals[v * 3 + k] = n ? src[s * 3 + k] : 0.0f;
      }
    }
    if (uvAttr && uvAttr->format == ty::VertexAttributeFormat::Vec2) {
      size_t n = 0;
      const float* src = attrData<float>(*uvAttr, n);
      base.uvs.resize(nv * 2);
      for (size_t v = 0; v < nv; ++v) {
        size_t s = faceVaryingUv ? v : (expand ? idx[v] : v);
        if (s >= n) s = 0;
        base.uvs[v * 2] = n ? src[s * 2] : 0.0f;
        base.uvs[v * 2 + 1] = n ? src[s * 2 + 1] : 0.0f;
      }
    }
    std::vector<uint32_t> tri(corners);
    for (size_t c = 0; c < corners; ++c) tri[c] = expand ? uint32_t(c) : idx[c];
    if (!rm.is_rightHanded)
      for (size_t f = 0; f + 2 < corners; f += 3) std::swap(tri[f + 1], tri[f + 2]);

    const size_t faces = corners / 3;
    std::vector<int> faceMat(faces, rm.material_id);
    for (const auto& [name, subset] : rm.material_subsetMap) {
      for (int f : subset.indices())
        if (f >= 0 && size_t(f) < faces) faceMat[size_t(f)] = subset.material_id;
    }
    std::map<int, std::vector<uint32_t>> byMat;
    for (size_t f = 0; f < faces; ++f)
      byMat[faceMat[f]].insert(byMat[faceMat[f]].end(), tri.begin() + long(f * 3), tri.begin() + long(f * 3 + 3));
    for (auto& [mat, ind] : byMat) {
      SceneMesh part;
      part.name = base.name + (byMat.size() > 1 ? "_" + std::to_string(mat < 0 ? 0 : mat) : "");
      part.material = mat;
      if (byMat.size() == 1) {
        part.positions = base.positions;
        part.normals = base.normals;
        part.uvs = base.uvs;
        part.indices = std::move(ind);
      } else {
        std::map<uint32_t, uint32_t> remap;
        for (uint32_t v : ind) {
          auto [it, inserted] = remap.emplace(v, uint32_t(remap.size()));
          if (inserted) {
            part.positions.insert(part.positions.end(), &base.positions[v * 3], &base.positions[v * 3] + 3);
            if (!base.normals.empty())
              part.normals.insert(part.normals.end(), &base.normals[v * 3], &base.normals[v * 3] + 3);
            if (!base.uvs.empty()) part.uvs.insert(part.uvs.end(), &base.uvs[v * 2], &base.uvs[v * 2] + 2);
          }
          part.indices.push_back(it->second);
        }
      }
      meshParts[mi].push_back(int(scene.meshes.size()));
      scene.meshes.push_back(std::move(part));
    }
  }

  scene.nodes.emplace_back();
  scene.nodes[0].name = "Root";
  std::function<bool(const ty::Node&)> hasMesh = [&](const ty::Node& n) {
    if (n.nodeType == ty::NodeType::Mesh && n.id >= 0) return true;
    for (const auto& c : n.children)
      if (hasMesh(c)) return true;
    return false;
  };
  std::function<int(const ty::Node&)> build = [&](const ty::Node& n) -> int {
    int at = int(scene.nodes.size());
    scene.nodes.emplace_back();
    scene.nodes[size_t(at)].name = n.prim_name;
    Matrix m;
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) m[size_t(i * 4 + j)] = n.local_matrix.m[i][j];
    scene.nodes[size_t(at)].matrix = m;
    if (n.nodeType == ty::NodeType::Mesh && n.id >= 0 && size_t(n.id) < meshParts.size())
      scene.nodes[size_t(at)].meshes = meshParts[size_t(n.id)];
    for (const auto& c : n.children) {
      if (!hasMesh(c)) continue;
      int ci = build(c);
      scene.nodes[size_t(at)].children.push_back(ci);
    }
    return at;
  };
  for (const auto& n : rs.nodes) {
    if (!hasMesh(n)) continue;
    int ci = build(n);
    scene.nodes[0].children.push_back(ci);
  }
  (void)format;
  return Result::ok();
}

}  // namespace jowe::convert::detail
