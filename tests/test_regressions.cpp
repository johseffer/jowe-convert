// Regression tests for the v0.2.1 fixes.
#include "test_util.hpp"
#include "video/isobmff.hpp"
#include "video/nal.hpp"
#include "video/transcode.hpp"

#ifndef JC_PROJECT_VERSION
#define JC_PROJECT_VERSION ""
#endif

using namespace jowe::convert::detail;

namespace {

struct VideoStats {
  int width = 0, height = 0;
  size_t samples = 0;
  double seconds = 0;
};

bool videoStats(const fs::path& p, VideoStats& s) {
  Movie m;
  std::string err;
  if (!parseMovieFile(p, m, err)) return false;
  for (auto& t : m.tracks)
    if (t.handler == "vide") {
      s.width = t.width;
      s.height = t.height;
      s.samples = t.samples.size();
      s.seconds = t.timescale ? double(t.durationTicks()) / t.timescale : 0;
      return true;
    }
  return false;
}

bool canReencode() {
  auto cap = videoTranscodeCapability();
  if (!cap.available) std::fprintf(stderr, "  skipped: %s\n", cap.reason.c_str());
  return cap.available;
}

fs::path dirFor(const char* name) {
  fs::path d = tempDir() / name;
  fs::create_directories(d);
  return d;
}

bool rewriteMovie(const fs::path& src, const fs::path& dst, const std::function<void(Movie&)>& edit) {
  Movie m;
  std::string err;
  if (!parseMovieFile(src, m, err)) return false;
  edit(m);
  std::ifstream in(src, std::ios::binary);
  std::ofstream out(dst, std::ios::binary | std::ios::trunc);
  return writeMovie(m, in, out, MuxOptions{}, err);
}

// Sets the version byte of every ctts box; offsets are stored the same way in both versions.
int setCttsVersion(const fs::path& p, uint8_t version) {
  auto b = readAll(p);
  int n = 0;
  for (size_t i = 4; i + 8 < b.size(); ++i)
    if (std::memcmp(&b[i], "ctts", 4) == 0) {
      b[i + 4] = version;
      ++n;
    }
  writeAll(p, b.data(), b.size());
  return n;
}

int cttsVersion(const fs::path& p) {
  auto b = readAll(p);
  for (size_t i = 4; i + 8 < b.size(); ++i)
    if (std::memcmp(&b[i], "ctts", 4) == 0) return b[i + 4];
  return -1;
}

bool matrixOf(const fs::path& p, uint8_t* m) {
  Movie mv;
  std::string err;
  if (!parseMovieFile(p, mv, err)) return false;
  for (auto& t : mv.tracks)
    if (t.handler == "vide") {
      std::memcpy(m, t.matrix, 36);
      return true;
    }
  return false;
}

const uint8_t kRot90[36] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0, 0, 0};
const uint8_t kIdentity[36] = {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0,
                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0, 0, 0};

}  // namespace

TEST(core_version_single_source) {
  std::fprintf(stderr, "  library %s, project %s\n", jc::version(), JC_PROJECT_VERSION);
  CHECK(std::string(jc::version()) == JC_PROJECT_VERSION);
  CHECK(std::string(jc::version()) != "0.1.0");
}

TEST(video_ctts_signed_offsets) {
  fs::path dir = dirFor("ctts");
  ParameterSets ps;
  ps.sps = {{0x67, 0x64, 0x00, 0x1f, 0xac}};
  ps.pps = {{0x68, 0xee, 0x3c, 0x80}};
  Movie mv;
  MovieTrack t;
  t.id = 1;
  t.handler = "vide";
  t.timescale = 600;
  t.width = 320;
  t.height = 240;
  t.codec = "avc1";
  t.sampleEntries.push_back(videoSampleEntry("avc1", buildAvcC(ps), 320, 240));
  const int32_t offsets[6] = {0, 40, -20, -20, 40, -20};
  Bytes payload(6 * 10, 0x22);
  for (int i = 0; i < 6; ++i) {
    MovieSample s;
    s.offset = uint64_t(i) * 10;
    s.size = 10;
    s.dts = uint64_t(i) * 20;
    s.duration = 20;
    s.cto = offsets[i];
    t.samples.push_back(s);
  }
  t.hasCtts = true;
  mv.tracks.push_back(t);
  writeAll(dir / "raw.bin", payload.data(), payload.size());
  {
    std::ifstream raw(dir / "raw.bin", std::ios::binary);
    std::ofstream o(dir / "v1.mp4", std::ios::binary);
    std::string err;
    REQUIRE(writeMovie(mv, raw, o, MuxOptions{}, err));
  }
  CHECK(cttsVersion(dir / "v1.mp4") == 1);
  fs::copy_file(dir / "v1.mp4", dir / "v0.mp4", fs::copy_options::overwrite_existing);
  CHECK(setCttsVersion(dir / "v0.mp4", 0) == 1);
  for (const char* name : {"v1.mp4", "v0.mp4"}) {
    Movie back;
    std::string err;
    REQUIRE(parseMovieFile(dir / name, back, err));
    REQUIRE(back.tracks.size() == 1 && back.tracks[0].samples.size() == 6);
    for (int i = 0; i < 6; ++i) CHECK(back.tracks[0].samples[size_t(i)].cto == offsets[i]);
  }
}

TEST(video_transcode_bframes_ctts) {
  if (!canReencode()) return;
  fs::path dir = dirFor("bframes");
  auto r = writeSyntheticClip(dir / "b.mp4", 1280, 720, 30, 45, true, "avc1", 6000, 8, 2);
  REQUIRE(r);
  Movie m;
  std::string err;
  REQUIRE(parseMovieFile(dir / "b.mp4", m, err));
  int32_t maxCto = 0;
  for (auto& t : m.tracks)
    if (t.handler == "vide")
      for (auto& s : t.samples) maxCto = std::max(maxCto, s.cto);
  if (maxCto == 0) {
    std::fprintf(stderr, "  skipped: this encoder made no B-frames\n");
    return;
  }
  // AVFoundation's layout: offsets relative to the first frame's, so the B-frames get negative ones.
  REQUIRE(rewriteMovie(dir / "b.mp4", dir / "ctts_v1.mp4", [&](Movie& mv) {
    for (auto& t : mv.tracks)
      if (t.handler == "vide") {
        const int32_t first = t.samples[0].cto;
        for (auto& s : t.samples) s.cto -= first;
      }
  }));
  CHECK(cttsVersion(dir / "ctts_v1.mp4") == 1);
  fs::copy_file(dir / "ctts_v1.mp4", dir / "ctts_v0.mp4", fs::copy_options::overwrite_existing);
  setCttsVersion(dir / "ctts_v0.mp4", 0);
  VideoStats in;
  REQUIRE(videoStats(dir / "ctts_v1.mp4", in));
  for (const char* name : {"ctts_v1.mp4", "ctts_v0.mp4"}) {
    jc::ConvertOptions o;
    o.video.maxShortSide = 480;
    fs::path out = dir / (std::string("out_") + name);
    auto rep = jc::convert(dir / name, out, o);
    CHECK_OK(rep);
    if (!rep.result) continue;
    VideoStats v;
    REQUIRE(videoStats(out, v));
    const auto size = fs::file_size(out);
    std::fprintf(stderr, "  %s: B-frames (max offset %d), %zu -> %zu frames, %.4f s -> %.4f s, %llu bytes\n", name,
                 int(maxCto), in.samples, v.samples, in.seconds, v.seconds, static_cast<unsigned long long>(size));
    CHECK(v.samples == in.samples);
    CHECK(std::fabs(v.seconds - in.seconds) <= 1.0 / 30 + 1e-6);
    CHECK(size < (8u << 20));
  }
}

TEST(video_transcode_audio_edit_list) {
  if (!canReencode()) return;
  fs::path dir = dirFor("primed");
  REQUIRE(writeSyntheticClip(dir / "src.mp4", 1280, 720, 30, 45, true));
  const int64_t priming = 2112;
  uint64_t audioDuration = 0;
  REQUIRE(rewriteMovie(dir / "src.mp4", dir / "primed.mp4", [&](Movie& mv) {
    for (auto& t : mv.tracks)
      if (t.handler == "soun") {
        audioDuration = uint64_t(double(t.durationTicks() - uint64_t(priming)) * mv.timescale / t.timescale);
        t.edts = buildEdts({{audioDuration, priming, 1}});
      }
  }));
  jc::ConvertOptions o;
  o.video.maxShortSide = 480;
  CHECK_OK(jc::convert(dir / "primed.mp4", dir / "primed_480.mp4", o));
  Movie out;
  std::string err;
  REQUIRE(parseMovieFile(dir / "primed_480.mp4", out, err));
  const MovieTrack* v = nullptr;
  const MovieTrack* a = nullptr;
  for (auto& t : out.tracks) {
    if (t.handler == "vide") v = &t;
    if (t.handler == "soun") a = &t;
  }
  REQUIRE(v && a);
  std::vector<EditEntry> edits;
  REQUIRE(parseEdts(a->edts, edits));
  REQUIRE(edits.size() == 1);
  const double audioSkip = double(edits[0].mediaTime) / a->timescale;
  const double videoStart = double(int64_t(v->samples[0].dts) + v->samples[0].cto) / v->timescale;
  std::fprintf(stderr, "  audio edit kept: skips %lld samples (%.1f ms); video starts at %.1f ms; both present at 0\n",
               static_cast<long long>(edits[0].mediaTime), audioSkip * 1000, videoStart * 1000);
  CHECK(edits[0].mediaTime == priming);
  CHECK(std::llabs(int64_t(edits[0].segmentDuration) - int64_t(audioDuration)) <= 1);
  CHECK(v->edts.empty());
  CHECK(std::fabs(videoStart) < 1e-9);

  jc::ConvertOptions aac;
  aac.video.audio = jc::VideoOptions::Audio::Aac;
  CHECK_OK(jc::convert(dir / "primed.mp4", dir / "primed_aac.mov", aac));
  Movie re;
  REQUIRE(parseMovieFile(dir / "primed_aac.mov", re, err));
  for (auto& t : re.tracks)
    if (t.handler == "soun") {
      std::vector<EditEntry> e;
      if (parseEdts(t.edts, e) && !e.empty())
        std::fprintf(stderr, "  re-encoded AAC keeps its encoder's edit: skips %lld samples\n",
                     static_cast<long long>(e[0].mediaTime));
      else
        std::fprintf(stderr, "  re-encoded AAC: the OS encoder wrote no edit list\n");
    }
}

TEST(video_transcode_rotation) {
  if (!canReencode()) return;
  fs::path dir = dirFor("rotation");
  REQUIRE(writeSyntheticClip(dir / "src.mp4", 640, 360, 30, 30, false));
  REQUIRE(rewriteMovie(dir / "src.mp4", dir / "rot90.mp4", [&](Movie& mv) {
    for (auto& t : mv.tracks)
      if (t.handler == "vide") std::memcpy(t.matrix, kRot90, 36);
  }));
  uint8_t mtx[36];

  jc::ConvertOptions bake;
  bake.video.bitrateKbps = 1500;
  CHECK_OK(jc::convert(dir / "rot90.mp4", dir / "baked.mp4", bake));
  VideoStats b;
  REQUIRE(videoStats(dir / "baked.mp4", b));
  REQUIRE(matrixOf(dir / "baked.mp4", mtx));
  std::fprintf(stderr, "  bake (default): 640x360 + 90 degree matrix -> %dx%d, matrix %s\n", b.width, b.height,
               std::memcmp(mtx, kIdentity, 36) == 0 ? "identity" : "rotated");
  CHECK(b.width == 360 && b.height == 640);
  CHECK(std::memcmp(mtx, kIdentity, 36) == 0);

  jc::ConvertOptions keep = bake;
  keep.video.bakeRotation = false;
  CHECK_OK(jc::convert(dir / "rot90.mp4", dir / "metadata.mp4", keep));
  VideoStats k;
  REQUIRE(videoStats(dir / "metadata.mp4", k));
  REQUIRE(matrixOf(dir / "metadata.mp4", mtx));
  std::fprintf(stderr, "  metadata: -> %dx%d, matrix %s\n", k.width, k.height,
               std::memcmp(mtx, kRot90, 36) == 0 ? "90 degrees" : "other");
  CHECK(k.width == 640 && k.height == 360);
  CHECK(std::memcmp(mtx, kRot90, 36) == 0);

  CHECK_OK(jc::convert(dir / "rot90.mp4", dir / "remux.mov"));
  REQUIRE(matrixOf(dir / "remux.mov", mtx));
  CHECK(std::memcmp(mtx, kRot90, 36) == 0);
}
