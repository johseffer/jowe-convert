#pragma once

#include <functional>
#include <istream>
#include <ostream>

#include "../core/common.hpp"

namespace jowe::convert::detail {

struct MovieSample {
  uint64_t offset = 0;
  uint32_t size = 0;
  uint64_t dts = 0;
  uint32_t duration = 0;
  int32_t cto = 0;
  bool sync = true;
  uint32_t descIndex = 1;
};

struct MovieTrack {
  uint32_t id = 0;
  std::string handler;
  std::string handlerName;
  uint32_t timescale = 0;
  uint64_t mediaDuration = 0;
  uint16_t language = 0x55C4;
  uint16_t layer = 0;
  uint16_t alternateGroup = 0;
  uint16_t volume = 0;
  uint32_t tkhdFlags = 3;
  uint8_t matrix[36] = {};
  uint32_t widthFixed = 0;
  uint32_t heightFixed = 0;
  Bytes edts;
  std::vector<Bytes> sampleEntries;
  std::string codec;
  int width = 0;
  int height = 0;
  int sampleRate = 0;
  int channels = 0;
  bool hasSyncTable = false;
  bool hasCtts = false;
  std::vector<MovieSample> samples;

  uint64_t durationTicks() const;
};

struct Movie {
  std::string majorBrand;
  std::vector<std::string> compatibleBrands;
  uint32_t timescale = 1000;
  uint64_t duration = 0;
  uint8_t matrix[36] = {};
  std::vector<MovieTrack> tracks;
};

// Parses a whole file held in memory (used for small files and probing).
bool parseMovie(const Bytes& file, Movie& movie, std::string& err);
// Parses a file on disk, loading only ftyp and moov into memory.
bool parseMovieFile(const fs::path& path, Movie& movie, std::string& err);

enum class ContainerFlavor { Mp4, Mov };

struct MuxOptions {
  ContainerFlavor flavor = ContainerFlavor::Mp4;
};

// Writes movie into out, copying each sample's bytes from source (offsets in MovieSample).
bool writeMovie(const Movie& movie, std::istream& source, std::ostream& out, const MuxOptions& opt,
                std::string& err, const std::function<bool(double)>& progress = {});

bool codecAllowedInMp4(const std::string& codec);

}  // namespace jowe::convert::detail
