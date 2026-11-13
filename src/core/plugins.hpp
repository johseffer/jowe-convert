#pragma once

#include <jowe/convert_plugin.h>

#include "../audio/audio.hpp"

namespace jowe::convert::detail {

struct PluginEncoder {
  const jc_encoder_plugin* plugin = nullptr;
  std::string source;
};

// Every registered encoder for this format, usable or not; usable ones first.
std::vector<PluginEncoder> pluginEncodersFor(const std::string& formatId);
std::vector<PluginEncoder> allPluginEncoders();
bool pluginUsable(const jc_encoder_plugin* p, std::string* reason);

void pluginNegotiate(const jc_encoder_plugin* p, int& sampleRate, int& channels, int& bitrateKbps);
Result pluginEncode(const Context& ctx, const jc_encoder_plugin* p, const Pcm& pcm, int bitrateKbps,
                    const fs::path& out);

}  // namespace jowe::convert::detail
