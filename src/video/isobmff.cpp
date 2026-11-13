#include "isobmff.hpp"

#include <cmath>
#include <fstream>
#include <map>

namespace jowe::convert::detail {

namespace {

struct BoxRef {
  std::string type;
  size_t start = 0;
  size_t payload = 0;
  size_t end = 0;
};

bool readBox(const uint8_t* p, size_t at, size_t limit, BoxRef& b) {
  if (at + 8 > limit) return false;
  ByteReader r(p, limit);
  r.seek(at);
  uint64_t size = r.be32();
  b.type = r.fourcc();
  if (size == 1) size = r.be64();
  else if (size == 0) size = limit - at;
  b.start = at;
  b.payload = r.pos();
  if (size < b.payload - at || size > limit - at) return false;
  b.end = size_t(at + size);
  return true;
}

template <class F>
void eachChild(const uint8_t* p, size_t from, size_t to, F&& f) {
  BoxRef b;
  for (size_t at = from; readBox(p, at, to, b); at = b.end) f(b);
}

void parseSampleEntry(const uint8_t* p, const BoxRef& e, MovieTrack& t) {
  ByteReader r(p, e.end);
  if (t.handler == "vide" && e.end >= e.payload + 28) {
    r.seek(e.payload + 24);
    t.width = r.be16();
    t.height = r.be16();
  } else if (t.handler == "soun" && e.end >= e.payload + 28) {
    r.seek(e.payload + 8);
    uint16_t version = r.be16();
    if (version == 2 && e.end >= e.payload + 40) {
      r.seek(e.payload + 28);
      uint64_t bits = r.be64();
      double rate;
      std::memcpy(&rate, &bits, 8);
      t.sampleRate = int(std::lround(rate));
      t.channels = int(r.be32());
    } else {
      r.seek(e.payload + 16);
      t.channels = r.be16();
      r.seek(e.payload + 24);
      t.sampleRate = int(r.be32() >> 16);
    }
  }
}

bool parseStbl(const uint8_t* p, const BoxRef& stbl, MovieTrack& t, std::string& err) {
  std::vector<uint32_t> sizes;
  uint32_t fixedSize = 0, sampleCount = 0;
  std::vector<std::pair<uint32_t, uint32_t>> stts, cttsRuns;
  std::vector<int32_t> cttsOffsets;
  struct Stsc { uint32_t first, perChunk, desc; };
  std::vector<Stsc> stsc;
  std::vector<uint64_t> chunks;
  std::vector<uint32_t> sync;
  bool ok = true;
  eachChild(p, stbl.payload, stbl.end, [&](const BoxRef& b) {
    ByteReader r(p, b.end);
    r.seek(b.payload);
    if (b.type == "stsd") {
      uint8_t version = r.u8();
      r.be24();
      uint32_t n = r.be32();
      (void)version;
      size_t at = r.pos();
      for (uint32_t i = 0; i < n; ++i) {
        BoxRef e;
        if (!readBox(p, at, b.end, e)) break;
        t.sampleEntries.emplace_back(p + at, p + e.end);
        if (i == 0) {
          t.codec = e.type;
          parseSampleEntry(p, e, t);
        }
        at = e.end;
      }
    } else if (b.type == "stts") {
      r.be32();
      uint32_t n = r.be32();
      for (uint32_t i = 0; i < n && r.ok(); ++i) {
        uint32_t c = r.be32();
        stts.emplace_back(c, r.be32());
      }
    } else if (b.type == "ctts") {
      uint8_t version = r.u8();
      r.be24();
      uint32_t n = r.be32();
      t.hasCtts = true;
      for (uint32_t i = 0; i < n && r.ok(); ++i) {
        uint32_t c = r.be32();
        uint32_t v = r.be32();
        cttsRuns.emplace_back(c, v);
        cttsOffsets.push_back(version == 1 ? int32_t(v) : int32_t(std::min<uint32_t>(v, 0x7fffffff)));
      }
    } else if (b.type == "stsc") {
      r.be32();
      uint32_t n = r.be32();
      for (uint32_t i = 0; i < n && r.ok(); ++i) {
        Stsc s;
        s.first = r.be32();
        s.perChunk = r.be32();
        s.desc = r.be32();
        stsc.push_back(s);
      }
    } else if (b.type == "stsz") {
      r.be32();
      fixedSize = r.be32();
      sampleCount = r.be32();
      if (!fixedSize) {
        if (sampleCount > r.remaining() / 4) { ok = false; return; }
        sizes.resize(sampleCount);
        for (auto& s : sizes) s = r.be32();
      }
    } else if (b.type == "stz2") {
      err = "compact sample sizes (stz2) are not supported";
      ok = false;
    } else if (b.type == "stco" || b.type == "co64") {
      r.be32();
      uint32_t n = r.be32();
      if (n > r.remaining() / 4) { ok = false; return; }
      chunks.resize(n);
      for (auto& c : chunks) c = b.type == "co64" ? r.be64() : r.be32();
    } else if (b.type == "stss") {
      r.be32();
      uint32_t n = r.be32();
      t.hasSyncTable = true;
      if (n > r.remaining() / 4) { ok = false; return; }
      sync.resize(n);
      for (auto& s : sync) s = r.be32();
    }
  });
  if (!ok) {
    if (err.empty()) err = "corrupt sample table";
    return false;
  }
  if (sampleCount == 0) return true;
  if (t.sampleEntries.empty() || stsc.empty() || chunks.empty()) {
    err = "incomplete sample table";
    return false;
  }
  t.samples.resize(sampleCount);
  size_t si = 0;
  for (size_t ci = 0; ci < chunks.size() && si < sampleCount; ++ci) {
    uint32_t chunkNo = uint32_t(ci + 1);
    const Stsc* e = &stsc[0];
    for (auto& s : stsc)
      if (s.first <= chunkNo) e = &s;
    uint64_t off = chunks[ci];
    for (uint32_t k = 0; k < e->perChunk && si < sampleCount; ++k, ++si) {
      auto& s = t.samples[si];
      s.offset = off;
      s.size = fixedSize ? fixedSize : sizes[si];
      s.descIndex = e->desc;
      off += s.size;
    }
  }
  if (si < sampleCount) {
    err = "sample table references missing chunks";
    return false;
  }
  uint64_t dts = 0;
  si = 0;
  for (auto& run : stts)
    for (uint32_t k = 0; k < run.first && si < sampleCount; ++k, ++si) {
      t.samples[si].dts = dts;
      t.samples[si].duration = run.second;
      dts += run.second;
    }
  for (; si < sampleCount; ++si) t.samples[si].dts = dts;
  si = 0;
  for (size_t i = 0; i < cttsRuns.size(); ++i)
    for (uint32_t k = 0; k < cttsRuns[i].first && si < sampleCount; ++k, ++si) t.samples[si].cto = cttsOffsets[i];
  if (t.hasSyncTable) {
    for (auto& s : t.samples) s.sync = false;
    for (uint32_t n : sync)
      if (n >= 1 && n <= sampleCount) t.samples[n - 1].sync = true;
  }
  return true;
}

bool parseTrak(const uint8_t* p, const BoxRef& trak, MovieTrack& t, std::string& err) {
  bool ok = true;
  eachChild(p, trak.payload, trak.end, [&](const BoxRef& b) {
    ByteReader r(p, b.end);
    r.seek(b.payload);
    if (b.type == "tkhd") {
      uint8_t v = r.u8();
      t.tkhdFlags = r.be24();
      if (v == 1) { r.be64(); r.be64(); } else { r.be32(); r.be32(); }
      t.id = r.be32();
      r.be32();
      if (v == 1) r.be64(); else r.be32();
      r.skip(8);
      t.layer = r.be16();
      t.alternateGroup = r.be16();
      t.volume = r.be16();
      r.be16();
      r.read(t.matrix, 36);
      t.widthFixed = r.be32();
      t.heightFixed = r.be32();
    } else if (b.type == "edts") {
      t.edts.assign(p + b.start, p + b.end);
    } else if (b.type == "mdia") {
      eachChild(p, b.payload, b.end, [&](const BoxRef& m) {
        ByteReader mr(p, m.end);
        mr.seek(m.payload);
        if (m.type == "mdhd") {
          uint8_t v = mr.u8();
          mr.be24();
          if (v == 1) { mr.be64(); mr.be64(); } else { mr.be32(); mr.be32(); }
          t.timescale = mr.be32();
          t.mediaDuration = v == 1 ? mr.be64() : mr.be32();
          t.language = mr.be16();
        } else if (m.type == "hdlr") {
          mr.be32();
          mr.be32();
          t.handler = mr.fourcc();
          mr.skip(12);
          if (mr.remaining() > 0) {
            const char* s = reinterpret_cast<const char*>(mr.ptr());
            size_t n = mr.remaining();
            size_t first = static_cast<uint8_t>(s[0]);
            if (first > 0 && first == n - 1) t.handlerName.assign(s + 1, first);
            else t.handlerName.assign(s, strnlen(s, n));
          }
        }
      });
      eachChild(p, b.payload, b.end, [&](const BoxRef& m) {
        if (m.type != "minf") return;
        eachChild(p, m.payload, m.end, [&](const BoxRef& s) {
          if (s.type == "stbl" && ok) ok = parseStbl(p, s, t, err);
        });
      });
    }
  });
  return ok;
}

bool parseMoov(const uint8_t* p, size_t from, size_t to, Movie& mv, std::string& err) {
  bool ok = true;
  eachChild(p, from, to, [&](const BoxRef& b) {
    if (!ok) return;
    ByteReader r(p, b.end);
    r.seek(b.payload);
    if (b.type == "mvhd") {
      uint8_t v = r.u8();
      r.be24();
      if (v == 1) { r.be64(); r.be64(); } else { r.be32(); r.be32(); }
      mv.timescale = r.be32();
      mv.duration = v == 1 ? r.be64() : r.be32();
      r.be32();
      r.be16();
      r.skip(10);
      r.read(mv.matrix, 36);
    } else if (b.type == "trak") {
      MovieTrack t;
      if (!parseTrak(p, b, t, err)) { ok = false; return; }
      mv.tracks.push_back(std::move(t));
    } else if (b.type == "mvex") {
      err = "fragmented MP4 is not supported yet";
      ok = false;
    }
  });
  return ok;
}

void parseFtyp(const uint8_t* p, const BoxRef& b, Movie& mv) {
  ByteReader r(p, b.end);
  r.seek(b.payload);
  mv.majorBrand = r.fourcc();
  r.be32();
  while (r.remaining() >= 4) mv.compatibleBrands.push_back(r.fourcc());
}

}  // namespace

uint64_t MovieTrack::durationTicks() const {
  if (samples.empty()) return mediaDuration;
  const auto& last = samples.back();
  uint64_t d = last.dts + last.duration;
  return d ? d : mediaDuration;
}

bool parseMovie(const Bytes& file, Movie& mv, std::string& err) {
  const uint8_t* p = file.data();
  bool haveMoov = false;
  BoxRef b;
  for (size_t at = 0; readBox(p, at, file.size(), b); at = b.end) {
    if (b.type == "ftyp") parseFtyp(p, b, mv);
    else if (b.type == "moov") {
      if (!parseMoov(p, b.payload, b.end, mv, err)) return false;
      haveMoov = true;
    } else if (b.type == "moof") {
      err = "fragmented MP4 is not supported yet";
      return false;
    }
  }
  if (!haveMoov) {
    if (err.empty()) err = "no movie header (moov) found";
    return false;
  }
  return true;
}

bool parseMovieFile(const fs::path& path, Movie& mv, std::string& err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { err = "cannot open file"; return false; }
  f.seekg(0, std::ios::end);
  const uint64_t total = uint64_t(f.tellg());
  uint64_t at = 0;
  bool haveMoov = false;
  while (at + 8 <= total) {
    f.seekg(std::streamoff(at));
    uint8_t h[16];
    if (!f.read(reinterpret_cast<char*>(h), 8)) break;
    uint64_t size = uint64_t(h[0]) << 24 | uint64_t(h[1]) << 16 | uint64_t(h[2]) << 8 | h[3];
    std::string type(reinterpret_cast<char*>(h + 4), 4);
    size_t header = 8;
    if (size == 1) {
      if (!f.read(reinterpret_cast<char*>(h + 8), 8)) break;
      size = 0;
      for (int i = 8; i < 16; ++i) size = size << 8 | h[i];
      header = 16;
    } else if (size == 0) {
      size = total - at;
    }
    if (size < header || at + size > total) {
      if (type == "mdat") break;
      err = "truncated box '" + type + "'";
      return false;
    }
    if (type == "ftyp" || type == "moov") {
      if (size > (512ull << 20)) { err = "movie header too large"; return false; }
      Bytes box(static_cast<size_t>(size));
      f.seekg(std::streamoff(at));
      if (!f.read(reinterpret_cast<char*>(box.data()), std::streamsize(size))) { err = "read error"; return false; }
      BoxRef b{type, 0, header, size_t(size)};
      if (type == "ftyp") parseFtyp(box.data(), b, mv);
      else {
        if (!parseMoov(box.data(), header, size_t(size), mv, err)) return false;
        haveMoov = true;
      }
    } else if (type == "moof") {
      err = "fragmented MP4 is not supported yet";
      return false;
    }
    at += size;
  }
  if (!haveMoov) {
    if (err.empty()) err = "no movie header (moov) found";
    return false;
  }
  return true;
}

bool codecAllowedInMp4(const std::string& c) {
  static const char* ok[] = {"avc1", "avc3", "hvc1", "hev1", "mp4a", "ac-3", "ec-3", "Opus", "fLaC", "alac",
                             "av01", "vp09", "vp08", "mp4v", "dvh1", "dvhe", ".mp3", "tx3g", "wvtt", "stpp"};
  for (auto* x : ok)
    if (c == x) return true;
  return false;
}

namespace {

class BoxWriter {
 public:
  ByteWriter& w;
  explicit BoxWriter(ByteWriter& out) : w(out) {}
  template <class F>
  void box(const char* type, F&& body, int version = -1, uint32_t flags = 0) {
    size_t at = w.size();
    w.be32(0);
    w.fourcc(type);
    if (version >= 0) {
      w.u8(uint8_t(version));
      w.be24(flags);
    }
    body();
    w.patchBe32(at, uint32_t(w.size() - at));
  }
};

struct Chunk {
  size_t track;
  size_t first;
  size_t count;
  uint32_t desc;
  double start;
  uint64_t bytes;
};

void identityMatrix(ByteWriter& w) {
  const uint32_t m[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
  for (uint32_t v : m) w.be32(v);
}

bool isZero(const uint8_t* m, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (m[i]) return false;
  return true;
}

void writeName(ByteWriter& w, const std::string& name, bool pascal) {
  if (pascal) {
    w.u8(uint8_t(std::min<size_t>(name.size(), 255)));
    w.str(name.substr(0, 255));
  } else {
    w.str(name);
    w.u8(0);
  }
}

Bytes buildMoov(const Movie& mv, const std::vector<Chunk>& chunks, const std::vector<uint64_t>& chunkOffsets,
                const MuxOptions& opt, bool co64) {
  const bool mov = opt.flavor == ContainerFlavor::Mov;
  ByteWriter w;
  BoxWriter bw(w);
  uint64_t movieDuration = 0;
  std::vector<uint64_t> trackDur(mv.tracks.size());
  for (size_t i = 0; i < mv.tracks.size(); ++i) {
    const auto& t = mv.tracks[i];
    trackDur[i] = t.timescale ? uint64_t(double(t.durationTicks()) * mv.timescale / t.timescale + 0.5) : 0;
    movieDuration = std::max(movieDuration, trackDur[i]);
  }
  uint32_t maxId = 0;
  for (auto& t : mv.tracks) maxId = std::max(maxId, t.id);
  bw.box("moov", [&] {
    const bool v1 = movieDuration > 0xFFFFFFFFull;
    bw.box("mvhd", [&] {
      if (v1) { w.be64(0); w.be64(0); w.be32(mv.timescale); w.be64(movieDuration); }
      else { w.be32(0); w.be32(0); w.be32(mv.timescale); w.be32(uint32_t(movieDuration)); }
      w.be32(0x00010000);
      w.be16(0x0100);
      w.zeros(10);
      if (isZero(mv.matrix, 36)) identityMatrix(w); else w.bytes(mv.matrix, 36);
      w.zeros(24);
      w.be32(maxId + 1);
    }, v1 ? 1 : 0);
    for (size_t ti = 0; ti < mv.tracks.size(); ++ti) {
      const auto& t = mv.tracks[ti];
      const bool video = t.handler == "vide";
      bw.box("trak", [&] {
        const bool tv1 = trackDur[ti] > 0xFFFFFFFFull;
        bw.box("tkhd", [&] {
          if (tv1) { w.be64(0); w.be64(0); w.be32(t.id); w.be32(0); w.be64(trackDur[ti]); }
          else { w.be32(0); w.be32(0); w.be32(t.id); w.be32(0); w.be32(uint32_t(trackDur[ti])); }
          w.zeros(8);
          w.be16(t.layer);
          w.be16(t.alternateGroup);
          w.be16(t.handler == "soun" ? (t.volume ? t.volume : 0x0100) : 0);
          w.be16(0);
          if (isZero(t.matrix, 36)) identityMatrix(w); else w.bytes(t.matrix, 36);
          w.be32(video ? (t.widthFixed ? t.widthFixed : uint32_t(t.width) << 16) : 0);
          w.be32(video ? (t.heightFixed ? t.heightFixed : uint32_t(t.height) << 16) : 0);
        }, tv1 ? 1 : 0, t.tkhdFlags ? t.tkhdFlags : 3);
        if (!t.edts.empty()) w.bytes(t.edts);
        bw.box("mdia", [&] {
          const uint64_t md = t.durationTicks();
          const bool mv1 = md > 0xFFFFFFFFull;
          bw.box("mdhd", [&] {
            if (mv1) { w.be64(0); w.be64(0); w.be32(t.timescale); w.be64(md); }
            else { w.be32(0); w.be32(0); w.be32(t.timescale); w.be32(uint32_t(md)); }
            w.be16(t.language);
            w.be16(0);
          }, mv1 ? 1 : 0);
          bw.box("hdlr", [&] {
            if (mov) w.fourcc("mhlr"); else w.be32(0);
            w.str(t.handler);
            w.zeros(12);
            std::string name = t.handlerName.empty() ? (video ? "VideoHandler" : "SoundHandler") : t.handlerName;
            writeName(w, name, mov);
          }, 0);
          bw.box("minf", [&] {
            if (video) bw.box("vmhd", [&] { w.zeros(8); }, 0, 1);
            else bw.box("smhd", [&] { w.zeros(4); }, 0);
            if (mov) {
              bw.box("hdlr", [&] {
                w.fourcc("dhlr");
                w.fourcc("alis");
                w.zeros(12);
                writeName(w, "DataHandler", true);
              }, 0);
            }
            bw.box("dinf", [&] {
              bw.box("dref", [&] {
                w.be32(1);
                bw.box(mov ? "alis" : "url ", [] {}, 0, 1);
              }, 0);
            });
            bw.box("stbl", [&] {
              bw.box("stsd", [&] {
                w.be32(uint32_t(t.sampleEntries.size()));
                for (auto& e : t.sampleEntries) w.bytes(e);
              }, 0);
              std::vector<std::pair<uint32_t, uint32_t>> runs;
              for (auto& s : t.samples) {
                if (!runs.empty() && runs.back().second == s.duration) ++runs.back().first;
                else runs.emplace_back(1, s.duration);
              }
              bw.box("stts", [&] {
                w.be32(uint32_t(runs.size()));
                for (auto& r : runs) { w.be32(r.first); w.be32(r.second); }
              }, 0);
              bool anyCto = false, negative = false;
              for (auto& s : t.samples) {
                if (s.cto) anyCto = true;
                if (s.cto < 0) negative = true;
              }
              if (anyCto) {
                std::vector<std::pair<uint32_t, int32_t>> c;
                for (auto& s : t.samples) {
                  if (!c.empty() && c.back().second == s.cto) ++c.back().first;
                  else c.emplace_back(1, s.cto);
                }
                bw.box("ctts", [&] {
                  w.be32(uint32_t(c.size()));
                  for (auto& r : c) { w.be32(r.first); w.be32(uint32_t(r.second)); }
                }, negative ? 1 : 0);
              }
              bool allSync = true;
              for (auto& s : t.samples) allSync = allSync && s.sync;
              if (!allSync) {
                bw.box("stss", [&] {
                  uint32_t n = 0;
                  for (auto& s : t.samples) n += s.sync;
                  w.be32(n);
                  for (size_t i = 0; i < t.samples.size(); ++i)
                    if (t.samples[i].sync) w.be32(uint32_t(i + 1));
                }, 0);
              }
              std::vector<size_t> mine;
              for (size_t ci = 0; ci < chunks.size(); ++ci)
                if (chunks[ci].track == ti) mine.push_back(ci);
              bw.box("stsc", [&] {
                size_t countAt = w.size();
                w.be32(0);
                uint32_t entries = 0, lastCount = 0, lastDesc = 0;
                for (size_t k = 0; k < mine.size(); ++k) {
                  const auto& c = chunks[mine[k]];
                  if (k == 0 || c.count != lastCount || c.desc != lastDesc) {
                    w.be32(uint32_t(k + 1));
                    w.be32(uint32_t(c.count));
                    w.be32(c.desc);
                    lastCount = uint32_t(c.count);
                    lastDesc = c.desc;
                    ++entries;
                  }
                }
                w.patchBe32(countAt, entries);
              }, 0);
              bool same = !t.samples.empty();
              for (auto& s : t.samples) same = same && s.size == t.samples[0].size;
              bw.box("stsz", [&] {
                w.be32(same ? t.samples[0].size : 0);
                w.be32(uint32_t(t.samples.size()));
                if (!same)
                  for (auto& s : t.samples) w.be32(s.size);
              }, 0);
              bw.box(co64 ? "co64" : "stco", [&] {
                w.be32(uint32_t(mine.size()));
                for (size_t ci : mine) co64 ? w.be64(chunkOffsets[ci]) : w.be32(uint32_t(chunkOffsets[ci]));
              }, 0);
            });
          });
        });
      });
    }
  });
  return std::move(w.data);
}

}  // namespace

bool writeMovie(const Movie& mv, std::istream& src, std::ostream& out, const MuxOptions& opt, std::string& err,
                const std::function<bool(double)>& progress) {
  std::vector<Chunk> chunks;
  uint64_t payload = 0;
  for (size_t ti = 0; ti < mv.tracks.size(); ++ti) {
    const auto& t = mv.tracks[ti];
    const double ts = t.timescale ? t.timescale : 1;
    size_t i = 0;
    while (i < t.samples.size()) {
      Chunk c{ti, i, 0, t.samples[i].descIndex, double(t.samples[i].dts) / ts, 0};
      while (i < t.samples.size() && t.samples[i].descIndex == c.desc &&
             (double(t.samples[i].dts) / ts - c.start) < 0.5 && c.bytes < (1u << 20)) {
        c.bytes += t.samples[i].size;
        ++c.count;
        ++i;
      }
      payload += c.bytes;
      chunks.push_back(c);
    }
  }
  std::stable_sort(chunks.begin(), chunks.end(), [](const Chunk& a, const Chunk& b) { return a.start < b.start; });

  ByteWriter ftyp;
  {
    BoxWriter bw(ftyp);
    bw.box("ftyp", [&] {
      if (opt.flavor == ContainerFlavor::Mov) {
        ftyp.fourcc("qt  ");
        ftyp.be32(0x20050300);
        ftyp.fourcc("qt  ");
      } else {
        ftyp.fourcc("isom");
        ftyp.be32(0x200);
        ftyp.fourcc("isom");
        ftyp.fourcc("iso2");
        bool avc = false;
        for (auto& t : mv.tracks) avc = avc || t.codec == "avc1" || t.codec == "avc3";
        if (avc) ftyp.fourcc("avc1");
        ftyp.fourcc("mp41");
      }
    });
  }

  std::vector<uint64_t> offsets(chunks.size(), 0);
  Bytes probeMoov = buildMoov(mv, chunks, offsets, opt, true);
  const bool co64 = ftyp.size() + probeMoov.size() + 16 + payload > 0xFFFFFFFFull;
  const bool largeMdat = payload + 8 > 0xFFFFFFFFull;
  Bytes moov = buildMoov(mv, chunks, offsets, opt, co64);
  uint64_t at = ftyp.size() + moov.size() + (largeMdat ? 16 : 8);
  for (size_t i = 0; i < chunks.size(); ++i) {
    offsets[i] = at;
    at += chunks[i].bytes;
  }
  moov = buildMoov(mv, chunks, offsets, opt, co64);

  out.write(reinterpret_cast<const char*>(ftyp.data.data()), std::streamsize(ftyp.size()));
  out.write(reinterpret_cast<const char*>(moov.data()), std::streamsize(moov.size()));
  ByteWriter mdat;
  if (largeMdat) {
    mdat.be32(1);
    mdat.fourcc("mdat");
    mdat.be64(payload + 16);
  } else {
    mdat.be32(uint32_t(payload + 8));
    mdat.fourcc("mdat");
  }
  out.write(reinterpret_cast<const char*>(mdat.data.data()), std::streamsize(mdat.size()));

  std::vector<char> buf;
  uint64_t done = 0;
  for (auto& c : chunks) {
    const auto& t = mv.tracks[c.track];
    for (size_t k = 0; k < c.count; ++k) {
      const auto& s = t.samples[c.first + k];
      buf.resize(s.size);
      src.clear();
      src.seekg(std::streamoff(s.offset));
      if (s.size && !src.read(buf.data(), std::streamsize(s.size))) {
        err = "the source ends before a sample it references";
        return false;
      }
      out.write(buf.data(), std::streamsize(s.size));
      done += s.size;
    }
    if (progress && !progress(payload ? double(done) / double(payload) : 1.0)) {
      err = "cancelled";
      return false;
    }
  }
  if (!out) {
    err = "write error";
    return false;
  }
  return true;
}

}  // namespace jowe::convert::detail
