#include <jowe/convert.hpp>

#include <cstdio>
#include <fstream>

namespace jc = jowe::convert;

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  std::filesystem::path dir = argv[1];
  std::ofstream(dir / "tri.usda") << "#usda 1.0\ndef Mesh \"Tri\"\n{\n    int[] faceVertexCounts = [3]\n"
                                     "    int[] faceVertexIndices = [0, 1, 2]\n"
                                     "    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n}\n";
  int writable = 0;
  for (auto& f : jc::listFormats()) writable += f.canWrite;
  auto rep = jc::convert(dir / "tri.usda", dir / "tri.usdz");
  std::printf("jowe-convert %s: %d writable formats, usda->usdz %s (%llu triangles)\n", jc::version(), writable,
              jc::toString(rep.result.status), static_cast<unsigned long long>(rep.output.model.triangles));
  return rep.result && rep.output.model.triangles == 1 ? 0 : 1;
}
