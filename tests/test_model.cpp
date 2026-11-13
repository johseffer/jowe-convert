#include "model/scene.hpp"
#include "test_util.hpp"

using namespace jowe::convert::detail;

namespace {

bool checkUsdzLayout(const fs::path& p, size_t* textures = nullptr) {
  Bytes zip = readAll(p);
  std::vector<ZipEntry> entries;
  std::string err;
  if (!listZip(zip, entries, err)) {
    std::fprintf(stderr, "  zip: %s\n", err.c_str());
    return false;
  }
  if (entries.empty()) return false;
  const std::string first = entries[0].name;
  if (first.size() < 5 || first.substr(first.size() - 5) != ".usda") {
    std::fprintf(stderr, "  first entry is %s\n", first.c_str());
    return false;
  }
  size_t tex = 0;
  for (auto& e : entries) {
    if (e.method != 0) {
      std::fprintf(stderr, "  %s is compressed (method %u)\n", e.name.c_str(), e.method);
      return false;
    }
    if (e.dataOffset % 64 != 0) {
      std::fprintf(stderr, "  %s data at %llu is not 64-byte aligned\n", e.name.c_str(),
                   static_cast<unsigned long long>(e.dataOffset));
      return false;
    }
    if (crc32(zip.data() + e.dataOffset, size_t(e.size)) != e.crc) {
      std::fprintf(stderr, "  %s CRC mismatch\n", e.name.c_str());
      return false;
    }
    if (e.name.rfind("textures/", 0) == 0) ++tex;
  }
  if (textures) *textures = tex;
  return true;
}

std::string usdaInside(const fs::path& usdz) {
  Bytes zip = readAll(usdz);
  std::vector<ZipEntry> entries;
  std::string err;
  if (!listZip(zip, entries, err) || entries.empty()) return {};
  return std::string(reinterpret_cast<const char*>(zip.data() + entries[0].dataOffset), size_t(entries[0].size));
}

}  // namespace

TEST(model_obj_usdz_glb_roundtrip) {
  fs::path dir = tempDir() / "model";
  makeTexturedObj(dir, "box");
  auto src = jc::probe(dir / "box.obj");
  REQUIRE(src.result);
  CHECK(src.info.model.materials >= 2);
  CHECK(src.info.model.textures == 5);
  CHECK(src.info.model.triangles == 12);

  auto r1 = jc::convert(dir / "box.obj", dir / "box.usdz");
  CHECK_OK(r1);
  size_t tex = 0;
  CHECK(checkUsdzLayout(dir / "box.usdz", &tex));
  CHECK(tex == 5);
  CHECK(r1.output.model.triangles == 12);
  CHECK(r1.output.model.textures == 5);
  CHECK(r1.output.model.materials >= 2);
  std::string usda = usdaInside(dir / "box.usdz");
  for (const char* needle : {"UsdPreviewSurface", "inputs:diffuseColor.connect", "inputs:normal.connect",
                             "inputs:roughness.connect", "inputs:metallic.connect", "inputs:emissiveColor.connect",
                             "UsdPrimvarReader_float2", "metersPerUnit", "upAxis = \"Y\"", "primvars:st"})
    if (usda.find(needle) == std::string::npos) {
      std::fprintf(stderr, "  usda lacks %s\n", needle);
      ++g_failures;
    }

  auto r2 = jc::convert(dir / "box.usdz", dir / "box.glb");
  CHECK_OK(r2);
  CHECK(r2.output.model.triangles == 12);
  CHECK(r2.output.model.textures >= 4);
  CHECK(r2.output.model.materials >= 2);

  auto r3 = jc::convert(dir / "box.glb", dir / "box2.usdz");
  CHECK_OK(r3);
  CHECK(checkUsdzLayout(dir / "box2.usdz"));
  CHECK(r3.output.model.triangles == 12);
  CHECK(r3.output.model.textures >= 4);
  for (int i = 0; i < 3; ++i) {
    CHECK(std::fabs(r3.output.model.boundsMin[i] + 1) < 1e-4);
    CHECK(std::fabs(r3.output.model.boundsMax[i] - 1) < 1e-4);
  }
}

TEST(model_all_writers) {
  fs::path dir = tempDir() / "writers";
  makeTexturedObj(dir, "box");
  for (const char* ext : {"glb", "gltf", "obj", "stl", "ply", "usda", "usd", "usdz"}) {
    fs::path out = dir / (std::string("out.") + ext);
    auto rep = jc::convert(dir / "box.obj", out);
    CHECK_OK(rep);
    if (!rep.result) continue;
    CHECK(rep.output.model.triangles == 12);
    CHECK(std::fabs(rep.output.model.boundsMax[0] - 1) < 1e-4);
    auto back = jc::convert(out, dir / (std::string("back_") + ext + ".glb"));
    CHECK_OK(back);
    CHECK(back.output.model.triangles == 12);
  }
  auto stl = jc::convert(dir / "out.stl", dir / "from_stl.usdz");
  CHECK_OK(stl);
  CHECK(checkUsdzLayout(dir / "from_stl.usdz"));
}

TEST(model_units_and_axis) {
  fs::path dir = tempDir() / "units";
  fs::create_directories(dir);
  writeText(dir / "zup.usda",
            "#usda 1.0\n(\n    defaultPrim = \"Root\"\n    metersPerUnit = 0.01\n    upAxis = \"Z\"\n)\n"
            "def Xform \"Root\"\n{\n    def Mesh \"Tri\"\n    {\n"
            "        int[] faceVertexCounts = [3]\n        int[] faceVertexIndices = [0, 1, 2]\n"
            "        point3f[] points = [(0, 0, 0), (100, 0, 0), (0, 0, 200)]\n    }\n}\n");
  auto p = jc::probe(dir / "zup.usda");
  REQUIRE(p.result);
  CHECK(p.info.model.upAxis == "Z");
  CHECK(std::fabs(p.info.model.metersPerUnit - 0.01) < 1e-9);

  auto usdz = jc::convert(dir / "zup.usda", dir / "zup.usdz");
  CHECK_OK(usdz);
  CHECK(usdz.output.model.upAxis == "Z");
  CHECK(std::fabs(usdz.output.model.metersPerUnit - 0.01) < 1e-9);
  CHECK(std::fabs(usdz.output.model.boundsMax[2] - 200) < 1e-3);

  auto glb = jc::convert(dir / "zup.usda", dir / "zup.glb");
  CHECK_OK(glb);
  CHECK(std::fabs(glb.output.model.boundsMax[1] - 2.0) < 1e-4);
  CHECK(std::fabs(glb.output.model.boundsMax[0] - 1.0) < 1e-4);

  jc::ConvertOptions o;
  o.model.upAxis = jc::ModelOptions::UpAxis::ZToY;
  o.model.scale = 0.5;
  auto rot = jc::convert(dir / "zup.usda", dir / "rot.usda", o);
  CHECK_OK(rot);
  CHECK(rot.output.model.upAxis == "Y");
  CHECK(std::fabs(rot.output.model.boundsMax[1] - 100) < 1e-3);
}

TEST(model_no_textures_and_errors) {
  fs::path dir = tempDir() / "notex";
  makeTexturedObj(dir, "box");
  jc::ConvertOptions o;
  o.model.textures = false;
  auto rep = jc::convert(dir / "box.obj", dir / "plain.usdz", o);
  CHECK_OK(rep);
  size_t tex = 99;
  CHECK(checkUsdzLayout(dir / "plain.usdz", &tex));
  CHECK(tex == 0);
  rep = jc::convert(dir / "box.obj", dir / "x.fbx");
  CHECK(rep.result.status == jc::Status::NotWritable);
  rep = jc::convert(dir / "box.obj", dir / "x.usdc");
  CHECK(rep.result.status == jc::Status::NotWritable);
  writeText(dir / "broken.glb", "glTF garbage");
  rep = jc::convert(dir / "broken.glb", dir / "y.usdz");
  CHECK(rep.result.status == jc::Status::Unreadable);
  CHECK(!fs::exists(dir / "y.usdz"));

  jc::ConvertOptions guarded;
  guarded.access = [&](const fs::path& p, bool) { return p.filename().string().find("albedo") == std::string::npos; };
  rep = jc::convert(dir / "box.obj", dir / "guarded.usdz", guarded);
  CHECK_OK(rep);
  CHECK(rep.output.model.textures == 4);
}
