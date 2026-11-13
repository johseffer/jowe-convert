#include "../audio/audio.hpp"
#include "../image/image.hpp"
#include "../model/scene.hpp"
#include "../video/video.hpp"
#include "common.hpp"

#include <fstream>

namespace jowe::convert {

namespace fs = std::filesystem;
using namespace detail;

namespace {

std::optional<FormatCapability> detectFormat(const fs::path& path, const Io& io) {
  auto byExt = formatForPath(path);
  if (byExt && byExt->kind != MediaKind::Image && byExt->kind != MediaKind::Audio) return byExt;
  if (!io.canRead(path)) return byExt;
  std::ifstream f(path, std::ios::binary);
  Bytes head(64);
  f.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
  head.resize(size_t(f.gcount()));
  std::string sniffed = sniffImageFormat(head);
  if (sniffed.empty() && (!byExt || byExt->kind == MediaKind::Audio)) {
    sniffed = sniffAudioFormat(head);
    if (sniffed == "m4a" && byExt && byExt->kind != MediaKind::Audio) sniffed.clear();
  }
  if (!sniffed.empty()) {
    if (auto f2 = findFormat(sniffed)) {
      if (!byExt || byExt->kind == f2->kind) return f2;
    }
  }
  return byExt;
}

}  // namespace

ProbeResult probe(const fs::path& path, const AccessCheck& access) {
  ProbeResult out;
  Io io{&access};
  std::error_code ec;
  if (!io.canRead(path)) {
    out.result = Result::error(Status::AccessDenied, "Access denied: " + path.string());
    return out;
  }
  if (!fs::is_regular_file(path, ec)) {
    out.result = Result::error(Status::Unreadable, "No such file: " + path.string());
    return out;
  }
  auto fmt = detectFormat(path, io);
  if (!fmt) {
    out.result = Result::error(Status::Unsupported, "Unknown file type: " + path.filename().string());
    return out;
  }
  out.info.kind = fmt->kind;
  out.info.format = fmt->id;
  out.info.fileSize = fs::file_size(path, ec);
  ConvertOptions opts;
  opts.access = access;
  Context ctx;
  ctx.io = io;
  ctx.options = &opts;
  switch (fmt->kind) {
    case MediaKind::Image:
    case MediaKind::Audio: {
      Bytes data;
      if (auto r = io.readFile(path, data); !r) {
        out.result = r;
        return out;
      }
      out.result = fmt->kind == MediaKind::Image ? probeImage(data, fmt->id, out.info)
                                                 : probeAudio(&ctx, path, data, fmt->id, out.info);
      break;
    }
    case MediaKind::Video:
      out.info.format = fmt->id;
      out.result = probeVideo(ctx, path, out.info);
      if (out.result) out.info.format = fmt->id == "m4v" ? "m4v" : out.info.format;
      break;
    case MediaKind::Model:
      if (!fmt->canRead) {
        out.result = Result::error(Status::Unsupported, fmt->readReason);
        return out;
      }
      out.result = probeModel(ctx, path, fmt->id, out.info);
      break;
  }
  return out;
}

ConvertReport convert(const fs::path& input, const fs::path& output, const ConvertOptions& options) {
  ConvertReport rep;
  Io io{&options.access};
  Context ctx;
  ctx.io = io;
  ctx.options = &options;
  ctx.warnings = &rep.warnings;
  ctx.written = &rep.writtenFiles;
  ctx.input = &rep.input;
  auto fail = [&](Status s, std::string msg) {
    rep.result = Result::error(s, std::move(msg));
    return rep;
  };

  std::error_code ec;
  if (!io.canRead(input)) return fail(Status::AccessDenied, "Access denied: " + input.string());
  if (!io.canWrite(output)) return fail(Status::AccessDenied, "Access denied: " + output.string());
  if (!fs::is_regular_file(input, ec)) return fail(Status::Unreadable, "No such file: " + input.string());
  if (fs::exists(output, ec)) {
    if (fs::equivalent(input, output, ec)) return fail(Status::InvalidArgument, "Input and output are the same file");
    if (!options.overwrite) return fail(Status::Failed, "Output exists: " + output.string());
  }
  if (!output.has_filename()) return fail(Status::InvalidArgument, "Output needs a file name");
  if (!output.parent_path().empty() && !fs::is_directory(output.parent_path(), ec))
    return fail(Status::InvalidArgument, "Output folder does not exist: " + output.parent_path().string());

  auto in = detectFormat(input, io);
  if (!in) return fail(Status::Unsupported, "Unknown input type: " + input.filename().string());
  if (!in->canRead) return fail(Status::Unsupported, in->readReason.empty() ? in->name + " can't be read here" : in->readReason);
  auto out = options.targetFormat.empty() ? formatForPath(output) : findFormat(options.targetFormat);
  if (!out) return fail(Status::InvalidArgument, "Unknown output type: " + output.filename().string());
  if (out->kind != in->kind)
    return fail(Status::KindMismatch, std::string("Can't convert ") + toString(in->kind) + " to " + toString(out->kind));
  if (!out->canWrite) return fail(Status::NotWritable, out->writeReason.empty() ? out->name + " can't be written" : out->writeReason);

  rep.input.kind = in->kind;
  rep.input.format = in->id;
  rep.input.fileSize = fs::file_size(input, ec);
  if (in->kind != MediaKind::Model) {
    auto pr = probe(input, options.access);
    if (pr.result) rep.input = pr.info;
  }
  ctx.progress(0.0);
  switch (in->kind) {
    case MediaKind::Image:
      rep.result = convertImage(ctx, input, in->id, output, out->id, rep.output);
      break;
    case MediaKind::Audio:
      rep.result = convertAudio(ctx, input, in->id, output, out->id, rep.output);
      break;
    case MediaKind::Video:
      rep.result = convertVideo(ctx, input, in->id, output, out->id, rep.output);
      break;
    case MediaKind::Model:
      rep.result = convertModel(ctx, input, in->id, output, out->id, rep.output);
      break;
  }
  if (!rep.result) {
    for (auto& w : rep.writtenFiles) fs::remove(w, ec);
    rep.writtenFiles.clear();
  } else {
    rep.output.kind = out->kind;
    if (rep.output.format.empty()) rep.output.format = out->id;
  }
  return rep;
}

}  // namespace jowe::convert
