#include "test_util.hpp"
#include "video/backend.hpp"
#include "video/linux_select.hpp"

using namespace jowe::convert::detail;

namespace {

LinuxVideoProbe::Hardware device(bool h264Encode, bool implemented, const char* detail) {
  LinuxVideoProbe::Hardware h;
  h.present = true;
  h.h264Encode = h264Encode;
  h.h264Decode = true;
  h.implemented = implemented;
  h.detail = detail;
  return h;
}

bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

}  // namespace

TEST(video_linux_backend_selection) {
  LinuxVideoProbe none;
  none.vaapi.detail = "no /dev/dri render node";
  none.v4l2.detail = "no memory-to-memory video device";
  none.openh264Detail = "libopenh264.so.8 not found";
  auto c = chooseLinuxVideoBackend(none);
  CHECK(c.backend.empty());
  CHECK(contains(c.reason, "OpenH264"));
  REQUIRE(c.notes.size() == 3);
  CHECK(contains(c.notes[0], "VA-API: no /dev/dri"));
  CHECK(contains(c.notes[1], "V4L2 M2M: no memory-to-memory"));
  CHECK(contains(c.notes[2], "OpenH264: libopenh264.so.8 not found"));

  LinuxVideoProbe soft = none;
  soft.openh264 = true;
  soft.openh264Detail = "Cisco OpenH264 2.6.0";
  c = chooseLinuxVideoBackend(soft);
  CHECK(c.backend == "openh264" && c.reason.empty());
  CHECK(contains(c.notes[2], "in use"));

  // Hardware this build can't drive yet is reported but not chosen.
  LinuxVideoProbe gpu = soft;
  gpu.vaapi = device(true, false, "/dev/dri/renderD128, Intel iHD");
  c = chooseLinuxVideoBackend(gpu);
  CHECK(c.backend == "openh264");
  CHECK(contains(c.notes[0], "doesn't drive it yet"));

  // Order once hardware is usable: VA-API, then V4L2, then OpenH264.
  LinuxVideoProbe both = soft;
  both.vaapi = device(true, true, "/dev/dri/renderD128");
  both.v4l2 = device(true, true, "/dev/video11");
  CHECK(chooseLinuxVideoBackend(both).backend == "vaapi");
  both.vaapi = device(false, true, "/dev/dri/renderD128");
  c = chooseLinuxVideoBackend(both);
  CHECK(c.backend == "v4l2");
  CHECK(contains(c.notes[0], "no H.264 encoder"));
  both.v4l2.present = false;
  CHECK(chooseLinuxVideoBackend(both).backend == "openh264");
}

TEST(video_linux_backend_report) {
  auto notes = videoBackendNotes();
#if defined(__linux__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
  REQUIRE(notes.size() == 3);
  for (auto& n : notes) std::fprintf(stderr, "  %s\n", n.c_str());
  for (auto& n : notes)
    if (contains(n, "VA-API") && !contains(n, "in use"))
      std::fprintf(stderr, "  VA-API path: not exercised on this runner\n");
#else
  CHECK(notes.empty());
  std::fprintf(stderr, "  not Linux: no backend notes\n");
#endif
}
