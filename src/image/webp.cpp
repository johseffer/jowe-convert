#include "image.hpp"

#include <webp/decode.h>
#include <webp/encode.h>

namespace jowe::convert::detail {

bool webpInfo(const Bytes& in, ImageHeader& h) {
  WebPBitstreamFeatures f;
  if (WebPGetFeatures(in.data(), in.size(), &f) != VP8_STATUS_OK) return false;
  h.width = f.width;
  h.height = f.height;
  h.channels = f.has_alpha ? 4 : 3;
  h.bitDepth = 8;
  return true;
}

bool webpDecode(const Bytes& in, Image& out, std::string& err) {
  WebPBitstreamFeatures f;
  if (WebPGetFeatures(in.data(), in.size(), &f) != VP8_STATUS_OK) {
    err = "not a WebP file";
    return false;
  }
  if (f.has_animation) {
    err = "animated WebP is not supported";
    return false;
  }
  int w = 0, h = 0;
  uint8_t* px = WebPDecodeRGBA(in.data(), in.size(), &w, &h);
  if (!px) {
    err = "corrupt WebP data";
    return false;
  }
  out.width = w;
  out.height = h;
  out.hasAlpha = f.has_alpha != 0;
  out.sourceChannels = f.has_alpha ? 4 : 3;
  out.sourceBitDepth = 8;
  out.rgba.assign(px, px + size_t(w) * h * 4);
  WebPFree(px);
  return true;
}

bool webpEncode(const Image& img, int quality, bool lossless, Bytes& out, std::string& err) {
  if (img.width > WEBP_MAX_DIMENSION || img.height > WEBP_MAX_DIMENSION) {
    err = "WebP images are limited to 16383 x 16383";
    return false;
  }
  uint8_t* data = nullptr;
  size_t n = 0;
  if (lossless) n = WebPEncodeLosslessRGBA(img.rgba.data(), img.width, img.height, img.width * 4, &data);
  else n = WebPEncodeRGBA(img.rgba.data(), img.width, img.height, img.width * 4, float(quality), &data);
  if (!n || !data) {
    err = "WebP encoder failed";
    return false;
  }
  out.assign(data, data + n);
  WebPFree(data);
  return true;
}

}  // namespace jowe::convert::detail
