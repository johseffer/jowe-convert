#include <jowe/convert_plugin.h>

#include "test_util.hpp"

#ifndef JC_TEST_PLUGIN
#define JC_TEST_PLUGIN ""
#endif

TEST(plugin_load_and_encode) {
  if (std::string(JC_TEST_PLUGIN).empty()) {
    std::fprintf(stderr, "  skipped: no shared-library plugins on this platform\n");
    return;
  }
  auto before = jc::findFormat("jctest");
  CHECK(!before.has_value());
  int added = 0;
  auto r = jc::loadPlugin(JC_TEST_PLUGIN, &added);
  REQUIRE(r);
  CHECK(added == 1);
  auto f = jc::findFormat("jctest");
  REQUIRE(f.has_value());
  CHECK(f->canWrite);
  CHECK(f->writeBackend.find("plugin: jowe-convert test encoder") == 0);
  CHECK(f->kind == jc::MediaKind::Audio);

  auto plugins = jc::loadedPlugins();
  REQUIRE(!plugins.empty());
  CHECK(plugins.back().license == "Apache-2.0");

  fs::path dir = tempDir() / "plugin";
  fs::create_directories(dir);
  makeWav(dir / "tone.wav", 96000, 2, 0.25);
  auto rep = jc::convert(dir / "tone.wav", dir / "tone.jctest");
  CHECK_OK(rep);
  auto bytes = readAll(dir / "tone.jctest");
  REQUIRE(bytes.size() > 16);
  CHECK(std::memcmp(bytes.data(), "JCTEST01", 8) == 0);
  uint32_t rate = 0, ch = 0;
  std::memcpy(&rate, bytes.data() + 8, 4);
  std::memcpy(&ch, bytes.data() + 12, 4);
  CHECK(rate == 48000);
  CHECK(ch == 2);
  CHECK(bytes.size() == 16 + size_t(48000 / 4) * 2 * 4);
}

TEST(plugin_abi_rejected) {
  static const jc_encoder_plugin bad = {99, sizeof(jc_encoder_plugin), JC_KIND_AUDIO, "bad", "bad", "bad", "0", "MIT",
                                        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  auto r = jc::registerEncoderPlugin(&bad, "test");
  CHECK(r.status == jc::Status::Unsupported);
  CHECK(!jc::findFormat("bad").has_value());
  auto missing = jc::loadPlugin(tempDir() / "no-such-plugin.so");
  CHECK(!missing);
}
