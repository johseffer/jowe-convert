#include <assimp/DefaultIOSystem.h>
#include <assimp/config.h>
#include <assimp/Exporter.hpp>
#include <assimp/GltfMaterial.h>
#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <memory>

#include "../image/image.hpp"
#include "scene.hpp"

namespace jowe::convert::detail {

namespace {

fs::path u8path(const char* s) {
#if defined(__cpp_char8_t)
  return fs::path(reinterpret_cast<const char8_t*>(s));
#else
  return fs::u8path(s);
#endif
}

class GuardedIO : public Assimp::IOSystem {
 public:
  GuardedIO(const Io& io, std::vector<fs::path>* written) : io_(io), written_(written) {}
  bool Exists(const char* p) const override { return io_.canRead(u8path(p)) && fs_.Exists(p); }
  char getOsSeparator() const override { return fs_.getOsSeparator(); }
  void Close(Assimp::IOStream* s) override { fs_.Close(s); }
  bool ComparePaths(const char* a, const char* b) const override { return fs_.ComparePaths(a, b); }
  Assimp::IOStream* Open(const char* p, const char* mode) override {
    const bool write = std::strchr(mode, 'w') || std::strchr(mode, 'a');
    const fs::path path = u8path(p);
    if (write ? !io_.canWrite(path) : !io_.canRead(path)) {
      denied = path.string();
      return nullptr;
    }
    Assimp::IOStream* s = fs_.Open(p, mode);
    if (s && write && written_) written_->push_back(path);
    return s;
  }
  std::string denied;

 private:
  const Io& io_;
  std::vector<fs::path>* written_;
  mutable Assimp::DefaultIOSystem fs_;
};

Matrix fromAi(const aiMatrix4x4& m) {
  Matrix r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) r[size_t(i * 4 + j)] = m[j][i];
  return r;
}

aiMatrix4x4 toAi(const Matrix& m) {
  aiMatrix4x4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) r[j][i] = ai_real(m[size_t(i * 4 + j)]);
  return r;
}

std::string extFromHint(const char* hint) {
  std::string h = lower(hint ? hint : "");
  if (h == "jpg" || h == "jpeg") return "jpeg";
  return h.empty() ? "png" : h;
}

struct Importer {
  const Context& ctx;
  const aiScene* ai;
  fs::path baseDir;
  Scene& scene;
  std::map<std::string, int> byPath;

  int texture(const aiString& path) {
    std::string key = path.C_Str();
    if (key.empty()) return -1;
    if (auto it = byPath.find(key); it != byPath.end()) return it->second;
    SceneTexture tex;
    if (const aiTexture* emb = ai->GetEmbeddedTexture(key.c_str())) {
      if (emb->mHeight == 0) {
        auto* p = reinterpret_cast<const uint8_t*>(emb->pcData);
        tex.data.assign(p, p + emb->mWidth);
        std::string sniffed = sniffImageFormat(tex.data);
        tex.format = sniffed.empty() ? extFromHint(emb->achFormatHint) : sniffed;
      } else {
        Image img;
        img.width = int(emb->mWidth);
        img.height = int(emb->mHeight);
        img.hasAlpha = true;
        img.rgba.resize(size_t(img.width) * img.height * 4);
        for (size_t i = 0; i < size_t(img.width) * img.height; ++i) {
          img.rgba[i * 4] = emb->pcData[i].r;
          img.rgba[i * 4 + 1] = emb->pcData[i].g;
          img.rgba[i * 4 + 2] = emb->pcData[i].b;
          img.rgba[i * 4 + 3] = emb->pcData[i].a;
        }
        if (!stbEncode(img, "png", 90, tex.data)) return byPath[key] = -1;
        tex.format = "png";
      }
      tex.name = emb->mFilename.length ? fs::path(emb->mFilename.C_Str()).stem().string() : "embedded";
    } else {
      std::string rel = key;
      for (auto& c : rel)
        if (c == '\\') c = '/';
      fs::path p = u8path(rel.c_str());
      if (p.is_relative()) p = baseDir / p;
      Bytes data;
      if (!ctx.io.readFile(p, data)) {
        ctx.warn("Texture not found or not allowed: " + key);
        return byPath[key] = -1;
      }
      tex.data = std::move(data);
      tex.format = sniffImageFormat(tex.data);
      if (tex.format.empty()) tex.format = extensionOf(p) == "jpg" ? "jpeg" : extensionOf(p);
      tex.name = p.stem().string();
    }
    scene.textures.push_back(std::move(tex));
    return byPath[key] = int(scene.textures.size() - 1);
  }

  TexRef tex(const aiMaterial* m, aiTextureType type, const char* channel) {
    TexRef r;
    aiString path;
    if (m->GetTextureCount(type) > 0 && m->GetTexture(type, 0, &path) == AI_SUCCESS) {
      r.texture = texture(path);
      r.channel = channel;
    }
    return r;
  }

  void material(const aiMaterial* m) {
    SceneMaterial out;
    aiString name;
    if (m->Get(AI_MATKEY_NAME, name) == AI_SUCCESS) out.name = name.C_Str();
    aiColor4D c4;
    aiColor3D c3;
    if (m->Get(AI_MATKEY_BASE_COLOR, c4) == AI_SUCCESS) out.baseColor = {c4.r, c4.g, c4.b, c4.a};
    else if (m->Get(AI_MATKEY_COLOR_DIFFUSE, c3) == AI_SUCCESS) out.baseColor = {c3.r, c3.g, c3.b, 1.0f};
    float f = 0;
    if (m->Get(AI_MATKEY_OPACITY, f) == AI_SUCCESS && f < 1.0f) out.opacity = std::clamp(f, 0.0f, 1.0f);
    out.opacity *= out.baseColor[3];
    if (m->Get(AI_MATKEY_METALLIC_FACTOR, f) == AI_SUCCESS) out.metallic = f;
    if (m->Get(AI_MATKEY_ROUGHNESS_FACTOR, f) == AI_SUCCESS) out.roughness = f;
    else if (m->Get(AI_MATKEY_SHININESS, f) == AI_SUCCESS && f > 0) out.roughness = std::sqrt(2.0f / (f + 2.0f));
    if (m->Get(AI_MATKEY_COLOR_EMISSIVE, c3) == AI_SUCCESS) {
      float k = 1.0f;
      m->Get(AI_MATKEY_EMISSIVE_INTENSITY, k);
      out.emissive = {c3.r * k, c3.g * k, c3.b * k};
    }
    int two = 0;
    if (m->Get(AI_MATKEY_TWOSIDED, two) == AI_SUCCESS) out.doubleSided = two != 0;
    aiString mode;
    if (m->Get(AI_MATKEY_GLTF_ALPHAMODE, mode) == AI_SUCCESS) {
      std::string s = mode.C_Str();
      if (s == "BLEND") out.blend = true;
      if (s == "MASK") {
        float cut = 0.5f;
        m->Get(AI_MATKEY_GLTF_ALPHACUTOFF, cut);
        out.opacityThreshold = cut;
      }
      if (s == "OPAQUE") out.opacity = 1.0f;
    } else if (out.opacity < 1.0f) {
      out.blend = true;
    }

    out.baseColorTex = tex(m, aiTextureType_BASE_COLOR, "rgb");
    if (!out.baseColorTex.valid()) out.baseColorTex = tex(m, aiTextureType_DIFFUSE, "rgb");
    out.normalTex = tex(m, aiTextureType_NORMALS, "rgb");
    out.emissiveTex = tex(m, aiTextureType_EMISSIVE, "rgb");
    if (!out.emissiveTex.valid()) out.emissiveTex = tex(m, aiTextureType_EMISSION_COLOR, "rgb");
    out.occlusionTex = tex(m, aiTextureType_AMBIENT_OCCLUSION, "r");
    if (!out.occlusionTex.valid()) out.occlusionTex = tex(m, aiTextureType_LIGHTMAP, "r");
    TexRef mr = tex(m, aiTextureType_GLTF_METALLIC_ROUGHNESS, "rgb");
    if (mr.valid()) {
      out.roughnessTex = {mr.texture, "g"};
      out.metallicTex = {mr.texture, "b"};
    } else {
      TexRef metal = tex(m, aiTextureType_METALNESS, "r");
      TexRef rough = tex(m, aiTextureType_DIFFUSE_ROUGHNESS, "r");
      if (metal.valid() && metal.texture == rough.texture) {
        out.roughnessTex = {metal.texture, "g"};
        out.metallicTex = {metal.texture, "b"};
      } else {
        out.metallicTex = metal;
        out.roughnessTex = rough;
      }
    }
    if (out.metallicTex.valid() && !out.metallic) out.metallic = 1.0f;
    if (out.roughnessTex.valid() && m->Get(AI_MATKEY_ROUGHNESS_FACTOR, f) != AI_SUCCESS) out.roughness = 1.0f;
    out.opacityTex = tex(m, aiTextureType_OPACITY, "r");
    if (out.opacityTex.valid()) out.blend = true;
    else if ((out.blend || out.opacityThreshold > 0) && out.baseColorTex.valid())
      out.opacityTex = {out.baseColorTex.texture, "a"};
    scene.materials.push_back(std::move(out));
  }

  int node(const aiNode* n) {
    int idx = int(scene.nodes.size());
    scene.nodes.emplace_back();
    scene.nodes[size_t(idx)].name = n->mName.C_Str();
    scene.nodes[size_t(idx)].matrix = fromAi(n->mTransformation);
    for (unsigned i = 0; i < n->mNumMeshes; ++i) {
      int m = meshMap[n->mMeshes[i]];
      if (m >= 0) scene.nodes[size_t(idx)].meshes.push_back(m);
    }
    for (unsigned i = 0; i < n->mNumChildren; ++i) {
      int c = node(n->mChildren[i]);
      scene.nodes[size_t(idx)].children.push_back(c);
    }
    return idx;
  }

  std::vector<int> meshMap;
};

}  // namespace

Result loadAssimp(const Context& ctx, const fs::path& path, const std::string& format, Scene& scene) {
  if (!ctx.io.canRead(path)) return Result::error(Status::AccessDenied, "Access denied: " + path.string());
  Assimp::Importer importer;
  auto* io = new GuardedIO(ctx.io, nullptr);
  importer.SetIOHandler(io);
  importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);
  const unsigned flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_SortByPType |
                         aiProcess_ValidateDataStructure;
  const aiScene* ai = importer.ReadFile(path.u8string().c_str(), flags);
  if (!ai || !ai->mRootNode) {
    std::string why = !io->denied.empty() ? "access denied: " + io->denied : importer.GetErrorString();
    return Result::error(io->denied.empty() ? Status::Unreadable : Status::AccessDenied, "This model can't be read: " + why);
  }
  if (ai->mFlags & AI_SCENE_FLAGS_INCOMPLETE)
    return Result::error(Status::Unreadable, "This model can't be read: the file holds no meshes");

  Importer im{ctx, ai, path.parent_path(), scene, {}, {}};
  for (unsigned i = 0; i < ai->mNumMaterials; ++i) im.material(ai->mMaterials[i]);
  im.meshMap.assign(ai->mNumMeshes, -1);
  for (unsigned i = 0; i < ai->mNumMeshes; ++i) {
    const aiMesh* m = ai->mMeshes[i];
    if (!(m->mPrimitiveTypes & aiPrimitiveType_TRIANGLE)) continue;
    SceneMesh out;
    out.name = m->mName.C_Str();
    out.material = int(m->mMaterialIndex);
    out.positions.resize(size_t(m->mNumVertices) * 3);
    for (unsigned v = 0; v < m->mNumVertices; ++v) {
      out.positions[v * 3] = m->mVertices[v].x;
      out.positions[v * 3 + 1] = m->mVertices[v].y;
      out.positions[v * 3 + 2] = m->mVertices[v].z;
    }
    if (m->mNormals) {
      out.normals.resize(out.positions.size());
      for (unsigned v = 0; v < m->mNumVertices; ++v) {
        out.normals[v * 3] = m->mNormals[v].x;
        out.normals[v * 3 + 1] = m->mNormals[v].y;
        out.normals[v * 3 + 2] = m->mNormals[v].z;
      }
    }
    if (m->mTextureCoords[0]) {
      out.uvs.resize(size_t(m->mNumVertices) * 2);
      for (unsigned v = 0; v < m->mNumVertices; ++v) {
        out.uvs[v * 2] = m->mTextureCoords[0][v].x;
        out.uvs[v * 2 + 1] = m->mTextureCoords[0][v].y;
      }
    }
    if (m->mColors[0]) {
      out.colors.resize(size_t(m->mNumVertices) * 4);
      for (unsigned v = 0; v < m->mNumVertices; ++v) {
        out.colors[v * 4] = m->mColors[0][v].r;
        out.colors[v * 4 + 1] = m->mColors[0][v].g;
        out.colors[v * 4 + 2] = m->mColors[0][v].b;
        out.colors[v * 4 + 3] = m->mColors[0][v].a;
      }
    }
    for (unsigned f = 0; f < m->mNumFaces; ++f) {
      const aiFace& face = m->mFaces[f];
      if (face.mNumIndices != 3) continue;
      out.indices.insert(out.indices.end(), face.mIndices, face.mIndices + 3);
    }
    if (out.indices.empty()) continue;
    im.meshMap[i] = int(scene.meshes.size());
    scene.meshes.push_back(std::move(out));
  }
  im.node(ai->mRootNode);

  scene.upAxis = "Y";
  scene.metersPerUnit = 1.0;
  scene.unitsKnown = format == "gltf" || format == "glb";
  if (ai->mMetaData) {
    int up = 1;
    if (ai->mMetaData->Get("UpAxis", up)) scene.upAxis = up == 2 ? "Z" : (up == 0 ? "X" : "Y");
    double unit = 0;
    float unitF = 0;
    if (ai->mMetaData->Get("UnitScaleFactor", unit) || (ai->mMetaData->Get("UnitScaleFactor", unitF) && (unit = unitF))) {
      if (format == "fbx" && unit > 0) {
        scene.metersPerUnit = unit / 100.0;
        scene.unitsKnown = true;
      }
    }
  }
  return Result::ok();
}

namespace {

void addTex(aiMaterial* m, aiTextureType type, const std::string& path) {
  aiString s(path);
  m->AddProperty(&s, AI_MATKEY_TEXTURE(type, 0));
}

}  // namespace

Result saveAssimp(const Context& ctx, const Scene& scene, const fs::path& path, const std::string& format) {
  const bool gltf = format == "gltf" || format == "glb";
  const bool obj = format == "obj";
  const bool withMaterials = gltf || obj;

  std::vector<std::string> texPath(scene.textures.size());
  std::vector<std::pair<fs::path, const Bytes*>> sideFiles;
  if (obj && ctx.options->model.textures) {
    std::map<std::string, int> used;
    for (size_t i = 0; i < scene.textures.size(); ++i) {
      const auto& t = scene.textures[i];
      std::string ext = t.format == "jpeg" ? "jpg" : t.format;
      std::string base = path.stem().string() + "_" + (t.name.empty() ? "tex" : t.name);
      for (auto& ch : base)
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-') ch = '_';
      std::string name = base + "." + ext;
      if (int n = used[name]++) name = base + "_" + std::to_string(n) + "." + ext;
      texPath[i] = name;
      sideFiles.emplace_back(path.parent_path() / name, &t.data);
    }
  }

  auto ai = std::make_unique<aiScene>();
  if (withMaterials && ctx.options->model.textures && gltf && !scene.textures.empty()) {
    ai->mNumTextures = unsigned(scene.textures.size());
    ai->mTextures = new aiTexture*[ai->mNumTextures];
    for (size_t i = 0; i < scene.textures.size(); ++i) {
      const auto& t = scene.textures[i];
      auto* at = new aiTexture();
      at->mWidth = unsigned(t.data.size());
      at->mHeight = 0;
      at->pcData = new aiTexel[t.data.size() / sizeof(aiTexel) + 1];
      std::memcpy(at->pcData, t.data.data(), t.data.size());
      std::string hint = t.format == "jpeg" ? "jpg" : t.format;
      std::strncpy(at->achFormatHint, hint.c_str(), HINTMAXTEXTURELEN - 1);
      at->mFilename = aiString(t.name);
      ai->mTextures[i] = at;
      texPath[i] = "*" + std::to_string(i);
    }
  }

  const size_t nMat = std::max<size_t>(1, scene.materials.size());
  ai->mNumMaterials = unsigned(nMat);
  ai->mMaterials = new aiMaterial*[nMat];
  for (size_t i = 0; i < nMat; ++i) {
    auto* m = new aiMaterial();
    ai->mMaterials[i] = m;
    if (i >= scene.materials.size()) {
      aiString name("default");
      m->AddProperty(&name, AI_MATKEY_NAME);
      continue;
    }
    const auto& sm = scene.materials[i];
    aiString name(sm.name.empty() ? "material_" + std::to_string(i) : sm.name);
    m->AddProperty(&name, AI_MATKEY_NAME);
    aiColor4D base(sm.baseColor[0], sm.baseColor[1], sm.baseColor[2], sm.opacity);
    m->AddProperty(&base, 1, AI_MATKEY_BASE_COLOR);
    aiColor3D diffuse(sm.baseColor[0], sm.baseColor[1], sm.baseColor[2]);
    m->AddProperty(&diffuse, 1, AI_MATKEY_COLOR_DIFFUSE);
    float metallic = sm.metallic, roughness = sm.roughness, opacity = sm.opacity;
    m->AddProperty(&metallic, 1, AI_MATKEY_METALLIC_FACTOR);
    m->AddProperty(&roughness, 1, AI_MATKEY_ROUGHNESS_FACTOR);
    float shininess = std::max(0.0f, 2.0f / std::max(roughness * roughness, 1e-4f) - 2.0f);
    m->AddProperty(&shininess, 1, AI_MATKEY_SHININESS);
    aiColor3D emissive(sm.emissive[0], sm.emissive[1], sm.emissive[2]);
    m->AddProperty(&emissive, 1, AI_MATKEY_COLOR_EMISSIVE);
    m->AddProperty(&opacity, 1, AI_MATKEY_OPACITY);
    int two = sm.doubleSided ? 1 : 0;
    m->AddProperty(&two, 1, AI_MATKEY_TWOSIDED);
    if (gltf) {
      aiString mode(sm.blend ? "BLEND" : (sm.opacityThreshold > 0 ? "MASK" : "OPAQUE"));
      m->AddProperty(&mode, AI_MATKEY_GLTF_ALPHAMODE);
      if (sm.opacityThreshold > 0) {
        float cut = sm.opacityThreshold;
        m->AddProperty(&cut, 1, AI_MATKEY_GLTF_ALPHACUTOFF);
      }
    }
    if (!ctx.options->model.textures || !withMaterials) continue;
    auto put = [&](const TexRef& r, aiTextureType type) {
      if (r.valid() && size_t(r.texture) < texPath.size() && !texPath[size_t(r.texture)].empty())
        addTex(m, type, texPath[size_t(r.texture)]);
    };
    put(sm.baseColorTex, aiTextureType_BASE_COLOR);
    put(sm.baseColorTex, aiTextureType_DIFFUSE);
    put(sm.normalTex, aiTextureType_NORMALS);
    put(sm.emissiveTex, aiTextureType_EMISSIVE);
    if (gltf) {
      put(sm.occlusionTex, aiTextureType_LIGHTMAP);
      if (sm.roughnessTex.valid()) put(sm.roughnessTex, aiTextureType_DIFFUSE_ROUGHNESS);
      else put(sm.metallicTex, aiTextureType_DIFFUSE_ROUGHNESS);
    } else {
      if (sm.opacityTex.valid() && sm.opacityTex.texture != sm.baseColorTex.texture) put(sm.opacityTex, aiTextureType_OPACITY);
      put(sm.roughnessTex, aiTextureType_DIFFUSE_ROUGHNESS);
      put(sm.metallicTex, aiTextureType_METALNESS);
    }
  }

  ai->mNumMeshes = unsigned(scene.meshes.size());
  ai->mMeshes = new aiMesh*[std::max<size_t>(1, scene.meshes.size())];
  for (size_t i = 0; i < scene.meshes.size(); ++i) {
    const auto& sm = scene.meshes[i];
    auto* m = new aiMesh();
    ai->mMeshes[i] = m;
    m->mName = aiString(sm.name);
    m->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    m->mMaterialIndex = sm.material >= 0 && size_t(sm.material) < scene.materials.size() ? unsigned(sm.material) : 0;
    const unsigned nv = unsigned(sm.vertexCount());
    m->mNumVertices = nv;
    m->mVertices = new aiVector3D[nv];
    for (unsigned v = 0; v < nv; ++v) m->mVertices[v] = aiVector3D(sm.positions[v * 3], sm.positions[v * 3 + 1], sm.positions[v * 3 + 2]);
    if (sm.normals.size() == sm.positions.size()) {
      m->mNormals = new aiVector3D[nv];
      for (unsigned v = 0; v < nv; ++v) m->mNormals[v] = aiVector3D(sm.normals[v * 3], sm.normals[v * 3 + 1], sm.normals[v * 3 + 2]);
    }
    if (sm.uvs.size() == size_t(nv) * 2 && withMaterials) {
      m->mTextureCoords[0] = new aiVector3D[nv];
      m->mNumUVComponents[0] = 2;
      for (unsigned v = 0; v < nv; ++v) m->mTextureCoords[0][v] = aiVector3D(sm.uvs[v * 2], sm.uvs[v * 2 + 1], 0);
    }
    if (sm.colors.size() == size_t(nv) * 4 && format != "stl") {
      m->mColors[0] = new aiColor4D[nv];
      for (unsigned v = 0; v < nv; ++v)
        m->mColors[0][v] = aiColor4D(sm.colors[v * 4], sm.colors[v * 4 + 1], sm.colors[v * 4 + 2], sm.colors[v * 4 + 3]);
    }
    m->mNumFaces = unsigned(sm.indices.size() / 3);
    m->mFaces = new aiFace[m->mNumFaces];
    for (unsigned f = 0; f < m->mNumFaces; ++f) {
      m->mFaces[f].mNumIndices = 3;
      m->mFaces[f].mIndices = new unsigned[3]{sm.indices[f * 3], sm.indices[f * 3 + 1], sm.indices[f * 3 + 2]};
    }
  }

  std::function<aiNode*(int)> build = [&](int idx) -> aiNode* {
    const auto& sn = scene.nodes[size_t(idx)];
    auto* n = new aiNode(sn.name.empty() ? "node" + std::to_string(idx) : sn.name);
    n->mTransformation = toAi(sn.matrix);
    if (!sn.meshes.empty()) {
      n->mNumMeshes = unsigned(sn.meshes.size());
      n->mMeshes = new unsigned[n->mNumMeshes];
      for (size_t k = 0; k < sn.meshes.size(); ++k) n->mMeshes[k] = unsigned(sn.meshes[k]);
    }
    if (!sn.children.empty()) {
      n->mNumChildren = unsigned(sn.children.size());
      n->mChildren = new aiNode*[n->mNumChildren];
      for (size_t k = 0; k < sn.children.size(); ++k) {
        n->mChildren[k] = build(sn.children[k]);
        n->mChildren[k]->mParent = n;
      }
    }
    return n;
  };
  ai->mRootNode = scene.nodes.empty() ? new aiNode("root") : build(0);

  const char* id = format == "glb" ? "glb2" : format == "gltf" ? "gltf2" : format == "stl" ? "stlb"
                 : format == "ply" ? "plyb" : "obj";
  unsigned pp = 0;
  if (format == "obj" || format == "stl" || format == "ply") pp = aiProcess_PreTransformVertices;

  std::vector<fs::path> written;
  Assimp::Exporter exporter;
  auto* io = new GuardedIO(ctx.io, &written);
  exporter.SetIOHandler(io);
  aiReturn rc = exporter.Export(ai.get(), id, path.u8string().c_str(), pp);
  auto cleanup = [&] {
    std::error_code ec;
    for (auto& w : written) fs::remove(w, ec);
  };
  if (rc != aiReturn_SUCCESS) {
    std::string why = !io->denied.empty() ? "access denied: " + io->denied : exporter.GetErrorString();
    cleanup();
    return Result::error(io->denied.empty() ? Status::Failed : Status::AccessDenied, "Export failed: " + why);
  }
  for (auto& [p, data] : sideFiles) {
    if (auto r = ctx.io.writeFile(p, *data); !r) {
      cleanup();
      return r;
    }
    written.push_back(p);
  }
  if (ctx.written)
    for (auto& w : written) ctx.written->push_back(w);
  return Result::ok();
}

}  // namespace jowe::convert::detail
