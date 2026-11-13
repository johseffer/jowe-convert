#pragma once

#include "../core/common.hpp"

namespace jowe::convert::detail {

Result probeVideo(const Context& ctx, const fs::path& path, ProbeInfo& info);
Result convertVideo(const Context& ctx, const fs::path& in, const std::string& inFormat, const fs::path& out,
                    const std::string& outFormat, ProbeInfo& outInfo);

}  // namespace jowe::convert::detail
