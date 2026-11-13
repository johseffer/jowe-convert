#pragma once

#include <array>

#include "../core/common.hpp"

namespace jowe::convert::detail {

struct SceneTexture {
  std::string name;
  std::string format;
  Bytes data;
};

// A texture input. channel is "rgb", "rgba", "r", "g", "b" or "a".
struct TexRef {
  int texture = -1;
  std::string channel = "rgb";
  bool valid() const { return texture >= 0; }
};

struct SceneMaterial {
  std::string name;
  std::array<float, 4> baseColor{0.8f, 0.8f, 0.8f, 1.0f};
  TexRef baseColorTex;
  float metallic = 0.0f;
  TexRef metallicTex;
  float roughness = 0.5f;
  TexRef roughnessTex;
  TexRef normalTex;
  std::array<float, 3> emissive{0, 0, 0};
  TexRef emissiveTex;
  float opacity = 1.0f;
  TexRef opacityTex;
  float opacityThreshold = 0.0f;
  TexRef occlusionTex;
  bool doubleSided = false;
  bool blend = false;
};

struct SceneMesh {
  std::string name;
  std::vector<float> positions;
  std::vector<float> normals;
  std::vector<float> uvs;
  std::vector<float> colors;
  std::vector<uint32_t> indices;
  int material = -1;

  size_t vertexCount() const { return positions.size() / 3; }
};

// Row-vector convention, row-major storage (USD layout; translation in m[12..14]).
using Matrix = std::array<double, 16>;
Matrix identityMatrix();
Matrix multiply(const Matrix& a, const Matrix& b);

struct SceneNode {
  std::string name;
  Matrix matrix = identityMatrix();
  std::vector<int> meshes;
  std::vector<int> children;
};

struct Scene {
  std::vector<SceneNode> nodes;
  std::vector<SceneMesh> meshes;
  std::vector<SceneMaterial> materials;
  std::vector<SceneTexture> textures;
  std::string upAxis = "Y";
  double metersPerUnit = 1.0;
  bool unitsKnown = false;
};

ModelStats sceneStats(const Scene& s);

Result loadAssimp(const Context& ctx, const fs::path& path, const std::string& format, Scene& scene);
Result saveAssimp(const Context& ctx, const Scene& scene, const fs::path& path, const std::string& format);

Result loadUsd(const Context& ctx, const fs::path& path, const std::string& format, Scene& scene);
Result saveUsda(const Context& ctx, const Scene& scene, const fs::path& path);
Result saveUsdz(const Context& ctx, const Scene& scene, const fs::path& path);
std::string buildUsda(const Scene& scene, const std::vector<std::string>& texturePaths);

struct ZipEntry {
  std::string name;
  uint64_t dataOffset = 0;
  uint64_t size = 0;
  uint16_t method = 0;
  uint32_t crc = 0;
};
Bytes buildStoredZip(const std::vector<std::pair<std::string, const Bytes*>>& files, size_t alignment);
bool listZip(const Bytes& zip, std::vector<ZipEntry>& entries, std::string& err);

Result probeModel(const Context& ctx, const fs::path& path, const std::string& format, ProbeInfo& info);
Result convertModel(const Context& ctx, const fs::path& in, const std::string& inFormat, const fs::path& out,
                    const std::string& outFormat, ProbeInfo& outInfo);

}  // namespace jowe::convert::detail
