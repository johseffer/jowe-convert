#include "image.hpp"

#include <map>

namespace jowe::convert::detail {

namespace {

struct TiffReader {
  const Bytes& b;
  bool le = true;

  uint16_t u16(size_t at) const {
    if (at + 2 > b.size()) return 0;
    return le ? uint16_t(b[at] | b[at + 1] << 8) : uint16_t(b[at] << 8 | b[at + 1]);
  }
  uint32_t u32(size_t at) const {
    if (at + 4 > b.size()) return 0;
    return le ? uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24
              : uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | uint32_t(b[at + 3]);
  }
};

using Tags = std::map<uint16_t, std::vector<uint32_t>>;

bool readIfd0(const Bytes& b, TiffReader& r, Tags& tags) {
  if (b.size() < 8) return false;
  if (b[0] == 'I' && b[1] == 'I') r.le = true;
  else if (b[0] == 'M' && b[1] == 'M') r.le = false;
  else return false;
  if (r.u16(2) != 42) return false;
  size_t ifd = r.u32(4);
  uint16_t n = r.u16(ifd);
  if (ifd + 2 + size_t(n) * 12 > b.size()) return false;
  for (uint16_t i = 0; i < n; ++i) {
    size_t e = ifd + 2 + size_t(i) * 12;
    uint16_t tag = r.u16(e), type = r.u16(e + 2);
    uint32_t count = r.u32(e + 4);
    size_t sz = type == 3 ? 2 : (type == 4 ? 4 : (type == 1 || type == 2 || type == 7 ? 1 : (type == 5 ? 8 : 0)));
    if (!sz || count > (1u << 24)) continue;
    size_t total = sz * count;
    size_t at = total <= 4 ? e + 8 : r.u32(e + 8);
    if (at + total > b.size()) continue;
    std::vector<uint32_t> v;
    v.reserve(type == 5 ? count * 2 : count);
    for (uint32_t k = 0; k < count; ++k) {
      if (type == 3) v.push_back(r.u16(at + k * 2));
      else if (type == 4) v.push_back(r.u32(at + k * 4));
      else if (type == 5) { v.push_back(r.u32(at + k * 8)); v.push_back(r.u32(at + k * 8 + 4)); }
      else v.push_back(b[at + k]);
    }
    tags[tag] = std::move(v);
  }
  return true;
}

uint32_t tag1(const Tags& t, uint16_t id, uint32_t def) {
  auto it = t.find(id);
  return it == t.end() || it->second.empty() ? def : it->second[0];
}

bool lzwDecode(const uint8_t* src, size_t n, Bytes& out, size_t expected) {
  std::vector<std::vector<uint8_t>> table;
  auto reset = [&] {
    table.assign(258, {});
    for (int i = 0; i < 256; ++i) table[i] = {uint8_t(i)};
  };
  reset();
  size_t bitPos = 0;
  int width = 9;
  int prev = -1;
  auto next = [&]() -> int {
    if ((bitPos + width) > n * 8) return 257;
    uint32_t v = 0;
    for (int i = 0; i < width; ++i) {
      size_t bit = bitPos + i;
      v = (v << 1) | ((src[bit >> 3] >> (7 - (bit & 7))) & 1);
    }
    bitPos += width;
    return int(v);
  };
  while (out.size() < expected) {
    int code = next();
    if (code == 257) break;
    if (code == 256) {
      reset();
      width = 9;
      code = next();
      if (code == 257) break;
      if (code > 255) return false;
      out.push_back(uint8_t(code));
      prev = code;
      continue;
    }
    std::vector<uint8_t> entry;
    if (code < int(table.size())) {
      entry = table[code];
      if (prev >= 0) {
        auto add = table[prev];
        add.push_back(entry[0]);
        if (table.size() < 4096) table.push_back(std::move(add));
      }
    } else if (code == int(table.size()) && prev >= 0) {
      entry = table[prev];
      entry.push_back(entry[0]);
      if (table.size() < 4096) table.push_back(entry);
    } else {
      return false;
    }
    out.insert(out.end(), entry.begin(), entry.end());
    prev = code;
    size_t next_code = table.size();
    if (next_code + 1 >= 2048) width = 12;
    else if (next_code + 1 >= 1024) width = 11;
    else if (next_code + 1 >= 512) width = 10;
  }
  return true;
}

bool packbitsDecode(const uint8_t* src, size_t n, Bytes& out, size_t expected) {
  size_t i = 0;
  while (i < n && out.size() < expected) {
    int8_t c = int8_t(src[i++]);
    if (c >= 0) {
      size_t len = size_t(c) + 1;
      if (i + len > n) return false;
      out.insert(out.end(), src + i, src + i + len);
      i += len;
    } else if (c != -128) {
      if (i >= n) return false;
      out.insert(out.end(), size_t(1 - c), src[i++]);
    }
  }
  return true;
}

}  // namespace

bool tiffInfo(const Bytes& in, ImageHeader& h) {
  TiffReader r{in};
  Tags t;
  if (!readIfd0(in, r, t)) return false;
  h.width = int(tag1(t, 256, 0));
  h.height = int(tag1(t, 257, 0));
  h.channels = int(tag1(t, 277, 1));
  h.bitDepth = int(tag1(t, 258, 1));
  return h.width > 0 && h.height > 0;
}

bool tiffDecode(const Bytes& in, Image& out, std::string& err) {
  TiffReader r{in};
  Tags t;
  if (!readIfd0(in, r, t)) { err = "not a TIFF file"; return false; }
  const uint32_t w = tag1(t, 256, 0), h = tag1(t, 257, 0);
  const uint32_t spp = tag1(t, 277, 1), bps = tag1(t, 258, 1), comp = tag1(t, 259, 1);
  const uint32_t photo = tag1(t, 262, spp >= 3 ? 2 : 1), planar = tag1(t, 284, 1), predictor = tag1(t, 317, 1);
  const uint32_t sampleFormat = tag1(t, 339, 1);
  if (!w || !h || w > 65535 || h > 65535) { err = "bad TIFF dimensions"; return false; }
  if (bps != 8 && bps != 16) { err = "TIFF with " + std::to_string(bps) + "-bit samples is not supported"; return false; }
  if (sampleFormat == 3) { err = "floating-point TIFF is not supported"; return false; }
  if (planar != 1) { err = "planar TIFF is not supported"; return false; }
  if (spp < 1 || spp > 4) { err = "unsupported TIFF channel count"; return false; }
  if (photo != 0 && photo != 1 && photo != 2 && photo != 3) { err = "unsupported TIFF colour model"; return false; }

  const bool tiled = t.count(322) && t.count(324);
  const uint32_t tw = tiled ? tag1(t, 322, w) : w;
  const uint32_t th = tiled ? tag1(t, 323, h) : tag1(t, 278, h);
  const auto& offsets = t[tiled ? 324 : 273];
  const auto& counts = t[tiled ? 325 : 279];
  if (offsets.empty() || offsets.size() != counts.size()) { err = "TIFF without image data"; return false; }

  const size_t bpp = size_t(spp) * bps / 8;
  std::vector<uint8_t> raw(size_t(w) * h * bpp);
  const uint32_t tilesAcross = tiled ? (w + tw - 1) / tw : 1;
  const uint32_t rowsPerBlock = std::min(th, h);

  for (size_t blk = 0; blk < offsets.size(); ++blk) {
    size_t off = offsets[blk], cnt = counts[blk];
    if (off + cnt > in.size()) { err = "truncated TIFF"; return false; }
    const uint32_t bw = tiled ? tw : w;
    const size_t expected = size_t(bw) * rowsPerBlock * bpp;
    Bytes dec;
    const uint8_t* src = in.data() + off;
    bool ok = true;
    switch (comp) {
      case 1: dec.assign(src, src + std::min(cnt, expected)); break;
      case 5: ok = lzwDecode(src, cnt, dec, expected); break;
      case 8:
      case 32946: ok = zlibDecompress(src, cnt, dec); break;
      case 32773: ok = packbitsDecode(src, cnt, dec, expected); break;
      default: err = "TIFF compression " + std::to_string(comp) + " is not supported"; return false;
    }
    if (!ok) { err = "corrupt TIFF data"; return false; }
    dec.resize(expected, 0);
    if (predictor == 2) {
      for (uint32_t y = 0; y < rowsPerBlock; ++y) {
        uint8_t* row = dec.data() + size_t(y) * bw * bpp;
        if (bps == 8) {
          for (uint32_t x = 1; x < bw; ++x)
            for (uint32_t c = 0; c < spp; ++c) row[x * spp + c] = uint8_t(row[x * spp + c] + row[(x - 1) * spp + c]);
        } else {
          auto rd = [&](size_t i) { return r.le ? uint16_t(row[i * 2] | row[i * 2 + 1] << 8) : uint16_t(row[i * 2] << 8 | row[i * 2 + 1]); };
          auto wr = [&](size_t i, uint16_t v) {
            if (r.le) { row[i * 2] = uint8_t(v); row[i * 2 + 1] = uint8_t(v >> 8); }
            else { row[i * 2] = uint8_t(v >> 8); row[i * 2 + 1] = uint8_t(v); }
          };
          for (uint32_t x = 1; x < bw; ++x)
            for (uint32_t c = 0; c < spp; ++c) wr(x * spp + c, uint16_t(rd(x * spp + c) + rd((x - 1) * spp + c)));
        }
      }
    }
    const uint32_t bx = tiled ? uint32_t(blk % tilesAcross) * tw : 0;
    const uint32_t by = tiled ? uint32_t(blk / tilesAcross) * th : uint32_t(blk) * rowsPerBlock;
    for (uint32_t y = 0; y < rowsPerBlock && by + y < h; ++y) {
      const uint32_t copyW = std::min(bw, w - std::min(w, bx));
      std::memcpy(raw.data() + (size_t(by + y) * w + bx) * bpp, dec.data() + size_t(y) * bw * bpp, size_t(copyW) * bpp);
    }
  }

  const auto& cmap = t[320];
  out.width = int(w);
  out.height = int(h);
  out.sourceBitDepth = int(bps);
  out.sourceChannels = int(spp);
  out.hasAlpha = (photo == 2 && spp == 4) || (photo <= 1 && spp == 2);
  out.rgba.assign(size_t(w) * h * 4, 255);
  auto sample = [&](size_t px, uint32_t c) -> uint32_t {
    size_t i = px * spp + c;
    if (bps == 8) return raw[i];
    return r.le ? uint32_t(raw[i * 2] | raw[i * 2 + 1] << 8) : uint32_t(raw[i * 2] << 8 | raw[i * 2 + 1]);
  };
  auto to8 = [&](uint32_t v) { return uint8_t(bps == 16 ? v >> 8 : v); };
  for (size_t p = 0, n = size_t(w) * h; p < n; ++p) {
    uint8_t* o = &out.rgba[p * 4];
    if (photo == 3) {
      uint32_t idx = sample(p, 0);
      size_t colors = size_t(1) << bps;
      if (cmap.size() >= colors * 3) {
        o[0] = uint8_t(cmap[idx] >> 8);
        o[1] = uint8_t(cmap[colors + idx] >> 8);
        o[2] = uint8_t(cmap[2 * colors + idx] >> 8);
      }
    } else if (photo <= 1) {
      uint8_t g = to8(sample(p, 0));
      if (photo == 0) g = uint8_t(255 - g);
      o[0] = o[1] = o[2] = g;
      if (spp >= 2) o[3] = to8(sample(p, 1));
    } else {
      o[0] = to8(sample(p, 0));
      o[1] = to8(sample(p, spp > 1 ? 1 : 0));
      o[2] = to8(sample(p, spp > 2 ? 2 : 0));
      if (spp == 4) o[3] = to8(sample(p, 3));
    }
  }
  return true;
}

bool tiffEncode(const Image& img, Bytes& out) {
  const uint32_t spp = img.hasAlpha ? 4 : 3;
  const uint32_t w = uint32_t(img.width), h = uint32_t(img.height);
  const uint32_t rowBytes = w * spp;
  const uint32_t rowsPerStrip = std::max<uint32_t>(1, (256 * 1024) / std::max<uint32_t>(1, rowBytes));
  const uint32_t strips = (h + rowsPerStrip - 1) / rowsPerStrip;

  ByteWriter bw;
  bw.str("II");
  bw.le16(42);
  bw.le32(0);

  std::vector<uint32_t> offsets, counts;
  std::vector<uint8_t> strip;
  for (uint32_t s = 0; s < strips; ++s) {
    uint32_t y0 = s * rowsPerStrip, rows = std::min(rowsPerStrip, h - y0);
    strip.assign(size_t(rows) * rowBytes, 0);
    for (uint32_t y = 0; y < rows; ++y) {
      const uint8_t* src = &img.rgba[size_t(y0 + y) * w * 4];
      uint8_t* dst = &strip[size_t(y) * rowBytes];
      for (uint32_t x = 0; x < w; ++x)
        for (uint32_t c = 0; c < spp; ++c) dst[x * spp + c] = src[x * 4 + c];
      for (uint32_t x = w - 1; x > 0; --x)
        for (uint32_t c = 0; c < spp; ++c) dst[x * spp + c] = uint8_t(dst[x * spp + c] - dst[(x - 1) * spp + c]);
    }
    Bytes z = zlibCompress(strip.data(), strip.size(), 6);
    if (z.empty()) return false;
    offsets.push_back(uint32_t(bw.size()));
    counts.push_back(uint32_t(z.size()));
    bw.bytes(z);
    if (bw.size() & 1) bw.u8(0);
  }

  auto putArray = [&](const std::vector<uint32_t>& v, bool shorts) {
    uint32_t at = uint32_t(bw.size());
    for (uint32_t x : v) shorts ? bw.le16(x) : bw.le32(x);
    if (bw.size() & 1) bw.u8(0);
    return at;
  };
  std::vector<uint32_t> bitsV(spp, 8);
  uint32_t bitsAt = putArray(bitsV, true);
  uint32_t offAt = strips > 1 ? putArray(offsets, false) : offsets[0];
  uint32_t cntAt = strips > 1 ? putArray(counts, false) : counts[0];
  uint32_t resAt = uint32_t(bw.size());
  bw.le32(72); bw.le32(1);

  struct Entry { uint16_t tag, type; uint32_t count, value; };
  std::vector<Entry> e = {
      {256, 4, 1, w},
      {257, 4, 1, h},
      {258, 3, spp, bitsAt},
      {259, 3, 1, 8},
      {262, 3, 1, 2},
      {273, 4, strips, offAt},
      {274, 3, 1, 1},
      {277, 3, 1, spp},
      {278, 4, 1, rowsPerStrip},
      {279, 4, strips, cntAt},
      {282, 5, 1, resAt},
      {283, 5, 1, resAt},
      {284, 3, 1, 1},
      {296, 3, 1, 2},
      {317, 3, 1, 2},
  };
  if (img.hasAlpha) e.push_back({338, 3, 1, 2});

  uint32_t ifdAt = uint32_t(bw.size());
  bw.patchLe32(4, ifdAt);
  bw.le16(uint16_t(e.size()));
  for (auto& x : e) {
    bw.le16(x.tag);
    bw.le16(x.type);
    bw.le32(x.count);
    if (x.type == 3 && x.count == 1) { bw.le16(x.value); bw.le16(0); }
    else bw.le32(x.value);
  }
  bw.le32(0);
  out = std::move(bw.data);
  return true;
}

}  // namespace jowe::convert::detail
