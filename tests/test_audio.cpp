#include "test_util.hpp"

#ifndef JC_MINIMP3_VECTORS
#define JC_MINIMP3_VECTORS ""
#endif

TEST(audio_pcm_containers) {
  fs::path dir = tempDir() / "audio";
  fs::create_directories(dir);
  makeWav(dir / "tone.wav", 44100, 2, 0.5);
  auto p = jc::probe(dir / "tone.wav");
  REQUIRE(p.result);
  CHECK(p.info.sampleRate == 44100 && p.info.channels == 2);
  CHECK(std::fabs(p.info.durationSeconds - 0.5) < 0.001);

  for (const char* ext : {"aiff", "caf", "flac", "wav"}) {
    for (int bits : {16, 24, 32}) {
      if (std::string(ext) == "flac" && bits == 32) continue;
      jc::ConvertOptions o;
      o.audio.bitDepth = bits;
      fs::path out = dir / ("tone_" + std::to_string(bits) + "." + ext);
      auto rep = jc::convert(dir / "tone.wav", out, o);
      CHECK_OK(rep);
      CHECK(rep.output.sampleRate == 44100);
      CHECK(rep.output.channels == 2);
      CHECK(std::fabs(rep.output.durationSeconds - 0.5) < 0.001);
      fs::path back = dir / ("back_" + std::to_string(bits) + "_" + ext + ".wav");
      jc::ConvertOptions b16;
      b16.audio.bitDepth = 16;
      auto rep2 = jc::convert(out, back, b16);
      CHECK_OK(rep2);
      auto a = readAll(dir / "tone.wav"), b = readAll(back);
      CHECK(a.size() == b.size());
      if (a.size() == b.size() && a.size() > 44) {
        int maxDiff = 0;
        for (size_t i = 44; i + 1 < a.size(); i += 2) {
          int x = int16_t(a[i] | a[i + 1] << 8), y = int16_t(b[i] | b[i + 1] << 8);
          maxDiff = std::max(maxDiff, std::abs(x - y));
        }
        CHECK(maxDiff <= 1);
      }
    }
  }
}

TEST(audio_resample_and_channels) {
  fs::path dir = tempDir() / "resample";
  fs::create_directories(dir);
  makeWav(dir / "tone.wav", 44100, 2, 1.0);
  jc::ConvertOptions o;
  o.audio.sampleRate = 48000;
  o.audio.channels = 1;
  auto rep = jc::convert(dir / "tone.wav", dir / "mono48.wav", o);
  CHECK_OK(rep);
  CHECK(rep.output.sampleRate == 48000 && rep.output.channels == 1);
  CHECK(std::fabs(rep.output.durationSeconds - 1.0) < 0.002);
  jc::ConvertOptions s;
  s.audio.channels = 2;
  s.audio.sampleRate = 22050;
  rep = jc::convert(dir / "mono48.wav", dir / "stereo22.flac", s);
  CHECK_OK(rep);
  CHECK(rep.output.sampleRate == 22050 && rep.output.channels == 2);
}

TEST(audio_mp3_decode) {
  fs::path vec = fs::path(JC_MINIMP3_VECTORS) / "l3-he_44khz.bit";
  if (!fs::exists(vec)) {
    std::fprintf(stderr, "  skipped: no minimp3 vector at %s\n", vec.string().c_str());
    return;
  }
  fs::path dir = tempDir() / "mp3";
  fs::create_directories(dir);
  fs::copy_file(vec, dir / "v.mp3", fs::copy_options::overwrite_existing);
  auto p = jc::probe(dir / "v.mp3");
  REQUIRE(p.result);
  CHECK(p.info.codec == "mp3" && p.info.sampleRate == 44100);
  auto rep = jc::convert(dir / "v.mp3", dir / "v.wav");
  CHECK_OK(rep);
  const auto pcmRef = fs::file_size(fs::path(JC_MINIMP3_VECTORS) / "l3-he_44khz.pcm");
  CHECK(rep.output.durationSeconds > 0);
  CHECK(std::fabs(rep.output.durationSeconds - double(pcmRef) / 2 / p.info.channels / 44100) < 0.05);
}

TEST(audio_encoded_targets) {
  fs::path dir = tempDir() / "aac";
  fs::create_directories(dir);
  makeWav(dir / "tone.wav", 48000, 2, 1.0);
  for (const char* id : {"m4a", "mp3"}) {
    auto f = jc::findFormat(id);
    REQUIRE(f.has_value());
    fs::path out = dir / (std::string("tone.") + id);
    auto rep = jc::convert(dir / "tone.wav", out);
    if (!f->canWrite) {
      CHECK(rep.result.status == jc::Status::NotWritable);
      CHECK(!f->writeReason.empty());
      std::fprintf(stderr, "  %s not writable here: %s\n", id, f->writeReason.c_str());
      continue;
    }
    CHECK_OK(rep);
    if (!rep.result) continue;
    CHECK(fs::file_size(out) > 1000);
    if (f->canRead) {
      auto back = jc::convert(out, dir / (std::string("back_") + id + ".wav"));
      CHECK_OK(back);
      CHECK(std::fabs(back.output.durationSeconds - 1.0) < 0.1);
    }
  }
}
