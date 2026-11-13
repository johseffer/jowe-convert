#include <cmath>

#include "audio.hpp"

namespace jowe::convert::detail {

namespace {

float clampf(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

int32_t toInt(float v, int bits) {
  const double scale = double(1u << (bits - 1));
  double s = std::floor(double(clampf(v)) * scale + 0.5);
  if (s > scale - 1) s = scale - 1;
  if (s < -scale) s = -scale;
  return int32_t(s);
}

float readSample(const uint8_t* p, int bits, bool isFloat, bool bigEndian) {
  const int bytes = bits / 8;
  uint8_t b[8];
  for (int i = 0; i < bytes; ++i) b[i] = bigEndian ? p[bytes - 1 - i] : p[i];
  if (isFloat) {
    if (bits == 32) {
      float f;
      std::memcpy(&f, b, 4);
      return f;
    }
    double d;
    std::memcpy(&d, b, 8);
    return float(d);
  }
  if (bits == 8) return (int(b[0]) - 128) / 128.0f;
  int32_t v = 0;
  for (int i = 0; i < bytes; ++i) v |= int32_t(b[i]) << (8 * i);
  v <<= (32 - bits);
  return float(double(v) / 2147483648.0);
}

void writeSamples(ByteWriter& w, const Pcm& pcm, int bits, bool isFloat, bool bigEndian, bool unsigned8) {
  const int bytes = bits / 8;
  uint8_t b[4];
  for (float v : pcm.samples) {
    if (isFloat) {
      float f = v;
      std::memcpy(b, &f, 4);
    } else if (bits == 8 && unsigned8) {
      b[0] = uint8_t(toInt(v, 8) + 128);
    } else {
      int32_t s = toInt(v, bits);
      for (int i = 0; i < bytes; ++i) b[i] = uint8_t(uint32_t(s) >> (8 * i));
    }
    for (int i = 0; i < bytes; ++i) w.u8(bigEndian ? b[bytes - 1 - i] : b[i]);
  }
}

bool decodeInterleaved(const uint8_t* data, size_t size, int channels, int bits, bool isFloat, bool bigEndian,
                       Pcm& out) {
  if (channels <= 0 || (bits != 8 && bits != 16 && bits != 24 && bits != 32 && bits != 64)) return false;
  if (isFloat && bits != 32 && bits != 64) return false;
  const size_t frameBytes = size_t(channels) * bits / 8;
  const size_t frames = size / frameBytes;
  out.samples.resize(frames * channels);
  for (size_t i = 0; i < frames * channels; ++i)
    out.samples[i] = readSample(data + i * (bits / 8), bits, isFloat, bigEndian);
  return true;
}

double readExtended(const uint8_t* p) {
  int exponent = ((p[0] & 0x7f) << 8) | p[1];
  uint64_t mant = 0;
  for (int i = 0; i < 8; ++i) mant = (mant << 8) | p[2 + i];
  if (exponent == 0 && mant == 0) return 0;
  double f = std::ldexp(double(mant), exponent - 16383 - 63);
  return (p[0] & 0x80) ? -f : f;
}

void writeExtended(ByteWriter& w, double v) {
  int exponent = 0;
  double m = std::frexp(v, &exponent);
  uint64_t mant = uint64_t(std::ldexp(m, 64));
  w.be16(uint16_t(exponent - 1 + 16383));
  w.be64(mant);
}

}  // namespace

// ---- WAV ----

static bool wavParse(const Bytes& in, AudioHeader& h, size_t& dataAt, size_t& dataSize, bool& isFloat) {
  if (in.size() < 12 || std::memcmp(in.data(), "RIFF", 4) || std::memcmp(in.data() + 8, "WAVE", 4)) return false;
  ByteReader r(in);
  r.seek(12);
  bool haveFmt = false;
  dataAt = 0;
  while (r.remaining() >= 8) {
    std::string id = r.fourcc();
    uint32_t sz = r.le32();
    size_t body = r.pos();
    if (id == "fmt ") {
      uint16_t tag = r.le16();
      h.channels = r.le16();
      h.sampleRate = int(r.le32());
      r.le32();
      r.le16();
      h.bits = r.le16();
      if (tag == 0xFFFE && sz >= 40) {
        r.le16();
        r.le16();
        r.le32();
        tag = r.le16();
      }
      if (tag == 1) isFloat = false;
      else if (tag == 3) isFloat = true;
      else return false;
      h.codec = isFloat ? "pcm_f" + std::to_string(h.bits) + "le" : "pcm_s" + std::to_string(h.bits) + "le";
      haveFmt = true;
    } else if (id == "data") {
      dataAt = body;
      dataSize = std::min<size_t>(sz, in.size() - body);
      break;
    }
    r.seek(body + sz + (sz & 1));
  }
  if (!haveFmt || !dataAt || h.channels <= 0 || h.bits <= 0) return false;
  h.frames = dataSize / (size_t(h.channels) * h.bits / 8);
  return true;
}

bool wavInfo(const Bytes& in, AudioHeader& h) {
  size_t at, sz;
  bool f = false;
  return wavParse(in, h, at, sz, f);
}

bool wavRead(const Bytes& in, Pcm& out, std::string& err) {
  AudioHeader h;
  size_t at = 0, sz = 0;
  bool isFloat = false;
  if (!wavParse(in, h, at, sz, isFloat)) { err = "unsupported or corrupt WAV"; return false; }
  out.sampleRate = h.sampleRate;
  out.channels = h.channels;
  out.sourceBits = h.bits;
  out.sourceFloat = isFloat;
  out.codec = h.codec;
  if (!decodeInterleaved(in.data() + at, sz, h.channels, h.bits, isFloat, false, out)) {
    err = "unsupported WAV sample format";
    return false;
  }
  return true;
}

bool wavWrite(const Pcm& pcm, int bits, Bytes& out) {
  const bool isFloat = bits == 32;
  const bool extensible = pcm.channels > 2;
  const uint32_t dataSize = uint32_t(pcm.samples.size() * (bits / 8));
  ByteWriter w;
  w.fourcc("RIFF");
  w.le32(0);
  w.fourcc("WAVE");
  w.fourcc("fmt ");
  w.le32(extensible ? 40 : (isFloat ? 18 : 16));
  w.le16(extensible ? 0xFFFE : (isFloat ? 3 : 1));
  w.le16(uint16_t(pcm.channels));
  w.le32(uint32_t(pcm.sampleRate));
  w.le32(uint32_t(pcm.sampleRate * pcm.channels * bits / 8));
  w.le16(uint16_t(pcm.channels * bits / 8));
  w.le16(uint16_t(bits));
  if (extensible) {
    w.le16(22);
    w.le16(uint16_t(bits));
    w.le32(0);
    static const uint8_t pcmGuid[16] = {1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
    uint8_t g[16];
    std::memcpy(g, pcmGuid, 16);
    if (isFloat) g[0] = 3;
    w.bytes(g, 16);
  } else if (isFloat) {
    w.le16(0);
  }
  if (isFloat) {
    w.fourcc("fact");
    w.le32(4);
    w.le32(uint32_t(pcm.frames()));
  }
  w.fourcc("data");
  w.le32(dataSize);
  writeSamples(w, pcm, bits, isFloat, false, true);
  if (w.size() & 1) w.u8(0);
  w.patchLe32(4, uint32_t(w.size() - 8));
  out = std::move(w.data);
  return true;
}

// ---- AIFF / AIFC ----

static bool aiffParse(const Bytes& in, AudioHeader& h, size_t& dataAt, size_t& dataSize, bool& isFloat,
                      bool& littleEndian) {
  if (in.size() < 12 || std::memcmp(in.data(), "FORM", 4)) return false;
  bool aifc = std::memcmp(in.data() + 8, "AIFC", 4) == 0;
  if (!aifc && std::memcmp(in.data() + 8, "AIFF", 4)) return false;
  ByteReader r(in);
  r.seek(12);
  bool haveComm = false;
  dataAt = 0;
  isFloat = false;
  littleEndian = false;
  while (r.remaining() >= 8) {
    std::string id = r.fourcc();
    uint32_t sz = r.be32();
    size_t body = r.pos();
    if (id == "COMM" && sz >= 18) {
      h.channels = r.be16();
      h.frames = r.be32();
      h.bits = r.be16();
      if (r.remaining() < 10) return false;
      h.sampleRate = int(std::lround(readExtended(r.ptr())));
      r.skip(10);
      h.codec = "pcm_s" + std::to_string(h.bits) + "be";
      if (aifc && sz >= 22) {
        std::string comp = r.fourcc();
        if (comp == "sowt") { littleEndian = true; h.codec = "pcm_s" + std::to_string(h.bits) + "le"; }
        else if (comp == "fl32" || comp == "FL32") { isFloat = true; h.bits = 32; h.codec = "pcm_f32be"; }
        else if (comp == "fl64" || comp == "FL64") { isFloat = true; h.bits = 64; h.codec = "pcm_f64be"; }
        else if (comp != "NONE" && comp != "twos") return false;
      }
      haveComm = true;
    } else if (id == "SSND") {
      uint32_t offset = r.be32();
      r.be32();
      dataAt = r.pos() + offset;
      dataSize = sz >= 8 + offset ? std::min<size_t>(sz - 8 - offset, in.size() - std::min(in.size(), dataAt)) : 0;
    }
    r.seek(body + sz + (sz & 1));
  }
  return haveComm && dataAt && h.channels > 0 && h.bits > 0;
}

bool aiffInfo(const Bytes& in, AudioHeader& h) {
  size_t a, s;
  bool f, le;
  return aiffParse(in, h, a, s, f, le);
}

bool aiffRead(const Bytes& in, Pcm& out, std::string& err) {
  AudioHeader h;
  size_t at = 0, sz = 0;
  bool isFloat = false, le = false;
  if (!aiffParse(in, h, at, sz, isFloat, le)) { err = "unsupported or corrupt AIFF"; return false; }
  int bits = (h.bits + 7) / 8 * 8;
  out.sampleRate = h.sampleRate;
  out.channels = h.channels;
  out.sourceBits = h.bits;
  out.sourceFloat = isFloat;
  out.codec = h.codec;
  if (!decodeInterleaved(in.data() + at, sz, h.channels, bits, isFloat, !le, out)) {
    err = "unsupported AIFF sample format";
    return false;
  }
  return true;
}

bool aiffWrite(const Pcm& pcm, int bits, Bytes& out) {
  ByteWriter w;
  w.fourcc("FORM");
  w.be32(0);
  w.fourcc("AIFF");
  w.fourcc("COMM");
  w.be32(18);
  w.be16(uint16_t(pcm.channels));
  w.be32(uint32_t(pcm.frames()));
  w.be16(uint16_t(bits));
  writeExtended(w, double(pcm.sampleRate));
  w.fourcc("SSND");
  w.be32(uint32_t(8 + pcm.samples.size() * (bits / 8)));
  w.be32(0);
  w.be32(0);
  writeSamples(w, pcm, bits, false, true, false);
  if (w.size() & 1) w.u8(0);
  w.patchBe32(4, uint32_t(w.size() - 8));
  out = std::move(w.data);
  return true;
}

// ---- CAF ----

static bool cafParse(const Bytes& in, AudioHeader& h, size_t& dataAt, size_t& dataSize, bool& isFloat, bool& le) {
  if (in.size() < 8 || std::memcmp(in.data(), "caff", 4)) return false;
  ByteReader r(in);
  r.seek(8);
  bool haveDesc = false;
  dataAt = 0;
  while (r.remaining() >= 12) {
    std::string id = r.fourcc();
    int64_t sz = int64_t(r.be64());
    size_t body = r.pos();
    if (id == "desc") {
      uint64_t rateBits = r.be64();
      double rate;
      std::memcpy(&rate, &rateBits, 8);
      std::string fmt = r.fourcc();
      uint32_t flags = r.be32();
      r.be32();
      r.be32();
      h.channels = int(r.be32());
      h.bits = int(r.be32());
      h.sampleRate = int(std::lround(rate));
      if (fmt != "lpcm") {
        h.codec = fmt;
        isFloat = false;
        le = false;
      } else {
        isFloat = flags & 1;
        le = flags & 2;
        h.codec = std::string("pcm_") + (isFloat ? "f" : "s") + std::to_string(h.bits) + (le ? "le" : "be");
      }
      haveDesc = true;
    } else if (id == "data") {
      dataAt = body + 4;
      size_t avail = in.size() - std::min(in.size(), dataAt);
      dataSize = sz < 0 ? avail : std::min<size_t>(size_t(sz) - 4, avail);
      if (sz < 0) break;
    }
    if (sz < 0) break;
    r.seek(body + size_t(sz));
  }
  if (!haveDesc || !dataAt || h.channels <= 0 || h.bits <= 0) return false;
  if (h.codec.rfind("pcm_", 0) == 0) h.frames = dataSize / (size_t(h.channels) * h.bits / 8);
  return true;
}

bool cafInfo(const Bytes& in, AudioHeader& h) {
  size_t a, s;
  bool f, le;
  return cafParse(in, h, a, s, f, le);
}

bool cafRead(const Bytes& in, Pcm& out, std::string& err) {
  AudioHeader h;
  size_t at = 0, sz = 0;
  bool isFloat = false, le = false;
  if (!cafParse(in, h, at, sz, isFloat, le)) { err = "unsupported or corrupt CAF"; return false; }
  if (h.codec.rfind("pcm_", 0) != 0) { err = "CAF with " + h.codec + " audio is not supported here"; return false; }
  out.sampleRate = h.sampleRate;
  out.channels = h.channels;
  out.sourceBits = h.bits;
  out.sourceFloat = isFloat;
  out.codec = h.codec;
  if (!decodeInterleaved(in.data() + at, sz, h.channels, h.bits, isFloat, !le, out)) {
    err = "unsupported CAF sample format";
    return false;
  }
  return true;
}

bool cafWrite(const Pcm& pcm, int bits, Bytes& out) {
  const bool isFloat = bits == 32;
  ByteWriter w;
  w.fourcc("caff");
  w.be16(1);
  w.be16(0);
  w.fourcc("desc");
  w.be64(32);
  double rate = pcm.sampleRate;
  uint64_t rateBits;
  std::memcpy(&rateBits, &rate, 8);
  w.be64(rateBits);
  w.fourcc("lpcm");
  w.be32(isFloat ? 1u : 0u);
  w.be32(uint32_t(pcm.channels * bits / 8));
  w.be32(1);
  w.be32(uint32_t(pcm.channels));
  w.be32(uint32_t(bits));
  w.fourcc("data");
  w.be64(4 + pcm.samples.size() * (bits / 8));
  w.be32(0);
  writeSamples(w, pcm, bits, isFloat, true, false);
  out = std::move(w.data);
  return true;
}

}  // namespace jowe::convert::detail
