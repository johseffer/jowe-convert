#include "audio.hpp"

#include <cmath>

#include "../core/plugins.hpp"
#include "../video/isobmff.hpp"

namespace jowe::convert::detail {

std::string sniffAudioFormat(const Bytes& b) {
  auto at = [&](size_t i, const char* s) {
    size_t n = std::strlen(s);
    return b.size() >= i + n && std::memcmp(b.data() + i, s, n) == 0;
  };
  if (at(0, "RIFF") && at(8, "WAVE")) return "wav";
  if (at(0, "FORM") && (at(8, "AIFF") || at(8, "AIFC"))) return "aiff";
  if (at(0, "caff")) return "caf";
  if (at(0, "fLaC")) return "flac";
  if (at(4, "ftyp")) return "m4a";
  if (at(0, "ID3")) {
    if (b.size() > 10) {
      size_t skip = 10 + (size_t(b[6] & 0x7f) << 21 | size_t(b[7] & 0x7f) << 14 | size_t(b[8] & 0x7f) << 7 | (b[9] & 0x7f));
      if (b.size() >= skip + 4 && std::memcmp(b.data() + skip, "fLaC", 4) == 0) return "flac";
    }
    return "mp3";
  }
  if (b.size() > 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) return "mp3";
  return {};
}

void remix(Pcm& pcm, int target) {
  if (target <= 0 || target == pcm.channels) return;
  const size_t frames = pcm.frames();
  const int src = pcm.channels;
  std::vector<float> out(frames * size_t(target));
  for (size_t f = 0; f < frames; ++f) {
    const float* in = &pcm.samples[f * src];
    float* o = &out[f * target];
    if (target == 1) {
      float sum = 0;
      for (int c = 0; c < src; ++c) sum += in[c];
      o[0] = sum / float(src);
    } else if (src == 1) {
      for (int c = 0; c < target; ++c) o[c] = in[0];
    } else {
      for (int c = 0; c < target; ++c) o[c] = c < src ? in[c] : 0.0f;
    }
  }
  pcm.samples.swap(out);
  pcm.channels = target;
}

void resample(Pcm& pcm, int targetRate) {
  if (targetRate <= 0 || targetRate == pcm.sampleRate || pcm.samples.empty()) return;
  const double ratio = double(targetRate) / pcm.sampleRate;
  const double cutoff = std::min(1.0, ratio) * 0.95;
  const int half = 24;
  const int phases = 1024;
  const double pi = 3.14159265358979323846;
  const double step = 1.0 / std::min(1.0, ratio);
  const int taps = int(std::ceil(half * step));
  std::vector<float> table(size_t(phases + 1) * (2 * taps));
  for (int ph = 0; ph <= phases; ++ph) {
    const double frac = double(ph) / phases;
    double sum = 0;
    for (int k = 0; k < 2 * taps; ++k) {
      const double x = double(k - taps + 1) - frac;
      const double sx = x * cutoff;
      double s = std::abs(sx) < 1e-9 ? 1.0 : std::sin(pi * sx) / (pi * sx);
      const double wpos = x / (taps + 1);
      double w = std::abs(wpos) >= 1 ? 0 : 0.42 + 0.5 * std::cos(pi * wpos) + 0.08 * std::cos(2 * pi * wpos);
      table[size_t(ph) * 2 * taps + k] = float(s * w);
      sum += s * w;
    }
    if (sum != 0)
      for (int k = 0; k < 2 * taps; ++k) table[size_t(ph) * 2 * taps + k] = float(table[size_t(ph) * 2 * taps + k] / sum);
  }
  const size_t inFrames = pcm.frames();
  const size_t outFrames = size_t(std::floor(double(inFrames) * ratio));
  const int ch = pcm.channels;
  std::vector<float> out(outFrames * ch);
  for (size_t i = 0; i < outFrames; ++i) {
    const double t = double(i) / ratio;
    const long base = long(std::floor(t));
    const int ph = int(std::lround((t - base) * phases));
    const float* w = &table[size_t(ph) * 2 * taps];
    for (int c = 0; c < ch; ++c) {
      double acc = 0;
      for (int k = 0; k < 2 * taps; ++k) {
        long idx = base + k - taps + 1;
        if (idx < 0 || idx >= long(inFrames)) continue;
        acc += double(w[k]) * pcm.samples[size_t(idx) * ch + c];
      }
      out[i * ch + c] = float(acc);
    }
  }
  pcm.samples.swap(out);
  pcm.sampleRate = targetRate;
}

static void fillProbe(const AudioHeader& h, const std::string& fmt, ProbeInfo& info) {
  info.format = fmt;
  info.codec = h.codec;
  info.sampleRate = h.sampleRate;
  info.channels = h.channels;
  info.bitDepth = h.bits;
  info.bitrateKbps = h.bitrateKbps;
  info.durationSeconds = h.sampleRate ? double(h.frames) / h.sampleRate : 0;
  if (!info.bitrateKbps && info.durationSeconds > 0 && info.fileSize)
    info.bitrateKbps = int(double(info.fileSize) * 8 / info.durationSeconds / 1000 + 0.5);
}

Result probeAudio(const Context*, const fs::path&, const Bytes& data, const std::string& format, ProbeInfo& info) {
  std::string fmt = sniffAudioFormat(data);
  if (fmt.empty()) fmt = format;
  AudioHeader h;
  bool ok = false;
  if (fmt == "wav") ok = wavInfo(data, h);
  else if (fmt == "aiff") ok = aiffInfo(data, h);
  else if (fmt == "caf") ok = cafInfo(data, h);
  else if (fmt == "flac") ok = flacInfo(data, h);
  else if (fmt == "mp3") ok = mp3Info(data, h);
  else if (fmt == "m4a") {
    Movie mv;
    std::string err;
    if (parseMovie(data, mv, err)) {
      for (auto& t : mv.tracks) {
        if (t.handler != "soun") continue;
        h.codec = t.codec;
        h.sampleRate = t.sampleRate;
        h.channels = t.channels;
        h.bits = 16;
        h.frames = t.timescale ? uint64_t(double(t.durationTicks()) / t.timescale * t.sampleRate) : 0;
        ok = true;
        break;
      }
      info.brand = mv.majorBrand;
    }
  }
  if (!ok) return Result::error(Status::Unreadable, "This audio file can't be read.");
  fillProbe(h, fmt, info);
  return Result::ok();
}

static Result decodeAudio(const Context& ctx, const fs::path& in, const Bytes& data, const std::string& fmt, Pcm& pcm) {
  std::string err;
  bool ok = false;
  if (fmt == "wav") ok = wavRead(data, pcm, err);
  else if (fmt == "aiff") ok = aiffRead(data, pcm, err);
  else if (fmt == "flac") ok = flacRead(data, pcm, err);
  else if (fmt == "mp3") ok = mp3Read(data, pcm, err);
  else if (fmt == "caf") {
    ok = cafRead(data, pcm, err);
    if (!ok && osAudioCanDecode("caf", nullptr)) ok = osAudioDecode(in, fmt, pcm, err);
  } else {
    if (!osAudioCanDecode(fmt, &err)) return Result::error(Status::Unsupported, err);
    if (!ctx.io.canRead(in)) return Result::error(Status::AccessDenied, "Access denied: " + in.string());
    ok = osAudioDecode(in, fmt, pcm, err);
  }
  if (!ok) return Result::error(Status::Unreadable, "This audio file can't be read: " + err);
  if (pcm.channels <= 0 || pcm.sampleRate <= 0) return Result::error(Status::Unreadable, "No audio stream");
  return Result::ok();
}

static int pickBits(const std::string& fmt, int requested, const Pcm& src) {
  int b = requested;
  if (!b) b = src.sourceFloat || src.sourceBits > 24 ? 32 : (src.sourceBits > 16 ? 24 : 16);
  if (b <= 16) b = 16;
  else if (b <= 24) b = 24;
  else b = 32;
  if (fmt == "flac" && b > 24) b = 24;
  return b;
}

Result convertAudio(const Context& ctx, const fs::path& in, const std::string& inFormat, const fs::path& out,
                    const std::string& outFormat, ProbeInfo& outInfo) {
  const AudioOptions& opt = ctx.options->audio;
  if (opt.channels < 0 || opt.channels > 8) return Result::error(Status::InvalidArgument, "channels must be 1..8");
  if (opt.sampleRate < 0 || (opt.sampleRate && (opt.sampleRate < 8000 || opt.sampleRate > 384000)))
    return Result::error(Status::InvalidArgument, "sample rate must be 8000..384000 Hz");

  const bool encoded = outFormat != "wav" && outFormat != "aiff" && outFormat != "caf" && outFormat != "flac";
  bool useOs = false;
  const jc_encoder_plugin* plugin = nullptr;
  if (encoded) {
    std::string reason;
    useOs = osAudioCanEncode(outFormat, &reason);
    if (!useOs) {
      for (auto& e : pluginEncodersFor(outFormat))
        if (pluginUsable(e.plugin, &reason)) { plugin = e.plugin; break; }
      if (!plugin) return Result::error(Status::NotWritable, reason);
    }
  }

  Bytes data;
  if (auto r = ctx.io.readFile(in, data); !r) return r;
  std::string fmt = sniffAudioFormat(data);
  if (fmt.empty()) fmt = inFormat;
  Pcm pcm;
  if (auto r = decodeAudio(ctx, in, data, fmt, pcm); !r) return r;
  data.clear();
  data.shrink_to_fit();
  if (ctx.cancelled()) return Result::error(Status::Cancelled, "Cancelled");
  ctx.progress(0.3);

  int rate = opt.sampleRate ? opt.sampleRate : pcm.sampleRate;
  int channels = opt.channels ? opt.channels : pcm.channels;
  int kbps = opt.bitrateKbps;
  if (useOs) osAudioNegotiate(outFormat, rate, channels, kbps);
  else if (plugin) pluginNegotiate(plugin, rate, channels, kbps);
  if (encoded && opt.sampleRate && rate != opt.sampleRate)
    ctx.warn("Sample rate held to " + std::to_string(rate) + " Hz for " + outFormat);
  if (encoded && opt.bitrateKbps && kbps != opt.bitrateKbps)
    ctx.warn("Bitrate set to " + std::to_string(kbps) + " kbit/s (the encoder's nearest)");

  const int srcBits = pcm.sourceBits;
  const bool srcFloat = pcm.sourceFloat;
  remix(pcm, channels);
  resample(pcm, rate);
  pcm.sourceBits = srcBits;
  pcm.sourceFloat = srcFloat;
  if (ctx.cancelled()) return Result::error(Status::Cancelled, "Cancelled");
  ctx.progress(0.5);

  PendingFile pending(ctx.io, out);
  if (encoded) {
    if (useOs) {
      if (!ctx.io.canWrite(pending.tempPath())) return Result::error(Status::AccessDenied, "Access denied: " + out.string());
      std::string err;
      if (!osAudioEncode(pcm, outFormat, kbps, pending.tempPath(), err)) return Result::error(Status::Failed, err);
    } else if (auto r = pluginEncode(ctx, plugin, pcm, kbps, pending.tempPath()); !r) {
      return r;
    }
  } else {
    const int bits = pickBits(outFormat, opt.bitDepth, pcm);
    Bytes encodedBytes;
    std::string err;
    bool ok = false;
    if (outFormat == "wav") ok = wavWrite(pcm, bits, encodedBytes);
    else if (outFormat == "aiff") ok = aiffWrite(pcm, bits, encodedBytes);
    else if (outFormat == "caf") ok = cafWrite(pcm, bits, encodedBytes);
    else if (outFormat == "flac") ok = flacWrite(pcm, bits, encodedBytes, err);
    else return Result::error(Status::NotWritable, "Audio can't be written as " + outFormat);
    if (!ok) return Result::error(Status::Failed, err.empty() ? "encoding failed" : err);
    if (auto r = ctx.io.writeFile(pending.tempPath(), encodedBytes); !r) return r;
  }
  if (ctx.cancelled()) return Result::error(Status::Cancelled, "Cancelled");
  if (auto r = pending.commit(); !r) return r;
  if (ctx.written) ctx.written->push_back(out);

  Bytes written;
  ctx.io.readFile(out, written);
  outInfo.kind = MediaKind::Audio;
  outInfo.fileSize = written.size();
  probeAudio(&ctx, out, written, outFormat, outInfo);
  ctx.progress(1.0);
  return Result::ok();
}

}  // namespace jowe::convert::detail
