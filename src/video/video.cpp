#include "video.hpp"

#include <fstream>

#include "isobmff.hpp"

namespace jowe::convert::detail {

static void describe(const Movie& mv, ProbeInfo& info) {
  info.brand = mv.majorBrand;
  info.durationSeconds = mv.timescale ? double(mv.duration) / mv.timescale : 0;
  for (auto& t : mv.tracks) {
    TrackInfo ti;
    ti.type = t.handler == "vide" ? "video" : t.handler == "soun" ? "audio" : t.handler;
    ti.codec = t.codec;
    ti.durationSeconds = t.timescale ? double(t.durationTicks()) / t.timescale : 0;
    ti.width = t.width;
    ti.height = t.height;
    ti.sampleRate = t.sampleRate;
    ti.channels = t.channels;
    ti.sampleCount = t.samples.size();
    info.durationSeconds = std::max(info.durationSeconds, ti.durationSeconds);
    if (t.handler == "vide" && !info.width) {
      info.width = t.width;
      info.height = t.height;
      info.codec = t.codec;
    }
    if (t.handler == "soun" && !info.sampleRate) {
      info.sampleRate = t.sampleRate;
      info.channels = t.channels;
      if (info.codec.empty()) info.codec = t.codec;
    }
    info.tracks.push_back(ti);
  }
  if (info.durationSeconds > 0 && info.fileSize)
    info.bitrateKbps = int(double(info.fileSize) * 8 / info.durationSeconds / 1000 + 0.5);
}

Result probeVideo(const Context& ctx, const fs::path& path, ProbeInfo& info) {
  if (!ctx.io.canRead(path)) return Result::error(Status::AccessDenied, "Access denied: " + path.string());
  Movie mv;
  std::string err;
  if (!parseMovieFile(path, mv, err)) return Result::error(Status::Unreadable, "This video can't be read: " + err);
  info.format = mv.majorBrand == "qt  " ? "mov" : (info.format.empty() ? "mp4" : info.format);
  describe(mv, info);
  return Result::ok();
}

Result convertVideo(const Context& ctx, const fs::path& in, const std::string&, const fs::path& out,
                    const std::string& outFormat, ProbeInfo& outInfo) {
  if (!ctx.io.canRead(in)) return Result::error(Status::AccessDenied, "Access denied: " + in.string());
  Movie mv;
  std::string err;
  if (!parseMovieFile(in, mv, err)) return Result::error(Status::Unreadable, "This video can't be read: " + err);

  Movie kept = mv;
  kept.tracks.clear();
  for (auto& t : mv.tracks) {
    if (t.handler != "vide" && t.handler != "soun") {
      ctx.warn("Dropped a '" + t.handler + "' track (only video and audio are remuxed)");
      continue;
    }
    if (t.samples.empty()) continue;
    if (outFormat == "mp4" && !codecAllowedInMp4(t.codec))
      return Result::error(Status::Unsupported, "'" + t.codec +
                                                    "' can't be carried in MP4 and transcoding is not available in "
                                                    "this version; convert to MOV instead");
    kept.tracks.push_back(t);
  }
  if (kept.tracks.empty()) return Result::error(Status::Unreadable, "The file has no video or audio samples");

  std::ifstream src(in, std::ios::binary);
  if (!src) return Result::error(Status::Unreadable, "Cannot open " + in.string());
  PendingFile pending(ctx.io, out);
  if (!ctx.io.canWrite(pending.tempPath())) return Result::error(Status::AccessDenied, "Access denied: " + out.string());
  {
    std::ofstream dst(pending.tempPath(), std::ios::binary | std::ios::trunc);
    if (!dst) return Result::error(Status::Failed, "Cannot create " + out.string());
    MuxOptions opt;
    opt.flavor = outFormat == "mov" ? ContainerFlavor::Mov : ContainerFlavor::Mp4;
    bool ok = writeMovie(kept, src, dst, opt, err, [&](double p) {
      ctx.progress(0.05 + 0.9 * p);
      return !ctx.cancelled();
    });
    if (!ok) {
      dst.close();
      return Result::error(ctx.cancelled() ? Status::Cancelled : Status::Failed, err);
    }
  }
  if (auto r = pending.commit(); !r) return r;
  if (ctx.written) ctx.written->push_back(out);
  std::error_code ec;
  outInfo.kind = MediaKind::Video;
  outInfo.format = outFormat;
  outInfo.fileSize = fs::file_size(out, ec);
  probeVideo(ctx, out, outInfo);
  outInfo.format = outFormat;
  ctx.progress(1.0);
  return Result::ok();
}

}  // namespace jowe::convert::detail
