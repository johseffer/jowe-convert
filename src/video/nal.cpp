#include "nal.hpp"

namespace jowe::convert::detail {

std::vector<Bytes> splitAnnexB(const uint8_t* s, size_t n) {
  std::vector<Bytes> nals;
  size_t i = 0, start = SIZE_MAX;
  while (i + 3 <= n) {
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
      if (start != SIZE_MAX) {
        size_t end = i;
        while (end > start && s[end - 1] == 0) --end;
        if (end > start) nals.emplace_back(s + start, s + end);
      }
      i += 3;
      start = i;
    } else {
      ++i;
    }
  }
  if (start != SIZE_MAX && start < n) nals.emplace_back(s + start, s + n);
  return nals;
}

std::vector<Bytes> splitLengthPrefixed(const uint8_t* s, size_t n, int lengthSize) {
  std::vector<Bytes> nals;
  size_t at = 0;
  while (at + size_t(lengthSize) <= n) {
    size_t len = 0;
    for (int k = 0; k < lengthSize; ++k) len = (len << 8) | s[at + size_t(k)];
    at += size_t(lengthSize);
    if (len > n - at) break;
    nals.emplace_back(s + at, s + at + len);
    at += len;
  }
  return nals;
}

void appendLengthPrefixed(Bytes& out, const Bytes& nal) {
  const uint32_t n = uint32_t(nal.size());
  out.push_back(uint8_t(n >> 24));
  out.push_back(uint8_t(n >> 16));
  out.push_back(uint8_t(n >> 8));
  out.push_back(uint8_t(n));
  out.insert(out.end(), nal.begin(), nal.end());
}

void appendAnnexB(Bytes& out, const Bytes& nal) {
  static const uint8_t sc[4] = {0, 0, 0, 1};
  out.insert(out.end(), sc, sc + 4);
  out.insert(out.end(), nal.begin(), nal.end());
}

int avcNalType(const Bytes& nal) { return nal.empty() ? -1 : nal[0] & 0x1f; }
int hevcNalType(const Bytes& nal) { return nal.empty() ? -1 : (nal[0] >> 1) & 0x3f; }

bool parseAvcC(const uint8_t* d, size_t n, ParameterSets& out) {
  ByteReader r(d, n);
  if (r.u8() != 1) return false;
  r.skip(3);
  out.lengthSize = (r.u8() & 3) + 1;
  int nsps = r.u8() & 0x1f;
  for (int i = 0; i < nsps && r.ok(); ++i) {
    uint16_t len = r.be16();
    Bytes b(len);
    if (!r.read(b.data(), len)) return false;
    out.sps.push_back(std::move(b));
  }
  int npps = r.u8();
  for (int i = 0; i < npps && r.ok(); ++i) {
    uint16_t len = r.be16();
    Bytes b(len);
    if (!r.read(b.data(), len)) return false;
    out.pps.push_back(std::move(b));
  }
  return r.ok() && !out.sps.empty() && !out.pps.empty();
}

bool parseHvcC(const uint8_t* d, size_t n, ParameterSets& out) {
  ByteReader r(d, n);
  if (r.u8() != 1) return false;
  r.skip(20);
  out.lengthSize = (r.u8() & 3) + 1;
  int arrays = r.u8();
  for (int a = 0; a < arrays && r.ok(); ++a) {
    int type = r.u8() & 0x3f;
    uint16_t count = r.be16();
    for (uint16_t i = 0; i < count && r.ok(); ++i) {
      uint16_t len = r.be16();
      Bytes b(len);
      if (!r.read(b.data(), len)) return false;
      if (type == 32) out.vps.push_back(std::move(b));
      else if (type == 33) out.sps.push_back(std::move(b));
      else if (type == 34) out.pps.push_back(std::move(b));
    }
  }
  return r.ok() && !out.sps.empty() && !out.pps.empty();
}

namespace {

Bytes unescape(const Bytes& nal) {
  Bytes r;
  r.reserve(nal.size());
  for (size_t i = 0; i < nal.size(); ++i) {
    if (i >= 2 && nal[i] == 3 && nal[i - 1] == 0 && nal[i - 2] == 0) continue;
    r.push_back(nal[i]);
  }
  return r;
}

}  // namespace

Bytes buildAvcC(const ParameterSets& ps) {
  ByteWriter w;
  if (ps.sps.empty() || ps.pps.empty() || ps.sps[0].size() < 4) return {};
  const Bytes& sps = ps.sps[0];
  w.u8(1);
  w.u8(sps[1]);
  w.u8(sps[2]);
  w.u8(sps[3]);
  w.u8(0xFC | 3);
  w.u8(uint8_t(0xE0 | ps.sps.size()));
  for (auto& s : ps.sps) {
    w.be16(uint32_t(s.size()));
    w.bytes(s);
  }
  w.u8(uint8_t(ps.pps.size()));
  for (auto& p : ps.pps) {
    w.be16(uint32_t(p.size()));
    w.bytes(p);
  }
  const int profile = sps[1];
  if (profile == 100 || profile == 110 || profile == 122 || profile == 144) {
    w.u8(0xFC | 1);
    w.u8(0xF8);
    w.u8(0xF8);
    w.u8(0);
  }
  return std::move(w.data);
}

Bytes buildHvcC(const ParameterSets& ps) {
  if (ps.vps.empty() || ps.sps.empty() || ps.pps.empty()) return {};
  Bytes rbsp = unescape(ps.sps[0]);
  if (rbsp.size() < 15) return {};
  ByteWriter w;
  w.u8(1);
  w.u8(rbsp[3]);
  w.bytes(&rbsp[4], 4);
  w.bytes(&rbsp[8], 6);
  w.u8(rbsp[14]);
  w.be16(0xF000);
  w.u8(0xFC);
  w.u8(0xFC | 1);
  w.u8(0xF8);
  w.u8(0xF8);
  w.be16(0);
  w.u8(0x0F);
  w.u8(3);
  const std::vector<Bytes>* lists[3] = {&ps.vps, &ps.sps, &ps.pps};
  const int types[3] = {32, 33, 34};
  for (int k = 0; k < 3; ++k) {
    w.u8(uint8_t(0x80 | types[k]));
    w.be16(uint32_t(lists[k]->size()));
    for (auto& n : *lists[k]) {
      w.be16(uint32_t(n.size()));
      w.bytes(n);
    }
  }
  return std::move(w.data);
}

Bytes videoSampleEntry(const std::string& codec, const Bytes& config, int width, int height) {
  ByteWriter w;
  w.be32(0);
  w.fourcc(codec.c_str());
  w.zeros(6);
  w.be16(1);
  w.zeros(16);
  w.be16(uint32_t(width));
  w.be16(uint32_t(height));
  w.be32(0x00480000);
  w.be32(0x00480000);
  w.be32(0);
  w.be16(1);
  w.zeros(32);
  w.be16(0x18);
  w.be16(0xFFFF);
  w.be32(uint32_t(8 + config.size()));
  w.fourcc(codec == "avc1" ? "avcC" : "hvcC");
  w.bytes(config);
  w.patchBe32(0, uint32_t(w.size()));
  return std::move(w.data);
}

Bytes sampleEntryConfig(const Bytes& e) {
  if (e.size() < 8 + 78) return {};
  ByteReader r(e);
  r.seek(8 + 78);
  while (r.remaining() >= 8) {
    size_t at = r.pos();
    uint32_t size = r.be32();
    std::string type = r.fourcc();
    if (size < 8 || at + size > e.size()) break;
    if (type == "avcC" || type == "hvcC") return Bytes(e.begin() + long(at + 8), e.begin() + long(at + size));
    r.seek(at + size);
  }
  return {};
}

}  // namespace jowe::convert::detail
