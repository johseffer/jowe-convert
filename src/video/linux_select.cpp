#include "linux_select.hpp"

namespace jowe::convert::detail {

namespace {

std::string codecs(const LinuxVideoProbe::Hardware& h) {
  std::string s;
  auto add = [&](bool on, const char* name) {
    if (on) s += (s.empty() ? "" : ", ") + std::string(name);
  };
  add(h.h264Encode, "H.264 encode");
  add(h.hevcEncode, "HEVC encode");
  add(h.h264Decode, "H.264 decode");
  add(h.hevcDecode, "HEVC decode");
  return s.empty() ? "no H.264 / HEVC entry points" : s;
}

std::string hardwareNote(const char* name, const LinuxVideoProbe::Hardware& h, bool chosen) {
  std::string line = std::string(name) + ": ";
  if (!h.present) return line + (h.detail.empty() ? "not found" : h.detail);
  line += h.detail + " (" + codecs(h) + ")";
  if (chosen) return line + " - in use";
  if (!h.h264Encode) return line + " - not used: no H.264 encoder";
  if (!h.implemented) return line + " - not used: this version doesn't drive it yet";
  return line + " - not used";
}

bool usable(const LinuxVideoProbe::Hardware& h) { return h.present && h.implemented && h.h264Encode; }

}  // namespace

LinuxVideoChoice chooseLinuxVideoBackend(const LinuxVideoProbe& p) {
  LinuxVideoChoice c;
  if (usable(p.vaapi)) c.backend = "vaapi";
  else if (usable(p.v4l2)) c.backend = "v4l2";
  else if (p.openh264) c.backend = "openh264";
  c.notes.push_back(hardwareNote("VA-API", p.vaapi, c.backend == "vaapi"));
  c.notes.push_back(hardwareNote("V4L2 M2M", p.v4l2, c.backend == "v4l2"));
  c.notes.push_back("OpenH264: " + (p.openh264Detail.empty() ? std::string(p.openh264 ? "loaded" : "not found")
                                                             : p.openh264Detail) +
                    (c.backend == "openh264" ? " - in use" : ""));
  if (c.backend.empty())
    c.reason = "No video encoder: install Cisco's OpenH264 2.6 (libopenh264.so.8) or use a VA-API / V4L2 device; "
               "MP4 / MOV can still be remuxed";
  return c;
}

}  // namespace jowe::convert::detail
