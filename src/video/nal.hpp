#pragma once

#include "../core/common.hpp"

namespace jowe::convert::detail {

// H.264 / HEVC NAL helpers shared by the video backends.

std::vector<Bytes> splitAnnexB(const uint8_t* data, size_t size);
std::vector<Bytes> splitLengthPrefixed(const uint8_t* data, size_t size, int lengthSize = 4);
void appendLengthPrefixed(Bytes& out, const Bytes& nal);
void appendAnnexB(Bytes& out, const Bytes& nal);

int avcNalType(const Bytes& nal);   // nal_unit_type of an H.264 NAL
int hevcNalType(const Bytes& nal);  // nal_unit_type of an HEVC NAL

struct ParameterSets {
  std::vector<Bytes> vps;  // HEVC only
  std::vector<Bytes> sps;
  std::vector<Bytes> pps;
  int lengthSize = 4;
};

bool parseAvcC(const uint8_t* data, size_t size, ParameterSets& out);
bool parseHvcC(const uint8_t* data, size_t size, ParameterSets& out);
Bytes buildAvcC(const ParameterSets& ps);
Bytes buildHvcC(const ParameterSets& ps);

// "avc1" / "hvc1" sample entries for the muxer.
Bytes videoSampleEntry(const std::string& codec, const Bytes& config, int width, int height);
// The avcC / hvcC payload inside a sample entry, or empty.
Bytes sampleEntryConfig(const Bytes& entry);

}  // namespace jowe::convert::detail
