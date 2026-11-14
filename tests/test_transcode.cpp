#include "test_util.hpp"
#include "video/isobmff.hpp"
#include "video/nal.hpp"
#include "video/transcode.hpp"

using namespace jowe::convert::detail;

namespace {

struct TrackStats {
  int width = 0, height = 0;
  size_t samples = 0;
  double seconds = 0;
  uint64_t bytes = 0;
  std::string codec;
};

bool trackStats(const fs::path& p, const char* handler, TrackStats& s) {
  Movie m;
  std::string err;
  if (!parseMovieFile(p, m, err)) {
    std::fprintf(stderr, "  parse %s: %s\n", p.string().c_str(), err.c_str());
    return false;
  }
  for (auto& t : m.tracks) {
    if (t.handler != handler) continue;
    s.width = t.width;
    s.height = t.height;
    s.samples = t.samples.size();
    s.seconds = t.timescale ? double(t.durationTicks()) / t.timescale : 0;
    s.codec = t.codec;
    for (auto& x : t.samples) s.bytes += x.size;
    return true;
  }
  return false;
}

bool capable() {
  auto cap = videoTranscodeCapability();
  if (!cap.available) std::fprintf(stderr, "  skipped: %s\n", cap.reason.c_str());
  return cap.available;
}

fs::path synthetic(const std::string& name, int w, int h, int fps, int frames, bool audio, int kbps = 8000,
                   int noise = 32) {
  fs::path dir = tempDir() / "transcode";
  fs::create_directories(dir);
  fs::path p = dir / name;
  if (!fs::exists(p)) {
    auto r = writeSyntheticClip(p, w, h, fps, frames, audio, "avc1", kbps, noise);
    if (!r) {
      std::fprintf(stderr, "  synthetic clip failed: %s\n", r.message.c_str());
      ++g_failures;
    }
  }
  return p;
}

}  // namespace

TEST(video_transcode_capability) {
  auto mp4 = jc::findFormat("mp4");
  auto mov = jc::findFormat("mov");
  REQUIRE(mp4 && mov);
  auto cap = videoTranscodeCapability();
  CHECK(mp4->canTranscode == cap.available && mov->canTranscode == cap.available);
#if defined(JC_EXPECT_VIDEO_ENCODER)
  CHECK(cap.available);
  CHECK(!mp4->videoEncoders.empty() && mp4->videoEncoders[0] == "h264");
  std::fprintf(stderr, "  backend %s, encoders:", cap.backend.c_str());
  for (auto& e : cap.encoders) std::fprintf(stderr, " %s", e.c_str());
  std::fprintf(stderr, "\n");
#else
  CHECK(!cap.available);
  CHECK(!mp4->transcodeReason.empty());
  std::fprintf(stderr, "  not available: %s\n", mp4->transcodeReason.c_str());

  fs::path dir = tempDir() / "transcode_unsupported";
  fs::create_directories(dir);
  ParameterSets ps;
  ps.sps = {{0x67, 0x64, 0x00, 0x1f, 0xac}};
  ps.pps = {{0x68, 0xee, 0x3c, 0x80}};
  Movie mv;
  MovieTrack t;
  t.id = 1;
  t.handler = "vide";
  t.timescale = 30000;
  t.width = 1920;
  t.height = 1080;
  t.codec = "avc1";
  t.sampleEntries.push_back(videoSampleEntry("avc1", buildAvcC(ps), 1920, 1080));
  Bytes payload(3 * 100, 0x11);
  for (int i = 0; i < 3; ++i) {
    MovieSample s;
    s.offset = uint64_t(i) * 100;
    s.size = 100;
    s.dts = uint64_t(i) * 1001;
    s.duration = 1001;
    t.samples.push_back(s);
  }
  mv.tracks.push_back(t);
  writeAll(dir / "raw.bin", payload.data(), payload.size());
  {
    std::ifstream raw(dir / "raw.bin", std::ios::binary);
    std::ofstream o(dir / "fake.mp4", std::ios::binary);
    std::string err;
    REQUIRE(writeMovie(mv, raw, o, MuxOptions{}, err));
  }
  jc::ConvertOptions o;
  o.video.maxShortSide = 720;
  auto rep = jc::convert(dir / "fake.mp4", dir / "out.mp4", o);
  CHECK(rep.result.status == jc::Status::Unsupported);
  CHECK(rep.result.message.find(mp4->transcodeReason) != std::string::npos);
  CHECK(!fs::exists(dir / "out.mp4"));
  o.video = jc::VideoOptions{};
  auto remux = jc::convert(dir / "fake.mp4", dir / "remux.mov", o);
  CHECK_OK(remux);
#endif
}

TEST(video_transcode_1080_to_720) {
  if (!capable()) return;
  fs::path src = synthetic("src1080.mp4", 1920, 1080, 30, 60, true);
  TrackStats in;
  REQUIRE(trackStats(src, "vide", in));
  CHECK(in.width == 1920 && in.height == 1080 && in.samples == 60);
  for (const char* ext : {"mp4", "mov"}) {
    jc::ConvertOptions o;
    o.video.maxShortSide = 720;
    fs::path out = src.parent_path() / (std::string("out720.") + ext);
    auto rep = jc::convert(src, out, o);
    CHECK_OK(rep);
    if (!rep.result) continue;
    TrackStats v, a;
    REQUIRE(trackStats(out, "vide", v));
    std::fprintf(stderr, "  %s: %dx%d, %zu frames, %.4f s (source %.4f s), %s\n", ext, v.width, v.height, v.samples,
                 v.seconds, in.seconds, v.codec.c_str());
    CHECK(v.width == 1280 && v.height == 720);
    CHECK(v.samples == in.samples);
    CHECK(std::fabs(v.seconds - in.seconds) <= 1.0 / 30 + 1e-6);
    CHECK(v.codec == "avc1");
    CHECK(trackStats(out, "soun", a) && a.codec == "mp4a");
    CHECK(rep.output.width == 1280 && rep.output.height == 720);
  }
}

TEST(video_transcode_audio_drop) {
  if (!capable()) return;
  fs::path src = synthetic("src1080.mp4", 1920, 1080, 30, 60, true);
  TrackStats a;
  REQUIRE(trackStats(src, "soun", a));
  jc::ConvertOptions o;
  o.video.maxShortSide = 480;
  o.video.audio = jc::VideoOptions::Audio::Drop;
  auto rep = jc::convert(src, src.parent_path() / "noaudio.mp4", o);
  CHECK_OK(rep);
  TrackStats v, none;
  CHECK(trackStats(src.parent_path() / "noaudio.mp4", "vide", v) && v.height == 480 && v.width == 852);
  CHECK(!trackStats(src.parent_path() / "noaudio.mp4", "soun", none));
  std::fprintf(stderr, "  mp4: audio dropped -> %dx%d, %zu frames, audio track %s
", v.width, v.height, v.samples,
               trackStats(src.parent_path() / "noaudio.mp4", "soun", none) ? "present" : "absent");
  jc::ConvertOptions remux;
  remux.video.audio = jc::VideoOptions::Audio::Drop;
  CHECK_OK(jc::convert(src, src.parent_path() / "noaudio_remux.mov", remux));
  CHECK(!trackStats(src.parent_path() / "noaudio_remux.mov", "soun", none));

  jc::ConvertOptions aac;
  aac.video.audio = jc::VideoOptions::Audio::Aac;
  aac.video.audioBitrateKbps = 96;
  auto r2 = jc::convert(src, src.parent_path() / "aac.mov", aac);
  CHECK_OK(r2);
  TrackStats a2;
  CHECK(trackStats(src.parent_path() / "aac.mov", "soun", a2) && a2.codec == "mp4a");
}

TEST(video_transcode_bitrate) {
  if (!capable()) return;
  fs::path src = synthetic("src720_grain.mp4", 1280, 720, 30, 150, false, 20000, 8);
  for (int kbps : {2000, 5000}) {
    jc::ConvertOptions o;
    o.video.codec = jc::VideoOptions::Codec::H264;
    o.video.bitrateKbps = kbps;
    fs::path out = src.parent_path() / ("br" + std::to_string(kbps) + ".mp4");
    auto rep = jc::convert(src, out, o);
    CHECK_OK(rep);
    TrackStats v;
    REQUIRE(trackStats(out, "vide", v));
    const double measured = double(v.bytes) * 8 / v.seconds / 1000;
    std::fprintf(stderr, "  target %d kbit/s, measured %.0f kbit/s (%+.1f%%)\n", kbps, measured,
                 100.0 * (measured - kbps) / kbps);
    CHECK(std::fabs(measured - kbps) <= 0.15 * kbps);
  }
}

TEST(video_transcode_frame_rate_cap) {
  if (!capable()) return;
  fs::path src = synthetic("src60fps.mp4", 640, 360, 60, 60, false);
  jc::ConvertOptions o;
  o.video.maxFrameRate = 30;
  fs::path out = src.parent_path() / "fps30.mp4";
  auto rep = jc::convert(src, out, o);
  CHECK_OK(rep);
  TrackStats v;
  REQUIRE(trackStats(out, "vide", v));
  std::fprintf(stderr, "  60 fps -> %zu frames in %.4f s\n", v.samples, v.seconds);
  CHECK(v.samples == 30);
  CHECK(std::fabs(v.seconds - 1.0) <= 1.0 / 30 + 1e-6);
}

TEST(video_transcode_hevc) {
  if (!capable()) return;
  auto cap = videoTranscodeCapability();
  if (std::find(cap.encoders.begin(), cap.encoders.end(), "hevc") == cap.encoders.end()) {
    std::fprintf(stderr, "  skipped: no HEVC encoder here\n");
    return;
  }
  fs::path src = synthetic("src1080.mp4", 1920, 1080, 30, 60, true);
  jc::ConvertOptions o;
  o.video.codec = jc::VideoOptions::Codec::HEVC;
  o.video.maxShortSide = 720;
  fs::path out = src.parent_path() / "hevc.mov";
  auto rep = jc::convert(src, out, o);
  CHECK_OK(rep);
  TrackStats v;
  CHECK(trackStats(out, "vide", v) && v.codec == "hvc1" && v.samples == 60 && v.height == 720);
}

TEST(video_transcode_cancel) {
  if (!capable()) return;
  fs::path src = synthetic("src1080.mp4", 1920, 1080, 30, 60, true);
  fs::path dir = tempDir() / "transcode_cancel";
  fs::create_directories(dir);
  std::atomic<bool> cancel{false};
  double seen = 0;
  jc::ConvertOptions o;
  o.video.maxShortSide = 720;
  o.cancel = &cancel;
  o.progress = [&](double p) {
    seen = p;
    if (p > 0.3) cancel = true;
  };
  auto rep = jc::convert(src, dir / "cancelled.mp4", o);
  CHECK(rep.result.status == jc::Status::Cancelled);
  CHECK(seen > 0.3 && seen < 0.9);
  std::fprintf(stderr, "  mp4: cancelled at %.0f%% with status %s
", seen * 100, jc::toString(rep.result.status));
  size_t leftovers = 0;
  for (auto& e : fs::directory_iterator(dir)) {
    std::fprintf(stderr, "  leftover %s\n", e.path().filename().string().c_str());
    ++leftovers;
  }
  CHECK(leftovers == 0);
}
