#include "shared.hpp"

#include <cmath>

namespace jvc {

std::string pathString(const std::filesystem::path& p) {
  auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

Json toJson(const jc::FormatCapability& f) {
  Json ext = Json::array();
  for (auto& e : f.extensions) ext.push(e);
  Json j = Json::object();
  j.set("id", f.id).set("name", f.name).set("kind", jc::toString(f.kind)).set("extensions", ext);
  j.set("read", f.canRead).set("write", f.canWrite);
  if (!f.readBackend.empty()) j.set("read_backend", f.readBackend);
  if (!f.writeBackend.empty()) j.set("write_backend", f.writeBackend);
  if (!f.canRead && !f.readReason.empty()) j.set("read_reason", f.readReason);
  if (!f.canWrite && !f.writeReason.empty()) j.set("write_reason", f.writeReason);
  if (f.kind == jc::MediaKind::Video) {
    Json t = Json::object();
    Json enc = Json::array();
    for (auto& e : f.videoEncoders) enc.push(e);
    t.set("available", f.canTranscode);
    if (!f.transcodeBackend.empty()) t.set("backend", f.transcodeBackend);
    if (!f.canTranscode) t.set("reason", f.transcodeReason);
    t.set("encoders", enc);
    j.set("transcode", t);
  }
  return j;
}

Json formatsJson(const std::string& kind) {
  Json arr = Json::array();
  for (auto& f : jc::listFormats())
    if (kind.empty() || kind == jc::toString(f.kind)) arr.push(toJson(f));
  return arr;
}

Json toJson(const jc::ProbeInfo& p) {
  Json j = Json::object();
  j.set("format", p.format).set("kind", jc::toString(p.kind)).set("file_size", uint64_t(p.fileSize));
  switch (p.kind) {
    case jc::MediaKind::Image:
      j.set("width", p.width).set("height", p.height).set("channels", p.channels).set("bit_depth", p.bitDepth);
      if (!p.codec.empty()) j.set("codec", p.codec);
      break;
    case jc::MediaKind::Audio:
      j.set("codec", p.codec).set("sample_rate", p.sampleRate).set("channels", p.channels);
      if (p.bitDepth) j.set("bit_depth", p.bitDepth);
      j.set("duration_seconds", std::round(p.durationSeconds * 1000) / 1000);
      if (p.bitrateKbps) j.set("bitrate_kbps", p.bitrateKbps);
      break;
    case jc::MediaKind::Video: {
      j.set("brand", p.brand).set("duration_seconds", std::round(p.durationSeconds * 1000) / 1000);
      if (p.width) j.set("width", p.width).set("height", p.height);
      Json tracks = Json::array();
      for (auto& t : p.tracks) {
        Json tj = Json::object();
        tj.set("type", t.type).set("codec", t.codec).set("duration_seconds", std::round(t.durationSeconds * 1000) / 1000);
        if (t.width) tj.set("width", t.width).set("height", t.height);
        if (t.sampleRate) tj.set("sample_rate", t.sampleRate).set("channels", t.channels);
        tj.set("samples", uint64_t(t.sampleCount));
        tracks.push(tj);
      }
      j.set("tracks", tracks);
      if (p.bitrateKbps) j.set("bitrate_kbps", p.bitrateKbps);
      break;
    }
    case jc::MediaKind::Model: {
      const auto& m = p.model;
      j.set("meshes", uint64_t(m.meshes)).set("vertices", uint64_t(m.vertices)).set("triangles", uint64_t(m.triangles));
      j.set("materials", uint64_t(m.materials)).set("textures", uint64_t(m.textures)).set("nodes", uint64_t(m.nodes));
      j.set("source_up_axis", m.sourceUpAxis);
      j.set("source_meters_per_unit", m.sourceMetersPerUnit > 0 ? Json(m.sourceMetersPerUnit) : Json());
      j.set("bounds_units", "m").set("bounds_up_axis", "Y");
      Json bmin = Json::array(), bmax = Json::array();
      for (int i = 0; i < 3; ++i) {
        bmin.push(double(m.boundsMin[i]));
        bmax.push(double(m.boundsMax[i]));
      }
      j.set("bounds_min", bmin).set("bounds_max", bmax);
      break;
    }
  }
  return j;
}

Json toJson(const jc::ConvertReport& r) {
  Json j = Json::object();
  j.set("ok", bool(r.result)).set("status", jc::toString(r.result.status));
  if (!r.result.message.empty()) j.set("message", r.result.message);
  if (r.result) {
    j.set("input", toJson(r.input));
    j.set("output", toJson(r.output));
    Json files = Json::array();
    for (auto& f : r.writtenFiles) files.push(pathString(f));
    j.set("files", files);
  }
  if (!r.warnings.empty()) {
    Json w = Json::array();
    for (auto& s : r.warnings) w.push(s);
    j.set("warnings", w);
  }
  return j;
}

std::string applyOption(jc::ConvertOptions& o, const std::string& name, const std::string& value) {
  auto toInt = [&](int& dst, int lo, int hi) -> std::string {
    try {
      size_t used = 0;
      int v = std::stoi(value, &used);
      if (used != value.size() || v < lo || v > hi)
        return name + " must be " + std::to_string(lo) + ".." + std::to_string(hi);
      dst = v;
      return {};
    } catch (...) {
      return name + " must be a number";
    }
  };
  if (name == "to" || name == "format") { o.targetFormat = value; return {}; }
  if (name == "quality") return toInt(o.image.quality, 1, 100);
  if (name == "max-size" || name == "max_size") return toInt(o.image.maxSize, 0, 65535);
  if (name == "sample-rate" || name == "sample_rate") return toInt(o.audio.sampleRate, 0, 384000);
  if (name == "channels") return toInt(o.audio.channels, 0, 8);
  if (name == "bit-depth" || name == "bit_depth") return toInt(o.audio.bitDepth, 0, 32);
  if (name == "bitrate" || name == "bitrate_kbps") return toInt(o.audio.bitrateKbps, 0, 512);
  if (name == "scale") {
    try {
      o.model.scale = std::stod(value);
      if (!(o.model.scale > 0)) return "scale must be > 0";
      return {};
    } catch (...) {
      return "scale must be a number";
    }
  }
  if (name == "video-codec" || name == "video_codec") {
    if (value == "keep") o.video.codec = jc::VideoOptions::Codec::Keep;
    else if (value == "h264") o.video.codec = jc::VideoOptions::Codec::H264;
    else if (value == "hevc" || value == "h265") o.video.codec = jc::VideoOptions::Codec::HEVC;
    else return "video-codec must be keep, h264 or hevc";
    return {};
  }
  if (name == "resolution") {
    if (value == "keep") { o.video.maxShortSide = 0; return {}; }
    int v = 0;
    try { v = std::stoi(value); } catch (...) {}
    if (v != 2160 && v != 1080 && v != 720 && v != 480) return "resolution must be keep, 2160, 1080, 720 or 480";
    o.video.maxShortSide = v;
    return {};
  }
  if (name == "video-bitrate" || name == "video_bitrate_kbps") return toInt(o.video.bitrateKbps, 0, 400000);
  if (name == "video-quality" || name == "video_quality") {
    if (value == "auto") o.video.quality = jc::VideoOptions::Quality::Auto;
    else if (value == "low") o.video.quality = jc::VideoOptions::Quality::Low;
    else if (value == "medium") o.video.quality = jc::VideoOptions::Quality::Medium;
    else if (value == "high") o.video.quality = jc::VideoOptions::Quality::High;
    else return "video-quality must be auto, low, medium or high";
    return {};
  }
  if (name == "max-fps" || name == "max_fps") {
    try {
      o.video.maxFrameRate = std::stod(value);
      if (o.video.maxFrameRate < 0 || o.video.maxFrameRate > 240) return "max-fps must be 0..240";
      return {};
    } catch (...) {
      return "max-fps must be a number";
    }
  }
  if (name == "audio") {
    if (value == "keep") o.video.audio = jc::VideoOptions::Audio::Keep;
    else if (value == "aac") o.video.audio = jc::VideoOptions::Audio::Aac;
    else if (value == "drop") o.video.audio = jc::VideoOptions::Audio::Drop;
    else return "audio must be keep, aac or drop";
    return {};
  }
  if (name == "audio-bitrate" || name == "audio_bitrate_kbps") return toInt(o.video.audioBitrateKbps, 0, 512);
  if (name == "up-axis" || name == "up_axis") {
    if (value == "keep") o.model.upAxis = jc::ModelOptions::UpAxis::Keep;
    else if (value == "z-to-y" || value == "z_to_y") o.model.upAxis = jc::ModelOptions::UpAxis::ZToY;
    else if (value == "y-to-z" || value == "y_to_z") o.model.upAxis = jc::ModelOptions::UpAxis::YToZ;
    else return "up-axis must be keep, z-to-y or y-to-z";
    return {};
  }
  return "unknown option --" + name;
}

}  // namespace jvc
