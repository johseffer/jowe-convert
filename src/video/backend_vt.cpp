#include "backend.hpp"

#if defined(JOWE_CONVERT_OS_CODECS) && defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <cstdio>
#include <map>
#include <memory>
#include <mutex>

#include "nal.hpp"

namespace {

struct DecodedFrame {
  uint32_t width = 0, height = 0;
  int64_t pts = 0;
  std::vector<uint8_t> y, uv;
};

struct Packet {
  std::vector<uint8_t> data;
  int64_t pts = 0, dts = 0;
  bool key = false;
};

void setErr(char* err, size_t n, const std::string& msg, OSStatus st = noErr) {
  if (!err || !n) return;
  if (st != noErr) std::snprintf(err, n, "%s (OSStatus %d)", msg.c_str(), int(st));
  else std::snprintf(err, n, "%s", msg.c_str());
}

bool isHevc(const char* codec) { return codec && (std::string(codec) == "hvc1" || std::string(codec) == "hev1"); }
bool isAvc(const char* codec) { return codec && (std::string(codec) == "avc1" || std::string(codec) == "avc3"); }

int64_t toUs(CMTime t) {
  if (!CMTIME_IS_VALID(t) || t.timescale == 0) return 0;
  return int64_t(double(t.value) * 1000000.0 / t.timescale);
}

CFNumberRef num(int32_t v) { return CFNumberCreate(nullptr, kCFNumberSInt32Type, &v); }
CFNumberRef numd(double v) { return CFNumberCreate(nullptr, kCFNumberDoubleType, &v); }

CFDictionaryRef nv12Attributes(int32_t w, int32_t h) {
  CFMutableDictionaryRef d =
      CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFNumberRef fmt = num(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
  CFDictionarySetValue(d, kCVPixelBufferPixelFormatTypeKey, fmt);
  CFRelease(fmt);
  if (w > 0 && h > 0) {
    CFNumberRef cw = num(w), ch = num(h);
    CFDictionarySetValue(d, kCVPixelBufferWidthKey, cw);
    CFDictionarySetValue(d, kCVPixelBufferHeightKey, ch);
    CFRelease(cw);
    CFRelease(ch);
  }
  CFDictionaryRef empty =
      CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(d, kCVPixelBufferIOSurfacePropertiesKey, empty);
  CFRelease(empty);
  return d;
}

}  // namespace

struct jc_video_decoder {
  VTDecompressionSessionRef session = nullptr;
  CMVideoFormatDescriptionRef format = nullptr;
  std::mutex mutex;
  std::vector<DecodedFrame> pending;
  OSStatus status = noErr;
};

struct jc_video_encoder {
  VTCompressionSessionRef session = nullptr;
  std::mutex mutex;
  std::vector<Packet> packets;
  std::vector<uint8_t> config;
  bool hevc = false;
  uint32_t width = 0, height = 0;
  int64_t frameDurationUs = 0;
  OSStatus status = noErr;
};

namespace jowe::convert::detail {

namespace {

int32_t canDecode(const char* codec, char* reason, size_t n) {
  if (isAvc(codec) || isHevc(codec)) return 1;
  setErr(reason, n, std::string("no decoder for ") + (codec ? codec : "?"));
  return 0;
}

int32_t canEncode(const char* codec, char* reason, size_t n) {
  if (!isAvc(codec) && !isHevc(codec)) {
    setErr(reason, n, std::string("no encoder for ") + (codec ? codec : "?"));
    return 0;
  }
  static std::mutex m;
  static std::map<std::string, std::pair<bool, OSStatus>> cache;
  std::lock_guard<std::mutex> lock(m);
  auto it = cache.find(codec);
  if (it == cache.end()) {
    VTCompressionSessionRef s = nullptr;
    OSStatus st = VTCompressionSessionCreate(nullptr, 640, 480, isHevc(codec) ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264,
                                             nullptr, nullptr, nullptr, nullptr, nullptr, &s);
    if (s) {
      VTCompressionSessionInvalidate(s);
      CFRelease(s);
    }
    it = cache.emplace(codec, std::make_pair(st == noErr, st)).first;
  }
  if (it->second.first) return 1;
  setErr(reason, n, isHevc(codec) ? "This device has no HEVC encoder" : "VideoToolbox has no H.264 encoder here",
         it->second.second);
  return 0;
}

void decompressed(void* refcon, void*, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image, CMTime pts,
                  CMTime) {
  auto* d = static_cast<jc_video_decoder*>(refcon);
  std::lock_guard<std::mutex> lock(d->mutex);
  if (status != noErr || !image) {
    if (status != noErr) d->status = status;
    return;
  }
  CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
  DecodedFrame f;
  f.width = uint32_t(CVPixelBufferGetWidth(image)) & ~1u;
  f.height = uint32_t(CVPixelBufferGetHeight(image)) & ~1u;
  f.pts = toUs(pts);
  const uint8_t* y = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(image, 0));
  const uint8_t* uv = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(image, 1));
  const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(image, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
  f.y.resize(size_t(f.width) * f.height);
  f.uv.resize(size_t(f.width) * f.height / 2);
  for (uint32_t r = 0; r < f.height; ++r) std::memcpy(&f.y[size_t(r) * f.width], y + r * ys, f.width);
  for (uint32_t r = 0; r < f.height / 2; ++r) std::memcpy(&f.uv[size_t(r) * f.width], uv + r * uvs, f.width);
  CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
  d->pending.push_back(std::move(f));
}

int32_t emitPending(jc_video_decoder* d, jc_video_frame_fn fn, void* user, char* err, size_t n) {
  std::vector<DecodedFrame> frames;
  OSStatus st;
  {
    std::lock_guard<std::mutex> lock(d->mutex);
    frames.swap(d->pending);
    st = d->status;
  }
  if (st != noErr) {
    setErr(err, n, "VideoToolbox could not decode a frame", st);
    return JC_ERR_FAILED;
  }
  for (auto& f : frames) {
    jc_video_frame vf{};
    vf.struct_size = sizeof vf;
    vf.format = JC_PIX_NV12;
    vf.width = f.width;
    vf.height = f.height;
    vf.plane[0] = f.y.data();
    vf.plane[1] = f.uv.data();
    vf.stride[0] = vf.stride[1] = f.width;
    vf.pts_us = f.pts;
    if (int32_t rc = fn(user, &vf); rc != JC_OK) return rc;
  }
  return JC_OK;
}

jc_video_decoder* decoderCreate(const char* codec, const uint8_t* config, size_t size, uint32_t, uint32_t, char* err,
                                size_t n) {
  ParameterSets ps;
  const bool hevc = isHevc(codec);
  if (!(hevc ? parseHvcC(config, size, ps) : parseAvcC(config, size, ps))) {
    setErr(err, n, "invalid decoder configuration record");
    return nullptr;
  }
  std::vector<const uint8_t*> ptrs;
  std::vector<size_t> sizes;
  for (auto* list : {&ps.vps, &ps.sps, &ps.pps})
    for (auto& x : *list) {
      ptrs.push_back(x.data());
      sizes.push_back(x.size());
    }
  auto d = std::make_unique<jc_video_decoder>();
  OSStatus st = hevc ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(nullptr, ptrs.size(), ptrs.data(), sizes.data(),
                                                                           ps.lengthSize, nullptr, &d->format)
                     : CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, ptrs.size(), ptrs.data(), sizes.data(),
                                                                           ps.lengthSize, &d->format);
  if (st != noErr) {
    setErr(err, n, "VideoToolbox rejected the parameter sets", st);
    return nullptr;
  }
  CFDictionaryRef attrs = nv12Attributes(0, 0);
  VTDecompressionOutputCallbackRecord cb{decompressed, d.get()};
  st = VTDecompressionSessionCreate(nullptr, d->format, nullptr, attrs, &cb, &d->session);
  CFRelease(attrs);
  if (st != noErr) {
    CFRelease(d->format);
    setErr(err, n, "VideoToolbox could not open a decoder", st);
    return nullptr;
  }
  return d.release();
}

int32_t decoderPush(jc_video_decoder* d, const uint8_t* data, size_t size, int64_t pts, int32_t, jc_video_frame_fn fn,
                    void* user, char* err, size_t n) {
  CMBlockBufferRef bb = nullptr;
  OSStatus st = CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, size, kCFAllocatorDefault, nullptr, 0, size, 0, &bb);
  if (st == noErr) st = CMBlockBufferReplaceDataBytes(data, bb, 0, size);
  CMSampleBufferRef sb = nullptr;
  CMSampleTimingInfo timing{kCMTimeInvalid, CMTimeMake(pts, 1000000), kCMTimeInvalid};
  if (st == noErr) st = CMSampleBufferCreateReady(nullptr, bb, d->format, 1, 1, &timing, 1, &size, &sb);
  if (st == noErr) {
    VTDecodeInfoFlags flags = 0;
    st = VTDecompressionSessionDecodeFrame(d->session, sb, 0, nullptr, &flags);
  }
  if (sb) CFRelease(sb);
  if (bb) CFRelease(bb);
  if (st != noErr) {
    setErr(err, n, "VideoToolbox decode failed", st);
    return JC_ERR_FAILED;
  }
  return emitPending(d, fn, user, err, n);
}

int32_t decoderFlush(jc_video_decoder* d, jc_video_frame_fn fn, void* user, char* err, size_t n) {
  VTDecompressionSessionFinishDelayedFrames(d->session);
  VTDecompressionSessionWaitForAsynchronousFrames(d->session);
  return emitPending(d, fn, user, err, n);
}

void decoderDestroy(jc_video_decoder* d) {
  if (d->session) {
    VTDecompressionSessionInvalidate(d->session);
    CFRelease(d->session);
  }
  if (d->format) CFRelease(d->format);
  delete d;
}

void compressed(void* refcon, void*, OSStatus status, VTEncodeInfoFlags, CMSampleBufferRef sb) {
  auto* e = static_cast<jc_video_encoder*>(refcon);
  std::lock_guard<std::mutex> lock(e->mutex);
  if (status != noErr) {
    e->status = status;
    return;
  }
  if (!sb) return;
  if (e->config.empty()) {
    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sb);
    auto atoms = static_cast<CFDictionaryRef>(
        CMFormatDescriptionGetExtension(fmt, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms));
    if (atoms) {
      auto cfg = static_cast<CFDataRef>(CFDictionaryGetValue(atoms, e->hevc ? CFSTR("hvcC") : CFSTR("avcC")));
      if (cfg) e->config.assign(CFDataGetBytePtr(cfg), CFDataGetBytePtr(cfg) + CFDataGetLength(cfg));
    }
  }
  Packet p;
  p.key = true;
  CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
  if (att && CFArrayGetCount(att) > 0) {
    auto dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(att, 0));
    CFBooleanRef notSync = nullptr;
    if (CFDictionaryGetValueIfPresent(dict, kCMSampleAttachmentKey_NotSync, reinterpret_cast<const void**>(&notSync)))
      p.key = !CFBooleanGetValue(notSync);
  }
  CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
  size_t len = CMBlockBufferGetDataLength(bb);
  p.data.resize(len);
  CMBlockBufferCopyDataBytes(bb, 0, len, p.data.data());
  p.pts = toUs(CMSampleBufferGetPresentationTimeStamp(sb));
  CMTime dts = CMSampleBufferGetDecodeTimeStamp(sb);
  p.dts = CMTIME_IS_VALID(dts) ? toUs(dts) : p.pts;
  e->packets.push_back(std::move(p));
}

int32_t takePackets(jc_video_encoder* e, jc_video_packet_fn fn, void* user, char* err, size_t n) {
  std::vector<Packet> packets;
  OSStatus st;
  {
    std::lock_guard<std::mutex> lock(e->mutex);
    packets.swap(e->packets);
    st = e->status;
  }
  if (st != noErr) {
    setErr(err, n, "VideoToolbox could not encode a frame", st);
    return JC_ERR_FAILED;
  }
  for (auto& p : packets)
    if (int32_t rc = fn(user, p.data.data(), p.data.size(), p.pts, p.dts, p.key ? 1 : 0); rc != JC_OK) return rc;
  return JC_OK;
}

jc_video_encoder* encoderCreate(const jc_video_encoder_config* c, char* err, size_t n) {
  if (!c || (!isAvc(c->codec) && !isHevc(c->codec)) || !c->width || !c->height || !c->fps_num || !c->fps_den) {
    setErr(err, n, "invalid encoder configuration");
    return nullptr;
  }
  auto e = std::make_unique<jc_video_encoder>();
  e->hevc = isHevc(c->codec);
  e->width = c->width;
  e->height = c->height;
  e->frameDurationUs = int64_t(1000000.0 * c->fps_den / c->fps_num);
  CFDictionaryRef attrs = nv12Attributes(int32_t(c->width), int32_t(c->height));
  OSStatus st = VTCompressionSessionCreate(nullptr, int32_t(c->width), int32_t(c->height),
                                           e->hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264, nullptr, attrs,
                                           nullptr, compressed, e.get(), &e->session);
  CFRelease(attrs);
  if (st != noErr) {
    setErr(err, n, "VideoToolbox could not open an encoder", st);
    return nullptr;
  }
  VTSessionSetProperty(e->session, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
  VTSessionSetProperty(e->session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
  VTSessionSetProperty(e->session, kVTCompressionPropertyKey_ProfileLevel,
                       e->hevc ? kVTProfileLevel_HEVC_Main_AutoLevel : kVTProfileLevel_H264_High_AutoLevel);
  CFNumberRef br = num(int32_t(c->bitrate_kbps * 1000));
  VTSessionSetProperty(e->session, kVTCompressionPropertyKey_AverageBitRate, br);
  CFRelease(br);
  // AverageBitRate alone is a soft target (short clips overshoot by 15-30%); cap each second as well.
  CFNumberRef bytes = num(int32_t(c->bitrate_kbps * 1000 / 8)), window = numd(1.0);
  const void* limits[2] = {bytes, window};
  CFArrayRef limitArray = CFArrayCreate(nullptr, limits, 2, &kCFTypeArrayCallBacks);
  VTSessionSetProperty(e->session, kVTCompressionPropertyKey_DataRateLimits, limitArray);
  CFRelease(limitArray);
  CFRelease(bytes);
  CFRelease(window);
  CFNumberRef fps = numd(double(c->fps_num) / c->fps_den);
  VTSessionSetProperty(e->session, kVTCompressionPropertyKey_ExpectedFrameRate, fps);
  CFRelease(fps);
  if (c->keyframe_interval) {
    CFNumberRef ki = num(int32_t(c->keyframe_interval));
    VTSessionSetProperty(e->session, kVTCompressionPropertyKey_MaxKeyFrameInterval, ki);
    CFRelease(ki);
  }
  VTCompressionSessionPrepareToEncodeFrames(e->session);
  return e.release();
}

int32_t encoderPush(jc_video_encoder* e, const jc_video_frame* f, jc_video_packet_fn fn, void* user, char* err,
                    size_t n) {
  if (f->width != e->width || f->height != e->height || f->format != JC_PIX_NV12) {
    setErr(err, n, "frame does not match the encoder");
    return JC_ERR_INVALID;
  }
  CVPixelBufferRef pb = nullptr;
  CFDictionaryRef attrs = nv12Attributes(int32_t(e->width), int32_t(e->height));
  CVReturn cv = CVPixelBufferCreate(nullptr, e->width, e->height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                    attrs, &pb);
  CFRelease(attrs);
  if (cv != kCVReturnSuccess) {
    setErr(err, n, "could not allocate a pixel buffer");
    return JC_ERR_FAILED;
  }
  CVPixelBufferLockBaseAddress(pb, 0);
  auto* y = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 0));
  auto* uv = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 1));
  const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
  for (uint32_t r = 0; r < e->height; ++r) std::memcpy(y + r * ys, f->plane[0] + size_t(r) * f->stride[0], e->width);
  for (uint32_t r = 0; r < e->height / 2; ++r)
    std::memcpy(uv + r * uvs, f->plane[1] + size_t(r) * f->stride[1], e->width);
  CVPixelBufferUnlockBaseAddress(pb, 0);
  VTEncodeInfoFlags flags = 0;
  OSStatus st = VTCompressionSessionEncodeFrame(e->session, pb, CMTimeMake(f->pts_us, 1000000),
                                                CMTimeMake(e->frameDurationUs, 1000000), nullptr, nullptr, &flags);
  CFRelease(pb);
  if (st != noErr) {
    setErr(err, n, "VideoToolbox encode failed", st);
    return JC_ERR_FAILED;
  }
  return takePackets(e, fn, user, err, n);
}

int32_t encoderFlush(jc_video_encoder* e, jc_video_packet_fn fn, void* user, char* err, size_t n) {
  VTCompressionSessionCompleteFrames(e->session, kCMTimeInvalid);
  return takePackets(e, fn, user, err, n);
}

int32_t encoderConfig(jc_video_encoder* e, uint8_t* out, size_t cap, size_t* size) {
  std::lock_guard<std::mutex> lock(e->mutex);
  if (e->config.empty()) return JC_ERR_FAILED;
  if (size) *size = e->config.size();
  if (!out || cap < e->config.size()) return JC_ERR_INVALID;
  std::memcpy(out, e->config.data(), e->config.size());
  return JC_OK;
}

void encoderDestroy(jc_video_encoder* e) {
  if (e->session) {
    VTCompressionSessionInvalidate(e->session);
    CFRelease(e->session);
  }
  delete e;
}

const jc_video_backend kVt = {
    JC_VIDEO_BACKEND_ABI_VERSION, sizeof(jc_video_backend), "VideoToolbox",
    canDecode, canEncode, decoderCreate, decoderPush, decoderFlush, decoderDestroy,
    encoderCreate, encoderPush, encoderFlush, encoderConfig, encoderDestroy};

}  // namespace

const jc_video_backend* builtinVideoBackend() { return &kVt; }
std::string noBuiltinVideoBackendReason() { return {}; }

}  // namespace jowe::convert::detail

#endif
