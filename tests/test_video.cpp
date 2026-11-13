#include "test_util.hpp"
#include "video/isobmff.hpp"

using namespace jowe::convert::detail;

namespace {

Bytes entry(const char* type, const std::function<void(ByteWriter&)>& body) {
  ByteWriter w;
  w.be32(0);
  w.fourcc(type);
  body(w);
  w.patchBe32(0, uint32_t(w.size()));
  return w.data;
}

Bytes avc1(int width, int height) {
  return entry("avc1", [&](ByteWriter& w) {
    w.zeros(6);
    w.be16(1);
    w.zeros(16);
    w.be16(uint16_t(width));
    w.be16(uint16_t(height));
    w.be32(0x00480000);
    w.be32(0x00480000);
    w.be32(0);
    w.be16(1);
    w.zeros(32);
    w.be16(0x18);
    w.be16(0xFFFF);
    w.bytes(entry("avcC", [](ByteWriter& c) {
      const uint8_t cfg[] = {1, 0x64, 0, 0x1f, 0xff, 0xe1, 0, 4, 0x67, 0x64, 0, 0x1f, 1, 0, 2, 0x68, 0xee};
      c.bytes(cfg, sizeof cfg);
    }));
  });
}

Bytes mp4a(int rate, int channels) {
  return entry("mp4a", [&](ByteWriter& w) {
    w.zeros(6);
    w.be16(1);
    w.be16(0);
    w.be16(0);
    w.be32(0);
    w.be16(uint16_t(channels));
    w.be16(16);
    w.be16(0);
    w.be16(0);
    w.be32(uint32_t(rate) << 16);
    w.bytes(entry("esds", [](ByteWriter& c) {
      const uint8_t d[] = {0, 0, 0, 0, 3, 25, 0, 1, 0, 4, 17, 0x40, 0x15, 0, 0, 0, 0, 1, 0xf4, 0, 0, 1, 0xf4, 0, 5, 2, 0x12, 0x10, 6, 1, 2};
      c.bytes(d, sizeof d);
    }));
  });
}

void makeMovie(const fs::path& path, Movie& mv, Bytes& payload) {
  std::ofstream src(path.string() + ".raw", std::ios::binary);
  mv.timescale = 600;
  mv.majorBrand = "isom";
  MovieTrack v;
  v.id = 1;
  v.handler = "vide";
  v.timescale = 30000;
  v.width = 320;
  v.height = 240;
  v.codec = "avc1";
  v.sampleEntries.push_back(avc1(320, 240));
  const uint8_t rot90[36] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0, 0, 0,
                             0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0, 0, 0};
  std::memcpy(v.matrix, rot90, 36);
  v.hasSyncTable = true;
  v.hasCtts = true;
  MovieTrack a;
  a.id = 2;
  a.handler = "soun";
  a.timescale = 48000;
  a.sampleRate = 48000;
  a.channels = 2;
  a.codec = "mp4a";
  a.sampleEntries.push_back(mp4a(48000, 2));

  uint64_t at = 0;
  for (int i = 0; i < 90; ++i) {
    MovieSample s;
    s.size = uint32_t(200 + (i * 37) % 900);
    s.offset = at;
    s.dts = uint64_t(i) * 1001;
    s.duration = 1001;
    s.cto = (i % 3) * 1001;
    s.sync = i % 30 == 0;
    for (uint32_t k = 0; k < s.size; ++k) payload.push_back(uint8_t(i * 7 + k));
    at += s.size;
    v.samples.push_back(s);
  }
  for (int i = 0; i < 141; ++i) {
    MovieSample s;
    s.size = uint32_t(300 + (i * 13) % 200);
    s.offset = at;
    s.dts = uint64_t(i) * 1024;
    s.duration = 1024;
    for (uint32_t k = 0; k < s.size; ++k) payload.push_back(uint8_t(i * 3 + k * 5));
    at += s.size;
    a.samples.push_back(s);
  }
  mv.tracks = {v, a};
  mv.duration = 3 * 600;
  src.write(reinterpret_cast<const char*>(payload.data()), std::streamsize(payload.size()));
  src.close();
  std::ifstream in(path.string() + ".raw", std::ios::binary);
  std::ofstream out(path, std::ios::binary);
  std::string err;
  MuxOptions opt;
  bool ok = writeMovie(mv, in, out, opt, err);
  if (!ok) std::fprintf(stderr, "  writeMovie: %s\n", err.c_str());
}

bool sameSamples(const fs::path& a, const fs::path& b) {
  Movie ma, mb;
  std::string err;
  if (!parseMovieFile(a, ma, err) || !parseMovieFile(b, mb, err)) return false;
  if (ma.tracks.size() != mb.tracks.size()) return false;
  auto fa = readAll(a), fb = readAll(b);
  for (size_t t = 0; t < ma.tracks.size(); ++t) {
    const auto& ta = ma.tracks[t];
    const auto& tb = mb.tracks[t];
    if (ta.samples.size() != tb.samples.size() || ta.codec != tb.codec || ta.timescale != tb.timescale) return false;
    if (std::memcmp(ta.matrix, tb.matrix, 36) != 0) return false;
    for (size_t i = 0; i < ta.samples.size(); ++i) {
      const auto& x = ta.samples[i];
      const auto& y = tb.samples[i];
      if (x.size != y.size || x.dts != y.dts || x.cto != y.cto || x.sync != y.sync || x.duration != y.duration) return false;
      if (std::memcmp(fa.data() + x.offset, fb.data() + y.offset, x.size) != 0) return false;
    }
  }
  return true;
}

}  // namespace

TEST(video_remux_mp4_mov) {
  fs::path dir = tempDir() / "video";
  fs::create_directories(dir);
  Movie mv;
  Bytes payload;
  makeMovie(dir / "synthetic.mp4", mv, payload);
  auto p = jc::probe(dir / "synthetic.mp4");
  REQUIRE(p.result);
  CHECK(p.info.tracks.size() == 2);
  CHECK(p.info.width == 320 && p.info.height == 240);
  CHECK(std::fabs(p.info.durationSeconds - 3.0) < 0.01);

  auto r1 = jc::convert(dir / "synthetic.mp4", dir / "a.mov");
  CHECK_OK(r1);
  CHECK(r1.output.brand == "qt  ");
  auto r2 = jc::convert(dir / "a.mov", dir / "b.mp4");
  CHECK_OK(r2);
  CHECK(r2.output.brand == "isom");
  CHECK(sameSamples(dir / "synthetic.mp4", dir / "a.mov"));
  CHECK(sameSamples(dir / "synthetic.mp4", dir / "b.mp4"));
  auto mov = readAll(dir / "a.mov");
  CHECK(mov.size() > 12 && std::memcmp(mov.data() + 8, "qt  ", 4) == 0);

  Movie in;
  std::string err;
  REQUIRE(parseMovieFile(dir / "a.mov", in, err));
  size_t syncs = 0;
  for (auto& s : in.tracks[0].samples) syncs += s.sync;
  CHECK(syncs == 3);
}

TEST(video_errors) {
  fs::path dir = tempDir() / "videoerr";
  fs::create_directories(dir);
  writeText(dir / "junk.mp4", "this is not a movie at all");
  auto rep = jc::convert(dir / "junk.mp4", dir / "x.mov");
  CHECK(rep.result.status == jc::Status::Unreadable);
  CHECK(!fs::exists(dir / "x.mov"));
}
