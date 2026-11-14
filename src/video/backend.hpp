#pragma once

#include <jowe/convert_plugin.h>

#include "../core/common.hpp"

namespace jowe::convert::detail {

// Built-in OS backend: Media Foundation on Windows, VideoToolbox on Apple, none elsewhere.
const jc_video_backend* builtinVideoBackend();
// Why there is no built-in backend on this platform (empty when there is one).
std::string noBuiltinVideoBackendReason();
// One line per backend this platform considered (Linux lists VA-API, V4L2 and OpenH264), best first.
std::vector<std::string> videoBackendNotes();

std::vector<const jc_video_backend*> videoBackends();

// First backend able to decode decodeCodec (skipped when empty) and encode encodeCodec.
const jc_video_backend* pickVideoBackend(const std::string& decodeCodec, const std::string& encodeCodec,
                                         std::string* reason);

}  // namespace jowe::convert::detail
