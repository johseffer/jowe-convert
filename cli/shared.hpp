#pragma once

#include <jowe/convert.hpp>

#include <string>

#include "json.hpp"

namespace jvc {

namespace jc = jowe::convert;

Json toJson(const jc::FormatCapability& f);
Json toJson(const jc::ProbeInfo& p);
Json toJson(const jc::ConvertReport& r);
Json formatsJson(const std::string& kindFilter);

// Fills options from named values; returns an error message or empty.
std::string applyOption(jc::ConvertOptions& o, const std::string& name, const std::string& value);

std::string pathString(const std::filesystem::path& p);

int runMcp(const std::vector<std::filesystem::path>& allow);

}  // namespace jvc
