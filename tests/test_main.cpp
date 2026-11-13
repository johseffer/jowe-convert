#include <chrono>
#include <cstdlib>

#include "test_util.hpp"

int g_failures = 0;

std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

fs::path tempDir() {
  static fs::path dir = [] {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path d = fs::temp_directory_path() / ("jc_tests_" + std::to_string(stamp));
    fs::create_directories(d);
    return d;
  }();
  return dir;
}

static void putLe(std::vector<uint8_t>& b, uint32_t v, int n) {
  for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

void makeWav(const fs::path& p, int rate, int channels, double seconds, int bits) {
  const uint32_t frames = uint32_t(rate * seconds);
  const int bytes = bits / 8;
  std::vector<uint8_t> b;
  b.insert(b.end(), {'R', 'I', 'F', 'F'});
  putLe(b, 36 + frames * channels * bytes, 4);
  b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  putLe(b, 16, 4);
  putLe(b, 1, 2);
  putLe(b, uint32_t(channels), 2);
  putLe(b, uint32_t(rate), 4);
  putLe(b, uint32_t(rate * channels * bytes), 4);
  putLe(b, uint32_t(channels * bytes), 2);
  putLe(b, uint32_t(bits), 2);
  b.insert(b.end(), {'d', 'a', 't', 'a'});
  putLe(b, frames * channels * bytes, 4);
  const double scale = double((1u << (bits - 1)) - 1);
  for (uint32_t i = 0; i < frames; ++i)
    for (int c = 0; c < channels; ++c) {
      double v = 0.5 * std::sin(2 * 3.14159265358979 * (440.0 * (c + 1)) * i / rate);
      putLe(b, uint32_t(int32_t(std::lround(v * scale))), bytes);
    }
  writeAll(p, b.data(), b.size());
}

static std::vector<uint8_t> bmp(int w, int h, const std::function<void(int, int, uint8_t*)>& px) {
  std::vector<uint8_t> b;
  const uint32_t size = uint32_t(w * h * 4);
  b.insert(b.end(), {'B', 'M'});
  putLe(b, 54 + 68 + size, 4);
  putLe(b, 0, 4);
  putLe(b, 54 + 68, 4);
  putLe(b, 40 + 68, 4);
  putLe(b, uint32_t(w), 4);
  putLe(b, uint32_t(h), 4);
  putLe(b, 1, 2);
  putLe(b, 32, 2);
  putLe(b, 3, 4);
  putLe(b, size, 4);
  putLe(b, 2835, 4);
  putLe(b, 2835, 4);
  putLe(b, 0, 4);
  putLe(b, 0, 4);
  putLe(b, 0x00FF0000, 4);
  putLe(b, 0x0000FF00, 4);
  putLe(b, 0x000000FF, 4);
  putLe(b, 0xFF000000, 4);
  b.insert(b.end(), 52, 0);
  for (int y = h - 1; y >= 0; --y)
    for (int x = 0; x < w; ++x) {
      uint8_t rgba[4];
      px(x, y, rgba);
      b.push_back(rgba[2]);
      b.push_back(rgba[1]);
      b.push_back(rgba[0]);
      b.push_back(rgba[3]);
    }
  return b;
}

void makePng(const fs::path& p, int w, int h, bool alpha) {
  auto data = bmp(w, h, [&](int x, int y, uint8_t* o) {
    o[0] = uint8_t(x * 255 / std::max(1, w - 1));
    o[1] = uint8_t(y * 255 / std::max(1, h - 1));
    o[2] = uint8_t(((x / 8) + (y / 8)) % 2 ? 200 : 40);
    o[3] = alpha ? uint8_t(255 - (x * 200 / std::max(1, w - 1))) : 255;
  });
  fs::path tmp = p;
  tmp.replace_extension(".src.bmp");
  writeAll(tmp, data.data(), data.size());
  auto rep = jc::convert(tmp, p);
  if (!rep.result) {
    std::fprintf(stderr, "  fixture PNG failed: %s\n", rep.result.message.c_str());
    ++g_failures;
  }
  fs::remove(tmp);
}

void makeTexturedObj(const fs::path& dir, const std::string& stem) {
  fs::create_directories(dir);
  makePng(dir / (stem + "_albedo.png"), 32, 32, true);
  auto flat = [&](const std::string& name, uint8_t r, uint8_t g, uint8_t b) {
    auto data = bmp(16, 16, [&](int, int, uint8_t* o) {
      o[0] = r;
      o[1] = g;
      o[2] = b;
      o[3] = 255;
    });
    fs::path tmp = dir / (name + ".bmp");
    writeAll(tmp, data.data(), data.size());
    jc::convert(tmp, dir / (name + ".png"));
    fs::remove(tmp);
  };
  flat(stem + "_normal", 128, 128, 255);
  flat(stem + "_rough", 180, 180, 180);
  flat(stem + "_metal", 60, 60, 60);
  flat(stem + "_emit", 255, 64, 0);

  writeText(dir / (stem + ".mtl"),
            "newmtl Painted\n"
            "Kd 0.9 0.8 0.7\n"
            "Ke 1 1 1\n"
            "d 1\n"
            "Pr 0.6\n"
            "Pm 0.3\n"
            "map_Kd " + stem + "_albedo.png\n"
            "norm " + stem + "_normal.png\n"
            "map_Pr " + stem + "_rough.png\n"
            "map_Pm " + stem + "_metal.png\n"
            "map_Ke " + stem + "_emit.png\n"
            "newmtl Plain\n"
            "Kd 0.2 0.4 0.9\n");
  std::string obj = "mtllib " + stem + ".mtl\n";
  const float v[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
  char buf[128];
  for (auto& p : v) {
    std::snprintf(buf, sizeof buf, "v %g %g %g\n", p[0], p[1], p[2]);
    obj += buf;
  }
  obj += "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n";
  obj += "vn 0 0 -1\nvn 0 0 1\nvn -1 0 0\nvn 1 0 0\nvn 0 -1 0\nvn 0 1 0\n";
  obj += "o Box\nusemtl Painted\n";
  obj += "f 1/1/1 4/4/1 3/3/1 2/2/1\n";
  obj += "f 5/1/2 6/2/2 7/3/2 8/4/2\n";
  obj += "f 1/1/3 5/2/3 8/3/3 4/4/3\n";
  obj += "usemtl Plain\n";
  obj += "f 2/1/4 3/2/4 7/3/4 6/4/4\n";
  obj += "f 1/1/5 2/2/5 6/3/5 5/4/5\n";
  obj += "f 4/1/6 8/2/6 7/3/6 3/4/6\n";
  writeText(dir / (stem + ".obj"), obj);
}

int main(int argc, char** argv) {
  std::string filter = argc > 1 ? argv[1] : "";
  int run = 0;
  for (auto& t : registry()) {
    std::string name = t.name;
    if (!filter.empty() && name.rfind(filter, 0) != 0) continue;
    int before = g_failures;
    std::fprintf(stderr, "[ RUN  ] %s\n", t.name);
    t.fn();
    std::fprintf(stderr, "[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", t.name);
    ++run;
  }
  std::error_code ec;
  if (!std::getenv("JC_KEEP_TEMP")) fs::remove_all(tempDir(), ec);
  else std::fprintf(stderr, "temp kept: %s\n", tempDir().string().c_str());
  if (run == 0) {
    std::fprintf(stderr, "no test matches '%s'\n", filter.c_str());
    return 1;
  }
  std::fprintf(stderr, "%d test(s), %d failure(s)\n", run, g_failures);
  return g_failures ? 1 : 0;
}
