#include "image.hpp"

#include <cstdlib>

// TinyUSDZ links its own stb copies; ours are static or renamed so the two never collide.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PIC
#define STBI_NO_PNM

#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define stbi_write_tga_with_rle jc_stbi_write_tga_with_rle
#define stbi_write_png_compression_level jc_stbi_write_png_compression_level
#define stbi_write_force_png_filter jc_stbi_write_force_png_filter
#define stbi_write_png_to_func jc_stbi_write_png_to_func
#define stbi_write_bmp_to_func jc_stbi_write_bmp_to_func
#define stbi_write_tga_to_func jc_stbi_write_tga_to_func
#define stbi_write_hdr_to_func jc_stbi_write_hdr_to_func
#define stbi_write_jpg_to_func jc_stbi_write_jpg_to_func
#define stbi_flip_vertically_on_write jc_stbi_flip_vertically_on_write
#define stbi_zlib_compress jc_stbi_zlib_compress
#define stbi_write_png_to_mem jc_stbi_write_png_to_mem

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace jowe::convert::detail {

bool stbInfo(const Bytes& in, ImageHeader& h) {
  int w = 0, ht = 0, c = 0;
  if (!stbi_info_from_memory(in.data(), int(in.size()), &w, &ht, &c)) return false;
  h.width = w;
  h.height = ht;
  h.channels = c;
  h.bitDepth = stbi_is_16_bit_from_memory(in.data(), int(in.size())) ? 16 : 8;
  return true;
}

bool stbDecode(const Bytes& in, Image& out, std::string& err) {
  int w = 0, h = 0, c = 0;
  stbi_uc* px = stbi_load_from_memory(in.data(), int(in.size()), &w, &h, &c, 4);
  if (!px) {
    err = stbi_failure_reason() ? stbi_failure_reason() : "decode failed";
    return false;
  }
  out.width = w;
  out.height = h;
  out.sourceChannels = c;
  out.sourceBitDepth = stbi_is_16_bit_from_memory(in.data(), int(in.size())) ? 16 : 8;
  out.rgba.assign(px, px + size_t(w) * h * 4);
  out.hasAlpha = (c == 2 || c == 4);
  stbi_image_free(px);
  return true;
}

static void writeToBytes(void* ctx, void* data, int size) {
  auto* b = static_cast<Bytes*>(ctx);
  auto* p = static_cast<const uint8_t*>(data);
  b->insert(b->end(), p, p + size);
}

bool stbEncode(const Image& img, const std::string& format, int quality, Bytes& out) {
  out.clear();
  const int comp = img.hasAlpha ? 4 : 3;
  std::vector<uint8_t> packed;
  const uint8_t* src = img.rgba.data();
  if (comp == 3) {
    packed.resize(size_t(img.width) * img.height * 3);
    for (size_t i = 0, n = size_t(img.width) * img.height; i < n; ++i) {
      packed[i * 3] = img.rgba[i * 4];
      packed[i * 3 + 1] = img.rgba[i * 4 + 1];
      packed[i * 3 + 2] = img.rgba[i * 4 + 2];
    }
    src = packed.data();
  }
  int ok = 0;
  if (format == "png") {
    jc_stbi_write_png_compression_level = 9;
    ok = stbi_write_png_to_func(writeToBytes, &out, img.width, img.height, comp, src, img.width * comp);
  } else if (format == "jpeg") {
    ok = stbi_write_jpg_to_func(writeToBytes, &out, img.width, img.height, comp, src, quality);
  } else if (format == "bmp") {
    ok = stbi_write_bmp_to_func(writeToBytes, &out, img.width, img.height, comp, src);
  } else if (format == "tga") {
    jc_stbi_write_tga_with_rle = 1;
    ok = stbi_write_tga_to_func(writeToBytes, &out, img.width, img.height, comp, src);
  }
  return ok != 0;
}

bool resizeRgba(const Image& in, int w, int h, Image& out) {
  out.width = w;
  out.height = h;
  out.hasAlpha = in.hasAlpha;
  out.sourceBitDepth = in.sourceBitDepth;
  out.sourceChannels = in.sourceChannels;
  out.rgba.assign(size_t(w) * h * 4, 0);
  return stbir_resize_uint8_srgb(in.rgba.data(), in.width, in.height, in.width * 4, out.rgba.data(), w, h, w * 4,
                                 in.hasAlpha ? STBIR_RGBA : STBIR_4CHANNEL) != nullptr;
}

Bytes zlibCompress(const uint8_t* data, size_t size, int level) {
  int n = 0;
  unsigned char* z = stbi_zlib_compress(const_cast<unsigned char*>(data), int(size), &n, level);
  Bytes out;
  if (z) {
    out.assign(z, z + n);
    STBIW_FREE(z);
  }
  return out;
}

bool zlibDecompress(const uint8_t* data, size_t size, Bytes& out) {
  int n = 0;
  char* p = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(data), int(size), &n);
  if (!p) return false;
  out.assign(reinterpret_cast<uint8_t*>(p), reinterpret_cast<uint8_t*>(p) + n);
  STBI_FREE(p);
  return true;
}

}  // namespace jowe::convert::detail
