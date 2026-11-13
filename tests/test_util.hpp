#pragma once

#include <jowe/convert.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace jc = jowe::convert;

struct TestCase {
  const char* name;
  void (*fn)();
};
std::vector<TestCase>& registry();
struct Registrar {
  Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); }
};

extern int g_failures;
fs::path tempDir();

#define TEST(name)                                         \
  static void test_##name();                               \
  static Registrar reg_##name(#name, &test_##name);        \
  static void test_##name()

#define CHECK(cond)                                                                    \
  do {                                                                                 \
    if (!(cond)) {                                                                     \
      std::fprintf(stderr, "  CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                    \
    }                                                                                  \
  } while (0)

#define REQUIRE(cond)                                                                    \
  do {                                                                                   \
    if (!(cond)) {                                                                       \
      std::fprintf(stderr, "  REQUIRE failed %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++g_failures;                                                                      \
      return;                                                                            \
    }                                                                                    \
  } while (0)

#define CHECK_OK(rep)                                                                                      \
  do {                                                                                                     \
    if (!(rep).result) {                                                                                   \
      std::fprintf(stderr, "  convert failed %s:%d: %s (%s)\n", __FILE__, __LINE__, (rep).result.message.c_str(), \
                   jc::toString((rep).result.status));                                                     \
      ++g_failures;                                                                                        \
    }                                                                                                      \
  } while (0)

inline std::vector<uint8_t> readAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

inline void writeAll(const fs::path& p, const void* data, size_t n) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(static_cast<const char*>(data), std::streamsize(n));
}

inline void writeText(const fs::path& p, const std::string& s) { writeAll(p, s.data(), s.size()); }

// 16-bit PCM WAV with a sine per channel.
void makeWav(const fs::path& p, int rate, int channels, double seconds, int bits = 16);
// Gradient RGBA image written as PNG through the library (BMP source converted).
void makePng(const fs::path& p, int w, int h, bool alpha);
// A textured cube: OBJ + MTL + PNG textures (base colour, normal, roughness, metallic, emissive).
void makeTexturedObj(const fs::path& dir, const std::string& stem);
