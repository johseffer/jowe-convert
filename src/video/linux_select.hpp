#pragma once

#include <string>
#include <vector>

namespace jowe::convert::detail {

// What the Linux probes found. Kept free of system types so the choice can be tested on every platform.
struct LinuxVideoProbe {
  struct Hardware {
    bool present = false;      // a device / driver answered
    bool h264Encode = false;
    bool hevcEncode = false;
    bool h264Decode = false;
    bool hevcDecode = false;
    bool implemented = false;  // this build can drive it
    std::string detail;        // device path, driver, or why nothing was found
  };
  Hardware vaapi;
  Hardware v4l2;
  bool openh264 = false;
  std::string openh264Detail;  // version and path, or why it wasn't loaded
};

struct LinuxVideoChoice {
  std::string backend;              // "vaapi", "v4l2", "openh264", or empty
  std::vector<std::string> notes;   // one line per candidate, best first
  std::string reason;               // why nothing was chosen
};

// Best first: VA-API hardware, then V4L2 memory-to-memory, then Cisco's OpenH264 (software).
LinuxVideoChoice chooseLinuxVideoBackend(const LinuxVideoProbe& probe);

}  // namespace jowe::convert::detail
