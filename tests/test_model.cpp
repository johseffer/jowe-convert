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
  CHECK(p.info.model.sourceUpAxis == "Z");
  CHECK(std::fabs(p.info.model.sourceMetersPerUnit - 0.01) < 1e-9);
  CHECK(std::fabs(p.info.model.boundsMax[1] - 2.0) < 1e-4);
  CHECK(std::fabs(p.info.model.boundsMax[0] - 1.0) < 1e-4);

  auto usdz = jc::convert(dir / "zup.usda", dir / "zup.usdz");
  CHECK_OK(usdz);
  CHECK(usdz.output.model.sourceUpAxis == "Z");
  CHECK(std::fabs(usdz.output.model.sourceMetersPerUnit - 0.01) < 1e-9);
  CHECK(std::fabs(usdz.output.model.boundsMax[1] - 2.0) < 1e-4);

  auto glb = jc::convert(dir / "zup.usda", dir / "zup.glb");
  CHECK_OK(glb);
  CHECK(std::fabs(glb.output.model.boundsMax[1] - 2.0) < 1e-4);
  CHECK(std::fabs(glb.output.model.boundsMax[0] - 1.0) < 1e-4);

  jc::ConvertOptions o;
  o.model.upAxis = jc::ModelOptions::UpAxis::ZToY;
  o.model.scale = 0.5;
  auto rot = jc::convert(dir / "zup.usda", dir / "rot.usda", o);
  CHECK_OK(rot);
  CHECK(rot.output.model.sourceUpAxis == "Y");
  CHECK(std::fabs(rot.output.model.boundsMax[1] - 1.0) < 1e-4);
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

namespace {

const char* kColor4Usda =
    "#usda 1.0\n(\n    defaultPrim = \"Root\"\n    metersPerUnit = 1\n    upAxis = \"Y\"\n)\n"
    "def Xform \"Root\"\n{\n"
    "    def Mesh \"Tri\" (\n        prepend apiSchemas = [\"MaterialBindingAPI\"]\n    )\n    {\n"
    "        int[] faceVertexCounts = [3]\n        int[] faceVertexIndices = [0, 1, 2]\n"
    "        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
    "        rel material:binding = </Root/Mat>\n    }\n"
    "    def Material \"Mat\"\n    {\n"
    "        token outputs:surface.connect = </Root/Mat/PS.outputs:surface>\n"
    "        def Shader \"PS\"\n        {\n"
    "            uniform token info:id = \"UsdPreviewSurface\"\n"
    "            color4f inputs:diffuseColor = (0.5, 0.25, 0.125, 1)\n"
    "            color4f inputs:emissiveColor = (1, 0.5, 0, 1)\n"
    "            float4 inputs:specularColor = (0.2, 0.2, 0.2, 1)\n"
    "            float inputs:opacity = 0.5\n"
    "            token outputs:surface\n        }\n    }\n}\n";

void expectColorMaterial(const fs::path& usda) {
  auto bytes = readAll(usda);
  std::string text(bytes.begin(), bytes.end());
  for (const char* needle : {"inputs:diffuseColor = (0.5, 0.25, 0.125)", "inputs:emissiveColor = (1, 0.5, 0)",
                             "float inputs:opacity = 0.5"})
    if (text.find(needle) == std::string::npos) {
      std::fprintf(stderr, "  %s lacks %s\n", usda.filename().string().c_str(), needle);
      ++g_failures;
    }
}

}  // namespace

TEST(model_usd_color4f_inputs) {
  fs::path dir = tempDir() / "color4f";
  fs::create_directories(dir);
  writeText(dir / "modelio.usda", kColor4Usda);
  auto p = jc::probe(dir / "modelio.usda");
  REQUIRE(p.result);
  CHECK(p.info.model.materials == 1);
  CHECK(p.info.model.triangles == 1);
  auto rep = jc::convert(dir / "modelio.usda", dir / "fixed.usda");
  CHECK_OK(rep);
  expectColorMaterial(dir / "fixed.usda");

  Bytes layer(kColor4Usda, kColor4Usda + std::strlen(kColor4Usda));
  Bytes zip = buildStoredZip({{"modelio.usda", &layer}}, 64);
  writeAll(dir / "modelio.usdz", zip.data(), zip.size());
  auto z = jc::convert(dir / "modelio.usdz", dir / "from_usdz.usda");
  CHECK_OK(z);
  expectColorMaterial(dir / "from_usdz.usda");
  auto glb = jc::convert(dir / "modelio.usdz", dir / "from_usdz.glb");
  CHECK_OK(glb);
  CHECK(glb.output.model.materials >= 1);
}

TEST(model_fbx_cm_probe_matches_convert) {
  fs::path dir = tempDir() / "fbxcm";
  fs::create_directories(dir);
  writeText(dir / "tri_cm.fbx",
            "; FBX 7.4.0 project file\n"
            "FBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n}\n"
            "GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n"
            "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
            "\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
            "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n"
            "\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
            "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
            "\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
            "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n\t}\n}\n"
            "Objects:  {\n"
            "\tGeometry: 1000, \"Geometry::Tri\", \"Mesh\" {\n"
            "\t\tVertices: *9 {\n\t\t\ta: 0,0,0,200,0,0,0,150,0\n\t\t}\n"
            "\t\tPolygonVertexIndex: *3 {\n\t\t\ta: 0,1,-3\n\t\t}\n\t\tGeometryVersion: 124\n\t}\n"
            "\tModel: 2000, \"Model::Tri\", \"Mesh\" {\n\t\tVersion: 232\n\t\tProperties70:  {\n\t\t}\n\t}\n}\n"
            "Connections:  {\n\tC: \"OO\",1000,2000\n\tC: \"OO\",2000,0\n}\n");
  auto p = jc::probe(dir / "tri_cm.fbx");
  REQUIRE(p.result);
  CHECK(std::fabs(p.info.model.sourceMetersPerUnit - 0.01) < 1e-9);
  CHECK(p.info.model.sourceUpAxis == "Y");
  CHECK(std::fabs(p.info.model.boundsMax[0] - 2.0f) < 1e-4);
  CHECK(std::fabs(p.info.model.boundsMax[1] - 1.5f) < 1e-4);
  for (const char* ext : {"glb", "usdz", "usda"}) {
    auto rep = jc::convert(dir / "tri_cm.fbx", dir / (std::string("tri.") + ext));
    CHECK_OK(rep);
    for (int i = 0; i < 3; ++i) {
      CHECK(std::fabs(rep.input.model.boundsMax[i] - p.info.model.boundsMax[i]) < 1e-4);
      CHECK(std::fabs(rep.output.model.boundsMax[i] - p.info.model.boundsMax[i]) < 1e-4);
      CHECK(std::fabs(rep.output.model.boundsMin[i] - p.info.model.boundsMin[i]) < 1e-4);
    }
  }
}

#ifndef JC_FIXTURES
#define JC_FIXTURES ""
#endif

namespace {

Scene loadForTest(const fs::path& p) {
  jc::ConvertOptions opts;
  Context ctx;
  ctx.options = &opts;
  Scene scene;
  std::string ext = extensionOf(p);
  jc::Result r = (ext == "usda" || ext == "usdc" || ext == "usdz" || ext == "usd") ? loadUsd(ctx, p, ext, scene)
                                                                               : loadAssimp(ctx, p, ext, scene);
  if (!r) {
    std::fprintf(stderr, "  load %s: %s\n", p.string().c_str(), r.message.c_str());
    ++g_failures;
  }
  return scene;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

void expectTriMat(const fs::path& p, bool gltf) {
  Scene s = loadForTest(p);
  if (s.meshes.empty()) {
    std::fprintf(stderr, "  %s has no mesh\n", p.string().c_str());
    ++g_failures;
    return;
  }
  const int mi = s.meshes[0].material;
  if (mi < 0 || size_t(mi) >= s.materials.size()) {
    std::fprintf(stderr, "  %s: mesh has no material\n", p.string().c_str());
    ++g_failures;
    return;
  }
  const SceneMaterial& m = s.materials[size_t(mi)];
  const bool ok = near(m.baseColor[0], 0.8f) && near(m.baseColor[1], 0.2f) && near(m.baseColor[2], 0.1f) &&
                  near(m.emissive[0], 0.1f) && near(m.emissive[1], 0.3f) && near(m.emissive[2], 0.9f) &&
                  near(m.opacity, 0.5f) && (gltf ? m.blend : true);
  if (!ok) {
    std::fprintf(stderr, "  %s: base (%g %g %g %g) emissive (%g %g %g) opacity %g blend %d\n", p.string().c_str(),
                 m.baseColor[0], m.baseColor[1], m.baseColor[2], m.baseColor[3], m.emissive[0], m.emissive[1],
                 m.emissive[2], m.opacity, int(m.blend));
    ++g_failures;
  }
  CHECK(s.meshes.size() == 1);
  CHECK(s.meshes[0].indices.size() == 3);
}

}  // namespace

TEST(model_modelio_usdc_color4f) {
  const fs::path fixtures = JC_FIXTURES;
  fs::path dir = tempDir() / "modelio";
  fs::create_directories(dir);
  for (const char* name : {"modelio_triangle_color4f.usdc", "modelio_triangle_color4f.usda"}) {
    fs::path src = fixtures / name;
    REQUIRE(fs::exists(src));
    auto p = jc::probe(src);
    REQUIRE(p.result);
    CHECK(p.info.model.triangles == 1);
    CHECK(p.info.model.materials == 1);
    CHECK(near(p.info.model.boundsMax[0], 1.0f) && near(p.info.model.boundsMax[1], 1.0f));
    expectTriMat(src, false);
    std::string stem = fs::path(name).extension().string().substr(1);
    for (const char* ext : {"usda", "glb"}) {
      fs::path out = dir / (stem + "." + ext);
      auto rep = jc::convert(src, out);
      CHECK_OK(rep);
      if (!rep.result) continue;
      CHECK(rep.output.model.triangles == 1);
      expectTriMat(out, std::string(ext) == "glb");
    }
  }
}
