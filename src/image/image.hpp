#pragma once

#include "../core/common.hpp"

namespace jowe::convert::detail {

// 8-bit RGBA, rows top to bottom, no padding.
struct Image {
  int width = 0;
  int height = 0;
  bool hasAlpha = false;
  int sourceBitDepth = 8;
  int sourceChannels = 4;
  std::vector<uint8_t> rgba;
};

struct ImageHeader {
  int width = 0;
  int height = 0;
  int channels = 0;
  int bitDepth = 8;
};

// stb_image / stb_image_write / stb_image_resize2 wrappers (stb_impl.cpp).
bool stbDecode(const Bytes& in, Image& out, std::string& err);
bool stbInfo(const Bytes& in, ImageHeader& h);
bool stbEncode(const Image& img, const std::string& format, int quality, Bytes& out);
bool resizeRgba(const Image& in, int w, int h, Image& out);
Bytes zlibCompress(const uint8_t* data, size_t size, int level);
bool zlibDecompress(const uint8_t* data, size_t size, Bytes& out);

bool webpDecode(const Bytes& in, Image& out, std::string& err);
bool webpInfo(const Bytes& in, ImageHeader& h);
bool webpEncode(const Image& img, int quality, bool lossless, Bytes& out, std::string& err);

bool tiffDecode(const Bytes& in, Image& out, std::string& err);
bool tiffInfo(const Bytes& in, ImageHeader& h);
bool tiffEncode(const Image& img, Bytes& out);

bool heifInfo(const Bytes& in, ImageHeader& h, std::string* codec = nullptr);
bool heicEncoderAvailable(std::string* reason);
bool heicEncode(const Image& img, int quality, Bytes& out, std::string& err);

// OS decoders (WIC on Windows, ImageIO on Apple). format is the format id, e.g. "heic".
bool osImageCanDecode(const std::string& format, std::string* reason);
bool osImageDecode(const Bytes& in, const std::string& format, Image& out, std::string& err);

int exifOrientation(const Bytes& jpeg);
void applyOrientation(Image& img, int orientation);

std::string sniffImageFormat(const Bytes& b);
Result decodeImage(const Bytes& data, const std::string& format, Image& out);
Result encodeImage(const Image& img, const std::string& format, const ImageOptions& opt, Bytes& out);
Result probeImage(const Bytes& data, const std::string& format, ProbeInfo& info);

Result convertImage(const Context& ctx, const fs::path& in, const std::string& inFormat, const fs::path& out,
                    const std::string& outFormat, ProbeInfo& outInfo);

// Used by the 3D writers to repack textures as PNG / JPEG.
Result transcodeImageBytes(const Bytes& in, const std::string& targetFormat, Bytes& out);

}  // namespace jowe::convert::detail
