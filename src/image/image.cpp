#include "image.hpp"

namespace jowe::convert::detail {

std::string sniffImageFormat(const Bytes& b) {
  auto at = [&](size_t i, const char* s) {
    size_t n = std::strlen(s);
    return b.size() >= i + n && std::memcmp(b.data() + i, s, n) == 0;
  };
  if (at(0, "\x89PNG")) return "png";
  if (b.size() > 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "jpeg";
  if (at(0, "RIFF") && at(8, "WEBP")) return "webp";
  if (at(0, "II*") || at(0, "MM\0*")) return "tiff";
  if (at(0, "BM")) return "bmp";
  if (at(0, "GIF8")) return "gif";
  if (at(0, "8BPS")) return "psd";
  if (at(4, "ftyp")) {
    if (at(8, "avif") || at(8, "avis")) return "avif";
    if (at(8, "heic") || at(8, "heix") || at(8, "mif1") || at(8, "msf1") || at(8, "heim") || at(8, "heis"))
      return "heic";
  }
  return {};
}

int exifOrientation(const Bytes& j) {
  if (j.size() < 4 || j[0] != 0xFF || j[1] != 0xD8) return 1;
  size_t i = 2;
  while (i + 4 <= j.size() && j[i] == 0xFF) {
    uint8_t marker = j[i + 1];
    size_t len = size_t(j[i + 2]) << 8 | j[i + 3];
    if (marker == 0xDA || marker == 0xD9) break;
    if (marker == 0xE1 && i + 4 + len <= j.size() + 2 && len > 8 && std::memcmp(&j[i + 4], "Exif\0\0", 6) == 0) {
      size_t t = i + 10;
      if (t + 8 > j.size()) return 1;
      bool le = j[t] == 'I';
      auto u16 = [&](size_t a) -> uint32_t {
        if (a + 2 > j.size()) return 0;
        return le ? uint32_t(j[a] | j[a + 1] << 8) : uint32_t(j[a] << 8 | j[a + 1]);
      };
      auto u32 = [&](size_t a) -> uint32_t { return le ? (u16(a) | u16(a + 2) << 16) : (u16(a) << 16 | u16(a + 2)); };
      size_t ifd = t + u32(t + 4);
      uint32_t n = u16(ifd);
      for (uint32_t k = 0; k < n && k < 512; ++k) {
        size_t e = ifd + 2 + k * 12;
        if (u16(e) == 0x0112) {
          uint32_t v = u16(e + 8);
          return (v >= 1 && v <= 8) ? int(v) : 1;
        }
      }
      return 1;
    }
    i += 2 + len;
  }
  return 1;
}

void applyOrientation(Image& img, int o) {
  if (o <= 1 || o > 8) return;
  const int w = img.width, h = img.height;
  const bool swap = o >= 5;
  const int nw = swap ? h : w, nh = swap ? w : h;
  std::vector<uint8_t> dst(img.rgba.size());
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int dx = x, dy = y;
      switch (o) {
        case 2: dx = w - 1 - x; break;
        case 3: dx = w - 1 - x; dy = h - 1 - y; break;
        case 4: dy = h - 1 - y; break;
        case 5: dx = y; dy = x; break;
        case 6: dx = h - 1 - y; dy = x; break;
        case 7: dx = h - 1 - y; dy = w - 1 - x; break;
        case 8: dx = y; dy = w - 1 - x; break;
      }
      std::memcpy(&dst[(size_t(dy) * nw + dx) * 4], &img.rgba[(size_t(y) * w + x) * 4], 4);
    }
  }
  img.rgba.swap(dst);
  img.width = nw;
  img.height = nh;
}

Result decodeImage(const Bytes& data, const std::string& format, Image& out) {
  std::string fmt = sniffImageFormat(data);
  if (fmt.empty()) fmt = format;
  std::string err;
  bool ok = false;
  if (fmt == "webp") ok = webpDecode(data, out, err);
  else if (fmt == "tiff") ok = tiffDecode(data, out, err);
  else if (fmt == "heic" || fmt == "avif") ok = osImageDecode(data, fmt, out, err);
  else {
    ok = stbDecode(data, out, err);
    if (ok && fmt == "jpeg") applyOrientation(out, exifOrientation(data));
  }
  if (!ok) {
    Status s = (fmt == "heic" || fmt == "avif") && !osImageCanDecode(fmt, nullptr) ? Status::Unsupported
                                                                                   : Status::Unreadable;
    return Result::error(s, "This image can't be read: " + err);
  }
  if (out.width <= 0 || out.height <= 0) return Result::error(Status::Unreadable, "Empty image");
  return Result::ok();
}

static void flattenOnWhite(Image& img) {
  if (!img.hasAlpha) return;
  for (size_t i = 0; i < img.rgba.size(); i += 4) {
    uint32_t a = img.rgba[i + 3];
    for (int c = 0; c < 3; ++c) img.rgba[i + c] = uint8_t((img.rgba[i + c] * a + 255 * (255 - a) + 127) / 255);
    img.rgba[i + 3] = 255;
  }
  img.hasAlpha = false;
}

Result encodeImage(const Image& src, const std::string& format, const ImageOptions& opt, Bytes& out) {
  const Image* img = &src;
  Image resized;
  if (opt.maxSize > 0 && std::max(src.width, src.height) > opt.maxSize) {
    double s = double(opt.maxSize) / std::max(src.width, src.height);
    int w = std::max(1, int(src.width * s + 0.5)), h = std::max(1, int(src.height * s + 0.5));
    if (!resizeRgba(src, w, h, resized)) return Result::error(Status::Failed, "Resize failed");
    img = &resized;
  }
  const int quality = std::clamp(opt.quality, 1, 100);
  std::string err;
  if (format == "png" || format == "bmp" || format == "tga") {
    if (!stbEncode(*img, format, quality, out)) return Result::error(Status::Failed, "Image encoder failed");
  } else if (format == "jpeg") {
    Image flat = *img;
    flattenOnWhite(flat);
    if (!stbEncode(flat, "jpeg", quality, out)) return Result::error(Status::Failed, "JPEG encoder failed");
  } else if (format == "webp") {
    if (!webpEncode(*img, quality, opt.lossless, out, err)) return Result::error(Status::Failed, err);
  } else if (format == "tiff") {
    if (!tiffEncode(*img, out)) return Result::error(Status::Failed, "TIFF encoder failed");
  } else if (format == "heic") {
    if (!heicEncoderAvailable(&err)) return Result::error(Status::NotWritable, err);
    if (!heicEncode(*img, quality, out, err)) return Result::error(Status::Failed, err);
  } else {
    return Result::error(Status::NotWritable, "Images can't be written as " + format);
  }
  return Result::ok();
}

Result probeImage(const Bytes& data, const std::string& format, ProbeInfo& info) {
  std::string fmt = sniffImageFormat(data);
  if (fmt.empty()) fmt = format;
  ImageHeader h;
  bool ok = false;
  if (fmt == "webp") ok = webpInfo(data, h);
  else if (fmt == "tiff") ok = tiffInfo(data, h);
  else if (fmt == "heic" || fmt == "avif") ok = heifInfo(data, h, &info.codec);
  else ok = stbInfo(data, h);
  if (!ok) return Result::error(Status::Unreadable, "This image can't be read.");
  info.format = fmt;
  info.width = h.width;
  info.height = h.height;
  info.channels = h.channels;
  info.bitDepth = h.bitDepth;
  if (fmt == "jpeg" && exifOrientation(data) >= 5) std::swap(info.width, info.height);
  return Result::ok();
}

Result convertImage(const Context& ctx, const fs::path& in, const std::string& inFormat, const fs::path& out,
                    const std::string& outFormat, ProbeInfo& outInfo) {
  Bytes data;
  if (auto r = ctx.io.readFile(in, data); !r) return r;
  ctx.progress(0.1);
  Image img;
  if (auto r = decodeImage(data, inFormat, img); !r) return r;
  if (ctx.cancelled()) return Result::error(Status::Cancelled, "Cancelled");
  ctx.progress(0.5);
  Bytes encoded;
  if (auto r = encodeImage(img, outFormat, ctx.options->image, encoded); !r) return r;
  if (ctx.cancelled()) return Result::error(Status::Cancelled, "Cancelled");
  PendingFile pending(ctx.io, out);
  if (auto r = ctx.io.writeFile(pending.tempPath(), encoded); !r) return r;
  if (auto r = pending.commit(); !r) return r;
  if (ctx.written) ctx.written->push_back(out);
  outInfo.kind = MediaKind::Image;
  outInfo.fileSize = encoded.size();
  probeImage(encoded, outFormat, outInfo);
  ctx.progress(1.0);
  return Result::ok();
}

Result transcodeImageBytes(const Bytes& in, const std::string& targetFormat, Bytes& out) {
  Image img;
  if (auto r = decodeImage(in, "", img); !r) return r;
  ImageOptions opt;
  opt.quality = 92;
  return encodeImage(img, targetFormat, opt, out);
}

}  // namespace jowe::convert::detail
