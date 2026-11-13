#include "image.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

#if defined(JOWE_CONVERT_HAS_KVAZAAR)
#include <kvazaar.h>
#endif

namespace jowe::convert::detail {

namespace {

struct Box {
  std::string type;
  size_t start = 0;
  size_t payload = 0;
  size_t end = 0;
};

bool nextBox(const Bytes& b, size_t at, size_t limit, Box& box) {
  if (at + 8 > limit) return false;
  ByteReader r(b.data(), limit);
  r.seek(at);
  uint64_t size = r.be32();
  box.type = r.fourcc();
  if (size == 1) size = r.be64();
  else if (size == 0) size = limit - at;
  box.start = at;
  box.payload = r.pos();
  if (size < box.payload - at || at + size > limit) return false;
  box.end = size_t(at + size);
  return true;
}

bool findChild(const Bytes& b, size_t from, size_t to, const std::string& type, Box& out) {
  Box box;
  for (size_t at = from; nextBox(b, at, to, box); at = box.end)
    if (box.type == type) { out = box; return true; }
  return false;
}

}  // namespace

bool heifInfo(const Bytes& in, ImageHeader& h, std::string* codec) {
  Box ftyp, meta, iprp, ipco;
  if (!findChild(in, 0, in.size(), "ftyp", ftyp)) return false;
  std::string brand(reinterpret_cast<const char*>(in.data() + ftyp.payload), 4);
  if (brand != "heic" && brand != "heix" && brand != "mif1" && brand != "msf1" && brand != "heim" &&
      brand != "heis" && brand != "avif" && brand != "hevc")
    return false;
  if (!findChild(in, 0, in.size(), "meta", meta)) return false;
  if (!findChild(in, meta.payload + 4, meta.end, "iprp", iprp)) return false;
  if (!findChild(in, iprp.payload, iprp.end, "ipco", ipco)) return false;
  Box box;
  bool found = false;
  for (size_t at = ipco.payload; nextBox(in, at, ipco.end, box); at = box.end) {
    if (box.type == "ispe" && !found) {
      ByteReader r(in.data(), box.end);
      r.seek(box.payload + 4);
      uint32_t w = r.be32(), ht = r.be32();
      if (uint64_t(w) * ht > uint64_t(h.width) * h.height) {
        h.width = int(w);
        h.height = int(ht);
      }
    } else if (box.type == "hvcC" && codec && codec->empty()) {
      *codec = "hvc1";
    } else if (box.type == "av1C" && codec && codec->empty()) {
      *codec = "av01";
    } else if (box.type == "pixi") {
      ByteReader r(in.data(), box.end);
      r.seek(box.payload + 4);
      int n = r.u8();
      h.channels = n;
      if (n > 0) h.bitDepth = r.u8();
    }
  }
  if (!h.channels) h.channels = 3;
  return h.width > 0 && h.height > 0;
}

#if defined(JOWE_CONVERT_HAS_KVAZAAR)

namespace {

std::vector<Bytes> splitAnnexB(const Bytes& s) {
  std::vector<Bytes> nals;
  size_t i = 0, start = std::string::npos;
  while (i + 3 <= s.size()) {
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
      if (start != std::string::npos) {
        size_t end = i;
        while (end > start && s[end - 1] == 0) --end;
        nals.emplace_back(s.begin() + start, s.begin() + end);
      }
      i += 3;
      start = i;
    } else {
      ++i;
    }
  }
  if (start != std::string::npos && start < s.size()) nals.emplace_back(s.begin() + start, s.end());
  return nals;
}

Bytes unescape(const Bytes& nal) {
  Bytes r;
  r.reserve(nal.size());
  for (size_t i = 0; i < nal.size(); ++i) {
    if (i >= 2 && nal[i] == 3 && nal[i - 1] == 0 && nal[i - 2] == 0) continue;
    r.push_back(nal[i]);
  }
  return r;
}

Bytes collect(const kvz_api* api, kvz_data_chunk* chunk) {
  Bytes out;
  for (kvz_data_chunk* c = chunk; c; c = c->next) out.insert(out.end(), c->data, c->data + c->len);
  if (chunk) api->chunk_free(chunk);
  return out;
}

void box(ByteWriter& w, const char* type, const std::function<void(ByteWriter&)>& body, int fullVersion = -1,
         uint32_t flags = 0) {
  size_t at = w.size();
  w.be32(0);
  w.fourcc(type);
  if (fullVersion >= 0) {
    w.u8(uint8_t(fullVersion));
    w.be24(flags);
  }
  body(w);
  w.patchBe32(at, uint32_t(w.size() - at));
}

}  // namespace

bool heicEncoderAvailable(std::string*) { return true; }

bool heicEncode(const Image& img, int quality, Bytes& out, std::string& err) {
  const kvz_api* api = kvz_api_get(8);
  if (!api) { err = "kvazaar is unavailable"; return false; }
  const int w = img.width + (img.width & 1);
  const int h = img.height + (img.height & 1);
  if (uint64_t(w) * h > 35651584ull || w > 16888 || h > 16888) {
    err = "HEIC is limited to about 35 megapixels in this version; use --max-size";
    return false;
  }

  kvz_config* cfg = api->config_alloc();
  if (!cfg || !api->config_init(cfg)) { err = "kvazaar config failed"; return false; }
  api->config_parse(cfg, "preset", "medium");
  api->config_parse(cfg, "level", "6.2");
  cfg->width = w;
  cfg->height = h;
  cfg->intra_period = 1;
  cfg->gop_len = 0;
  cfg->hash = KVZ_HASH_NONE;
  cfg->add_encoder_info = 0;
  cfg->aud_enable = 0;
  cfg->vui.fullrange = 1;
  cfg->vui.colorprim = 1;
  cfg->vui.transfer = 13;
  cfg->vui.colormatrix = 6;
  int q = std::clamp(quality, 1, 100);
  cfg->qp = std::clamp(int(std::lround((100 - q) * 0.4 + 14)), 0, 51);

  kvz_encoder* enc = api->encoder_open(cfg);
  if (!enc) {
    api->config_destroy(cfg);
    err = "kvazaar could not open the encoder";
    return false;
  }
  kvz_picture* pic = api->picture_alloc(w, h);
  for (int y = 0; y < h; ++y) {
    const int sy = std::min(y, img.height - 1);
    for (int x = 0; x < w; ++x) {
      const int sx = std::min(x, img.width - 1);
      const uint8_t* p = &img.rgba[(size_t(sy) * img.width + sx) * 4];
      float r = p[0], g = p[1], b = p[2];
      if (img.hasAlpha) {
        float a = p[3] / 255.0f;
        r = r * a + 255 * (1 - a);
        g = g * a + 255 * (1 - a);
        b = b * a + 255 * (1 - a);
      }
      pic->y[y * pic->stride + x] = kvz_pixel(std::clamp(0.299f * r + 0.587f * g + 0.114f * b + 0.5f, 0.0f, 255.0f));
    }
  }
  const int cw = w / 2, ch = h / 2, cstride = pic->stride / 2;
  for (int y = 0; y < ch; ++y) {
    for (int x = 0; x < cw; ++x) {
      float r = 0, g = 0, b = 0;
      for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
          const int sy = std::min(y * 2 + dy, img.height - 1), sx = std::min(x * 2 + dx, img.width - 1);
          const uint8_t* p = &img.rgba[(size_t(sy) * img.width + sx) * 4];
          float a = img.hasAlpha ? p[3] / 255.0f : 1.0f;
          r += p[0] * a + 255 * (1 - a);
          g += p[1] * a + 255 * (1 - a);
          b += p[2] * a + 255 * (1 - a);
        }
      r /= 4; g /= 4; b /= 4;
      pic->u[y * cstride + x] = kvz_pixel(std::clamp(-0.168736f * r - 0.331264f * g + 0.5f * b + 128.5f, 0.0f, 255.0f));
      pic->v[y * cstride + x] = kvz_pixel(std::clamp(0.5f * r - 0.418688f * g - 0.081312f * b + 128.5f, 0.0f, 255.0f));
    }
  }

  Bytes stream;
  kvz_data_chunk* chunk = nullptr;
  uint32_t len = 0;
  if (!api->encoder_encode(enc, pic, &chunk, &len, nullptr, nullptr, nullptr)) {
    err = "kvazaar failed to encode";
  } else {
    stream = collect(api, chunk);
    for (int guard = 0; guard < 64; ++guard) {
      chunk = nullptr;
      if (!api->encoder_encode(enc, nullptr, &chunk, &len, nullptr, nullptr, nullptr) || !chunk) break;
      Bytes more = collect(api, chunk);
      stream.insert(stream.end(), more.begin(), more.end());
    }
  }
  api->picture_free(pic);
  api->encoder_close(enc);
  api->config_destroy(cfg);
  if (stream.empty()) {
    if (err.empty()) err = "kvazaar produced no data";
    return false;
  }

  Bytes vps, sps, pps;
  ByteWriter slices;
  for (auto& nal : splitAnnexB(stream)) {
    if (nal.size() < 2) continue;
    int type = (nal[0] >> 1) & 0x3f;
    if (type == 32) vps = nal;
    else if (type == 33) sps = nal;
    else if (type == 34) pps = nal;
    else if (type < 32 || type == 39 || type == 40) {
      slices.be32(uint32_t(nal.size()));
      slices.bytes(nal);
    }
  }
  if (vps.empty() || sps.empty() || pps.empty() || slices.size() == 0) {
    err = "kvazaar output is missing parameter sets";
    return false;
  }
  Bytes rbsp = unescape(sps);
  if (rbsp.size() < 15) { err = "short SPS"; return false; }

  ByteWriter f;
  box(f, "ftyp", [](ByteWriter& w) {
    w.fourcc("heic");
    w.be32(0);
    w.fourcc("mif1");
    w.fourcc("heic");
  });

  box(f, "meta", [&](ByteWriter& mw) {
    box(mw, "hdlr", [](ByteWriter& b) {
      b.be32(0);
      b.fourcc("pict");
      b.zeros(12);
      b.u8(0);
    }, 0);
    box(mw, "pitm", [](ByteWriter& b) { b.be16(1); }, 0);
    box(mw, "iloc", [&](ByteWriter& b) {
      b.u8(0x44);
      b.u8(0x00);
      b.be16(1);
      b.be16(1);
      b.be16(0);
      b.be16(1);
      b.be32(0);
      b.be32(uint32_t(slices.size()));
    }, 0);
    box(mw, "iinf", [](ByteWriter& b) {
      b.be16(1);
      box(b, "infe", [](ByteWriter& e) {
        e.be16(1);
        e.be16(0);
        e.fourcc("hvc1");
        e.u8(0);
      }, 2);
    }, 0);
    box(mw, "iprp", [&](ByteWriter& b) {
      int props = 0;
      box(b, "ipco", [&](ByteWriter& p) {
        box(p, "hvcC", [&](ByteWriter& c) {
          c.u8(1);
          c.u8(rbsp[3]);
          c.bytes(&rbsp[4], 4);
          c.bytes(&rbsp[8], 6);
          c.u8(rbsp[14]);
          c.be16(0xF000);
          c.u8(0xFC);
          c.u8(0xFC | 1);
          c.u8(0xF8);
          c.u8(0xF8);
          c.be16(0);
          c.u8(0x0F);
          c.u8(3);
          for (const Bytes* n : {&vps, &sps, &pps}) {
            c.u8(uint8_t(0x80 | ((((*n)[0]) >> 1) & 0x3f)));
            c.be16(1);
            c.be16(uint16_t(n->size()));
            c.bytes(*n);
          }
        });
        ++props;
        box(p, "ispe", [&](ByteWriter& c) {
          c.be32(uint32_t(w));
          c.be32(uint32_t(h));
        }, 0);
        ++props;
        box(p, "colr", [](ByteWriter& c) {
          c.fourcc("nclx");
          c.be16(1);
          c.be16(13);
          c.be16(6);
          c.u8(0x80);
        });
        ++props;
        box(p, "pixi", [](ByteWriter& c) {
          c.u8(3);
          c.u8(8); c.u8(8); c.u8(8);
        }, 0);
        ++props;
        if (w != img.width || h != img.height) {
          box(p, "clap", [&](ByteWriter& c) {
            c.be32(uint32_t(img.width)); c.be32(1);
            c.be32(uint32_t(img.height)); c.be32(1);
            c.be32(uint32_t(-(w - img.width))); c.be32(2);
            c.be32(uint32_t(-(h - img.height))); c.be32(2);
          });
          ++props;
        }
      });
      box(b, "ipma", [&](ByteWriter& m) {
        m.be32(1);
        m.be16(1);
        m.u8(uint8_t(props));
        m.u8(0x80 | 1);
        m.u8(2);
        m.u8(3);
        m.u8(4);
        if (props == 5) m.u8(0x80 | 5);
      }, 0);
    });
  }, 0);

  Box mt, il;
  findChild(f.data, 0, f.size(), "meta", mt);
  findChild(f.data, mt.payload + 4, mt.end, "iloc", il);
  const size_t offsetField = il.payload + 14;
  const uint32_t mdatPayload = uint32_t(f.size() + 8);
  f.patchBe32(offsetField, mdatPayload);
  f.be32(uint32_t(slices.size() + 8));
  f.fourcc("mdat");
  f.bytes(slices.data);
  out = std::move(f.data);
  return true;
}

#else

bool heicEncoderAvailable(std::string* reason) {
  if (reason) *reason = "This build has no HEIC encoder (kvazaar disabled)";
  return false;
}

bool heicEncode(const Image&, int, Bytes&, std::string& err) {
  err = "This build has no HEIC encoder";
  return false;
}

#endif

}  // namespace jowe::convert::detail
