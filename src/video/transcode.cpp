#include "transcode.hpp"

#include <cmath>
#include <fstream>
#include <map>

#include "../audio/audio.hpp"
#include "backend.hpp"
#include "nal.hpp"

namespace jowe::convert::detail {

namespace {

bool isAvc(const std::string& c) { return c == "avc1" || c == "avc3"; }
bool isHevc(const std::string& c) { return c == "hvc1" || c == "hev1"; }

struct Frame {
  int64_t pts = 0;
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> y, uv;
};

// Box-filtered NV12 resize (each output pixel averages the source pixels it covers).
void scaleNv12(const jc_video_frame& f, uint32_t dw, uint32_t dh, Frame& out) {
  out.width = dw;
  out.height = dh;
  out.y.resize(size_t(dw) * dh);
  out.uv.resize(size_t(dw) * dh / 2);
  const uint32_t sw = f.width & ~1u, sh = f.height & ~1u;
  if (sw == dw && sh == dh) {
    for (uint32_t r = 0; r < dh; ++r) std::memcpy(&out.y[size_t(r) * dw], f.plane[0] + size_t(r) * f.stride[0], dw);
    for (uint32_t r = 0; r < dh / 2; ++r)
      std::memcpy(&out.uv[size_t(r) * dw], f.plane[1] + size_t(r) * f.stride[1], dw);
    return;
  }
  auto spans = [](uint32_t src, uint32_t dst) {
    std::vector<std::pair<uint32_t, uint32_t>> s(dst);
    for (uint32_t i = 0; i < dst; ++i) {
      uint32_t a = uint32_t(uint64_t(i) * src / dst), b = uint32_t(uint64_t(i + 1) * src / dst);
      s[i] = {a, std::max(b, a + 1)};
    }
    return s;
  };
  const auto xs = spans(sw, dw), ys = spans(sh, dh);
  for (uint32_t r = 0; r < dh; ++r)
    for (uint32_t c = 0; c < dw; ++c) {
      uint32_t sum = 0, n = 0;
      for (uint32_t y = ys[r].first; y < ys[r].second; ++y) {
        const uint8_t* row = f.plane[0] + size_t(y) * f.stride[0];
        for (uint32_t x = xs[c].first; x < xs[c].second; ++x) sum += row[x], ++n;
      }
      out.y[size_t(r) * dw + c] = uint8_t((sum + n / 2) / n);
    }
  const auto cxs = spans(sw / 2, dw / 2), cys = spans(sh / 2, dh / 2);
  for (uint32_t r = 0; r < dh / 2; ++r)
    for (uint32_t c = 0; c < dw / 2; ++c) {
      uint32_t su = 0, sv = 0, n = 0;
      for (uint32_t y = cys[r].first; y < cys[r].second; ++y) {
        const uint8_t* row = f.plane[1] + size_t(y) * f.stride[1];
        for (uint32_t x = cxs[c].first; x < cxs[c].second; ++x) su += row[x * 2], sv += row[x * 2 + 1], ++n;
      }
      out.uv[size_t(r) * dw + c * 2] = uint8_t((su + n / 2) / n);
      out.uv[size_t(r) * dw + c * 2 + 1] = uint8_t((sv + n / 2) / n);
    }
}

jc_video_frame view(const Frame& f) {
  jc_video_frame v{};
  v.struct_size = sizeof v;
  v.format = JC_PIX_NV12;
  v.width = f.width;
  v.height = f.height;
  v.plane[0] = f.y.data();
  v.plane[1] = f.uv.data();
  v.stride[0] = v.stride[1] = f.width;
  v.pts_us = f.pts;
  return v;
}

struct EncodedSample {
  uint64_t offset = 0;
  uint32_t size = 0;
  int64_t pts = 0, dts = 0;
  bool key = false;
};

struct EncodeSink {
  std::ofstream* file = nullptr;
  uint64_t written = 0;
  std::vector<EncodedSample> samples;
  bool ok = true;
};

int32_t onPacket(void* user, const uint8_t* data, size_t size, int64_t pts, int64_t dts, int32_t key) {
  auto* s = static_cast<EncodeSink*>(user);
  s->file->write(reinterpret_cast<const char*>(data), std::streamsize(size));
  if (!*s->file) {
    s->ok = false;
    return JC_ERR_IO;
  }
  s->samples.push_back({s->written, uint32_t(size), pts, dts, key != 0});
  s->written += size;
  return JC_OK;
}

// Fills a video track from the encoder's samples (times in microseconds) at the given timescale.
bool buildVideoTrack(const jc_video_backend* b, jc_video_encoder* enc, const std::string& codec, uint32_t w,
                     uint32_t h, std::vector<EncodedSample> samples, int64_t lastDurationUs, uint32_t timescale,
                     MovieTrack& t, std::string& err) {
  size_t cfgSize = 0;
  b->encoder_config(enc, nullptr, 0, &cfgSize);
  Bytes cfg(cfgSize);
  if (!cfgSize || b->encoder_config(enc, cfg.data(), cfg.size(), &cfgSize) != JC_OK) {
    err = "the encoder gave no codec configuration";
    return false;
  }
  if (samples.empty()) {
    err = "the encoder produced no samples";
    return false;
  }
  std::stable_sort(samples.begin(), samples.end(), [](const EncodedSample& a, const EncodedSample& b2) { return a.dts < b2.dts; });
  const int64_t base = samples.front().dts;
  auto ticks = [&](int64_t us) { return int64_t(std::llround(double(us - base) * timescale / 1e6)); };
  t = MovieTrack();
  t.id = 1;
  t.handler = "vide";
  t.handlerName = "VideoHandler";
  t.timescale = timescale;
  t.codec = codec;
  t.width = int(w);
  t.height = int(h);
  t.sampleEntries.push_back(videoSampleEntry(codec, cfg, int(w), int(h)));
  t.hasSyncTable = true;
  for (size_t i = 0; i < samples.size(); ++i) {
    const auto& s = samples[i];
    MovieSample m;
    m.offset = s.offset;
    m.size = s.size;
    m.dts = uint64_t(ticks(s.dts));
    const int64_t next = i + 1 < samples.size() ? ticks(samples[i + 1].dts) : ticks(s.dts + lastDurationUs);
    m.duration = uint32_t(std::max<int64_t>(1, next - int64_t(m.dts)));
    m.cto = int32_t(ticks(s.pts) - ticks(s.dts));
    m.sync = s.key;
    t.hasCtts = t.hasCtts || m.cto != 0;
    t.samples.push_back(m);
  }
  t.mediaDuration = t.durationTicks();
  return true;
}

struct TempFile {
  fs::path path;
  ~TempFile() {
    if (!path.empty()) {
      std::error_code ec;
      fs::remove(path, ec);
    }
  }
};

fs::path tempBeside(const fs::path& out, const char* suffix) {
  fs::path p = out;
  p += suffix;
  return p;
}

uint32_t evenDown(double v) { return std::max<uint32_t>(2, uint32_t(v) & ~1u); }

// Encodes pcm to AAC in a temporary M4A and returns its audio track (samples read from that file).
Result aacTrack(const Context& ctx, Pcm& pcm, int kbps, const fs::path& tmp, MovieTrack& track) {
  std::string reason;
  if (!osAudioCanEncode("m4a", &reason)) return Result::error(Status::Unsupported, reason);
  int rate = pcm.sampleRate, channels = pcm.channels;
  osAudioNegotiate("m4a", rate, channels, kbps);
  remix(pcm, channels);
  resample(pcm, rate);
  if (!ctx.io.canWrite(tmp)) return Result::error(Status::AccessDenied, "Access denied: " + tmp.string());
  std::string err;
  if (!osAudioEncode(pcm, "m4a", kbps, tmp, err)) return Result::error(Status::Failed, "AAC encoding failed: " + err);
  Movie m;
  if (!parseMovieFile(tmp, m, err)) return Result::error(Status::Failed, "AAC encoding failed: " + err);
  for (auto& t : m.tracks)
    if (t.handler == "soun") {
      track = t;
      track.edts.clear();
      return Result::ok();
    }
  return Result::error(Status::Failed, "the AAC encoder wrote no audio track");
}

}  // namespace

double nominalFrameRate(const MovieTrack& t) {
  if (t.samples.empty() || !t.timescale) return 0;
  const uint64_t d = t.durationTicks();
  return d ? double(t.samples.size()) * t.timescale / double(d) : 0;
}

bool needsTranscode(const VideoOptions& o, const MovieTrack& v) {
  if (o.codec != VideoOptions::Codec::Keep || o.bitrateKbps > 0 || o.quality != VideoOptions::Quality::Auto ||
      o.audio == VideoOptions::Audio::Aac)
    return true;
  const int shortSide = std::min(v.width, v.height);
  if (o.maxShortSide > 0 && shortSide > o.maxShortSide) return true;
  if (o.maxFrameRate > 0 && nominalFrameRate(v) > o.maxFrameRate + 0.01) return true;
  return false;
}

TranscodeCapability videoTranscodeCapability() {
  TranscodeCapability c;
  std::string reason;
  const jc_video_backend* b = pickVideoBackend("avc1", "avc1", &reason);
  if (!b) {
    c.reason = reason.empty() ? "No video encoder on this platform" : reason;
    return c;
  }
  c.available = true;
  c.backend = b->name;
  c.encoders.push_back("h264");
  char buf[256] = {0};
  for (auto* x : videoBackends())
    if (x->can_encode("hvc1", buf, sizeof buf)) {
      c.encoders.push_back("hevc");
      break;
    }
  return c;
}

Result transcodeVideo(const Context& ctx, const fs::path& in, const Movie& src, const fs::path& out,
                      const std::string& outFormat) {
  const VideoOptions& opt = ctx.options->video;
  const MovieTrack* video = nullptr;
  const MovieTrack* audio = nullptr;
  for (auto& t : src.tracks) {
    if (t.handler == "vide" && !video && !t.samples.empty()) video = &t;
    if (t.handler == "soun" && !audio && !t.samples.empty()) audio = &t;
  }
  if (!video) return Result::error(Status::Unreadable, "The file has no video track");
  if (!isAvc(video->codec) && !isHevc(video->codec))
    return Result::error(Status::Unsupported, "Re-encoding needs H.264 or HEVC input, not '" + video->codec + "'");

  const std::string outCodec = opt.codec == VideoOptions::Codec::HEVC ? "hvc1" : "avc1";
  const std::string inCodec = isHevc(video->codec) ? "hvc1" : "avc1";
  std::string reason;
  const jc_video_backend* b = pickVideoBackend(inCodec, outCodec, &reason);
  if (!b) return Result::error(Status::Unsupported, "Video can't be re-encoded here: " + reason);

  Bytes config = sampleEntryConfig(video->sampleEntries.empty() ? Bytes() : video->sampleEntries[0]);
  if (config.empty()) return Result::error(Status::Unreadable, "The video track has no decoder configuration");

  const uint32_t srcW = uint32_t(video->width), srcH = uint32_t(video->height);
  if (!srcW || !srcH) return Result::error(Status::Unreadable, "The video track has no size");
  double scale = 1.0;
  const uint32_t shortSide = std::min(srcW, srcH);
  if (opt.maxShortSide > 0 && shortSide > uint32_t(opt.maxShortSide)) scale = double(opt.maxShortSide) / shortSide;
  const uint32_t dstW = evenDown(std::lround(srcW * scale)), dstH = evenDown(std::lround(srcH * scale));

  const double srcFps = nominalFrameRate(*video);
  double fps = srcFps > 0 ? srcFps : 30.0;
  const bool capFps = opt.maxFrameRate > 0 && fps > opt.maxFrameRate + 0.01;
  if (capFps) fps = opt.maxFrameRate;
  uint32_t fpsNum = uint32_t(std::lround(fps * 1000)), fpsDen = 1000;
  if (std::fabs(fps - 30000.0 / 1001) < 0.01) fpsNum = 30000, fpsDen = 1001;
  if (std::fabs(fps - 24000.0 / 1001) < 0.01) fpsNum = 24000, fpsDen = 1001;
  if (std::fabs(fps - 60000.0 / 1001) < 0.01) fpsNum = 60000, fpsDen = 1001;

  int kbps = opt.bitrateKbps;
  if (kbps <= 0) {
    double bpp = opt.quality == VideoOptions::Quality::Low ? 0.05 : opt.quality == VideoOptions::Quality::High ? 0.16 : 0.10;
    if (outCodec == "hvc1") bpp *= 0.6;
    kbps = std::max(500, int(bpp * dstW * dstH * fps / 1000.0));
  }

  char err[512] = {0};
  jc_video_decoder* dec = b->decoder_create(inCodec.c_str(), config.data(), config.size(), srcW, srcH, err, sizeof err);
  if (!dec) return Result::error(Status::Failed, std::string("Video decoder: ") + err);
  jc_video_encoder_config ec{sizeof ec, outCodec.c_str(), dstW, dstH, fpsNum, fpsDen, uint32_t(kbps),
                             uint32_t(std::max(1.0, std::round(fps * 2)))};
  jc_video_encoder* enc = b->encoder_create(&ec, err, sizeof err);
  if (!enc) {
    b->decoder_destroy(dec);
    return Result::error(Status::Failed, std::string("Video encoder: ") + err);
  }
  struct Guard {
    const jc_video_backend* b;
    jc_video_decoder* d;
    jc_video_encoder* e;
    ~Guard() {
      b->decoder_destroy(d);
      b->encoder_destroy(e);
    }
  } guard{b, dec, enc};

  TempFile tmpVideo{tempBeside(out, ".jvvideo")};
  if (!ctx.io.canWrite(tmpVideo.path)) return Result::error(Status::AccessDenied, "Access denied: " + out.string());
  std::ofstream vfile(tmpVideo.path, std::ios::binary | std::ios::trunc);
  if (!vfile) return Result::error(Status::Failed, "Cannot create a temporary file next to the output");
  EncodeSink sink{&vfile};

  struct Pipeline {
    const jc_video_backend* b;
    jc_video_encoder* enc;
    EncodeSink* sink;
    uint32_t w, h;
    std::map<int64_t, Frame> pending;
    bool capFps;
    double minGapUs;
    int64_t nextAllowed = INT64_MIN;
    int64_t firstPts = INT64_MIN;
    int64_t lastKeptPts = 0;
    uint64_t framesIn = 0, framesOut = 0;
    char* err;
    size_t errSize;
    int32_t emit(Frame& f) {
      if (firstPts == INT64_MIN) firstPts = f.pts;
      if (capFps) {
        if (nextAllowed != INT64_MIN && f.pts + 1000 < nextAllowed) return JC_OK;
        const int64_t gap = int64_t(minGapUs);
        nextAllowed = (nextAllowed == INT64_MIN ? f.pts : nextAllowed) + gap;
        if (nextAllowed <= f.pts) nextAllowed = f.pts + gap;
      }
      lastKeptPts = f.pts;
      ++framesOut;
      jc_video_frame v = view(f);
      v.pts_us = f.pts - firstPts;
      return b->encoder_push(enc, &v, onPacket, sink, err, errSize);
    }
    int32_t drain(size_t keep) {
      while (pending.size() > keep) {
        auto it = pending.begin();
        Frame f = std::move(it->second);
        pending.erase(it);
        if (int32_t rc = emit(f); rc != JC_OK) return rc;
      }
      return JC_OK;
    }
  } pipe{b, enc, &sink, dstW, dstH, {}, capFps, 1e6 / fps, INT64_MIN, INT64_MIN, 0, 0, 0, err, sizeof err};

  auto onFrame = [](void* user, const jc_video_frame* f) -> int32_t {
    auto* p = static_cast<Pipeline*>(user);
    Frame fr;
    fr.pts = f->pts_us;
    scaleNv12(*f, p->w, p->h, fr);
    ++p->framesIn;
    p->pending[fr.pts] = std::move(fr);
    return p->drain(6);
  };

  std::ifstream source(in, std::ios::binary);
  if (!source) return Result::error(Status::Unreadable, "Cannot open " + in.string());
  std::vector<char> buf;
  const size_t n = video->samples.size();
  for (size_t i = 0; i < n; ++i) {
    if (ctx.cancelled()) return Result::error(Status::Cancelled, "Cancelled");
    const auto& s = video->samples[i];
    buf.resize(s.size);
    source.clear();
    source.seekg(std::streamoff(s.offset));
    if (s.size && !source.read(buf.data(), std::streamsize(s.size)))
      return Result::error(Status::Unreadable, "The video ends before a sample it references");
    const int64_t ptsUs = int64_t(std::llround((double(s.dts) + s.cto) * 1e6 / video->timescale));
    int32_t rc = b->decoder_push(dec, reinterpret_cast<const uint8_t*>(buf.data()), buf.size(), ptsUs, s.sync ? 1 : 0,
                                 onFrame, &pipe, err, sizeof err);
    if (rc != JC_OK) return Result::error(Status::Failed, std::string("Video: ") + err);
    ctx.progress(0.02 + 0.83 * double(i + 1) / double(n));
  }
  if (b->decoder_flush(dec, onFrame, &pipe, err, sizeof err) != JC_OK || pipe.drain(0) != JC_OK ||
      b->encoder_flush(enc, onPacket, &sink, err, sizeof err) != JC_OK)
    return Result::error(Status::Failed, std::string("Video: ") + err);
  vfile.close();
  if (!sink.ok || !vfile) return Result::error(Status::Failed, "Cannot write the temporary video");
  if (ctx.cancelled()) return Result::error(Status::Cancelled, "Cancelled");

  const uint32_t timescale = video->timescale >= 1000 ? video->timescale : 90000;
  const int64_t lastDur = int64_t(std::llround(1e6 / fps));
  Movie mv;
  mv.timescale = src.timescale ? src.timescale : 1000;
  std::memcpy(mv.matrix, src.matrix, sizeof mv.matrix);
  MovieTrack vt;
  std::string e2;
  if (!buildVideoTrack(b, enc, outCodec, dstW, dstH, sink.samples, lastDur, timescale, vt, e2))
    return Result::error(Status::Failed, "Video: " + e2);
  std::memcpy(vt.matrix, video->matrix, sizeof vt.matrix);
  vt.language = video->language;
  vt.source = 1;
  mv.tracks.push_back(vt);

  TempFile tmpAudio;
  if (audio && opt.audio == VideoOptions::Audio::Keep) {
    if (outFormat == "mp4" && !codecAllowedInMp4(audio->codec))
      return Result::error(Status::Unsupported, "'" + audio->codec + "' audio can't go in MP4: use audio re-encode (AAC) or drop");
    MovieTrack at = *audio;
    at.id = 2;
    at.source = 0;
    at.edts.clear();
    mv.tracks.push_back(at);
  } else if (audio && opt.audio == VideoOptions::Audio::Aac) {
    Pcm pcm;
    std::string aerr;
    if (!osAudioCanDecode("m4a", &aerr) || !osAudioDecode(in, "m4a", pcm, aerr))
      return Result::error(Status::Unsupported, "The audio can't be decoded for AAC re-encoding: " + aerr);
    tmpAudio.path = tempBeside(out, ".jvaudio.m4a");
    MovieTrack at;
    if (auto r = aacTrack(ctx, pcm, ctx.options->video.audioBitrateKbps, tmpAudio.path, at); !r) return r;
    at.id = 2;
    at.source = 2;
    mv.tracks.push_back(at);
  }
  ctx.progress(0.9);

  std::ifstream vin(tmpVideo.path, std::ios::binary);
  std::ifstream ain;
  if (!tmpAudio.path.empty()) ain.open(tmpAudio.path, std::ios::binary);
  PendingFile pending(ctx.io, out);
  {
    std::ofstream dst(pending.tempPath(), std::ios::binary | std::ios::trunc);
    if (!dst) return Result::error(Status::Failed, "Cannot create " + out.string());
    MuxOptions mo;
    mo.flavor = outFormat == "mov" ? ContainerFlavor::Mov : ContainerFlavor::Mp4;
    std::string merr;
    std::vector<std::istream*> sources{&source, &vin, tmpAudio.path.empty() ? nullptr : &ain};
    if (!writeMovie(mv, sources, dst, mo, merr, [&](double p) {
          ctx.progress(0.9 + 0.09 * p);
          return !ctx.cancelled();
        }))
      return Result::error(ctx.cancelled() ? Status::Cancelled : Status::Failed, merr);
  }
  if (auto r = pending.commit(); !r) return r;
  if (ctx.written) ctx.written->push_back(out);
  if (pipe.framesOut == 0) return Result::error(Status::Failed, "No frame was decoded");
  ctx.warn("Re-encoded with " + std::string(b->name) + ": " + std::to_string(dstW) + "x" + std::to_string(dstH) + ", " +
           std::to_string(kbps) + " kbit/s, " + std::to_string(pipe.framesOut) + " frames");
  return Result::ok();
}

Result writeSyntheticClip(const fs::path& out, int width, int height, int fps, int frames, bool withAudio,
                          const std::string& codec, int bitrateKbps, int noise) {
  std::string reason;
  const jc_video_backend* b = pickVideoBackend("", codec, &reason);
  if (!b) return Result::error(Status::Unsupported, reason);
  char err[512] = {0};
  jc_video_encoder_config ec{sizeof ec, codec.c_str(), uint32_t(width), uint32_t(height), uint32_t(fps), 1,
                             uint32_t(bitrateKbps), uint32_t(fps)};
  jc_video_encoder* enc = b->encoder_create(&ec, err, sizeof err);
  if (!enc) return Result::error(Status::Failed, err);
  struct G {
    const jc_video_backend* b;
    jc_video_encoder* e;
    ~G() { b->encoder_destroy(e); }
  } g{b, enc};

  TempFile tmp{tempBeside(out, ".jvvideo")};
  std::ofstream vfile(tmp.path, std::ios::binary | std::ios::trunc);
  EncodeSink sink{&vfile};
  Frame f;
  f.width = uint32_t(width);
  f.height = uint32_t(height);
  f.y.resize(size_t(width) * height);
  f.uv.resize(size_t(width) * height / 2);
  uint32_t rng = 0x12345678u;
  for (int i = 0; i < frames; ++i) {
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        const int base = ((x + i * 7) ^ (y + i * 3)) & 0xff;
        const int grain = noise > 0 ? int(rng % uint32_t(noise)) - noise / 2 : 0;
        f.y[size_t(y) * width + x] = uint8_t(std::clamp(16 + (base * 219 >> 8) + grain, 16, 235));
      }
    for (int y = 0; y < height / 2; ++y)
      for (int x = 0; x < width / 2; ++x) {
        f.uv[size_t(y) * width + x * 2] = uint8_t(128 + ((x + i) % 64) - 32);
        f.uv[size_t(y) * width + x * 2 + 1] = uint8_t(128 + ((y + i) % 64) - 32);
      }
    f.pts = int64_t(std::llround(i * 1e6 / fps));
    jc_video_frame v = view(f);
    if (b->encoder_push(enc, &v, onPacket, &sink, err, sizeof err) != JC_OK) return Result::error(Status::Failed, err);
  }
  if (b->encoder_flush(enc, onPacket, &sink, err, sizeof err) != JC_OK) return Result::error(Status::Failed, err);
  vfile.close();

  Movie mv;
  mv.timescale = 1000;
  MovieTrack vt;
  std::string e2;
  if (!buildVideoTrack(b, enc, codec, uint32_t(width), uint32_t(height), sink.samples, int64_t(1e6 / fps),
                       uint32_t(fps) * 1000, vt, e2))
    return Result::error(Status::Failed, e2);
  vt.source = 1;
  mv.tracks.push_back(vt);

  TempFile tmpAudio;
  if (withAudio) {
    Pcm pcm;
    pcm.sampleRate = 48000;
    pcm.channels = 2;
    const size_t n = size_t(48000.0 * frames / fps);
    pcm.samples.resize(n * 2);
    for (size_t k = 0; k < n; ++k)
      pcm.samples[k * 2] = pcm.samples[k * 2 + 1] = float(0.3 * std::sin(2 * 3.14159265358979 * 440.0 * double(k) / 48000));
    tmpAudio.path = tempBeside(out, ".jvaudio.m4a");
    Context ctx;
    ConvertOptions co;
    ctx.options = &co;
    MovieTrack at;
    if (auto r = aacTrack(ctx, pcm, 128, tmpAudio.path, at); !r) return r;
    at.id = 2;
    at.source = 2;
    mv.tracks.push_back(at);
  }
  std::ifstream vin(tmp.path, std::ios::binary), ain;
  if (withAudio) ain.open(tmpAudio.path, std::ios::binary);
  std::ofstream dst(out, std::ios::binary | std::ios::trunc);
  std::vector<std::istream*> sources{nullptr, &vin, withAudio ? &ain : nullptr};
  MuxOptions mo;
  std::string merr;
  if (!writeMovie(mv, sources, dst, mo, merr)) return Result::error(Status::Failed, merr);
  return Result::ok();
}

}  // namespace jowe::convert::detail
