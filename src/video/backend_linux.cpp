#include "backend.hpp"

#if defined(JOWE_CONVERT_OS_CODECS) && defined(__linux__) && !defined(__ANDROID__)

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>

#include "linux_select.hpp"
#include "nal.hpp"

#if defined(JOWE_CONVERT_HAS_OPENH264)
#include <wels/codec_api.h>
#include <wels/codec_ver.h>
#endif

namespace jowe::convert::detail {

namespace {

void setErr(char* err, size_t n, const std::string& msg) {
  if (err && n) std::snprintf(err, n, "%s", msg.c_str());
}

// ---- VA-API probe (libva is loaded at run time; no build dependency) ----

LinuxVideoProbe::Hardware probeVaapi() {
  LinuxVideoProbe::Hardware h;
  void* va = dlopen("libva.so.2", RTLD_NOW | RTLD_LOCAL);
  void* drm = dlopen("libva-drm.so.2", RTLD_NOW | RTLD_LOCAL);
  if (!va || !drm) {
    h.detail = "libva / libva-drm not installed";
    if (va) dlclose(va);
    if (drm) dlclose(drm);
    return h;
  }
  using GetDisplayDrm = void* (*)(int);
  using Initialize = int (*)(void*, int*, int*);
  using Terminate = int (*)(void*);
  using MaxEntrypoints = int (*)(void*);
  using QueryEntrypoints = int (*)(void*, int, int*, int*);
  using Vendor = const char* (*)(void*);
  auto getDisplay = reinterpret_cast<GetDisplayDrm>(dlsym(drm, "vaGetDisplayDRM"));
  auto init = reinterpret_cast<Initialize>(dlsym(va, "vaInitialize"));
  auto term = reinterpret_cast<Terminate>(dlsym(va, "vaTerminate"));
  auto maxEp = reinterpret_cast<MaxEntrypoints>(dlsym(va, "vaMaxNumEntrypoints"));
  auto queryEp = reinterpret_cast<QueryEntrypoints>(dlsym(va, "vaQueryConfigEntrypoints"));
  auto vendor = reinterpret_cast<Vendor>(dlsym(va, "vaQueryVendorString"));
  bool sawNode = false;
  if (getDisplay && init && term && maxEp && queryEp) {
    for (int i = 128; i < 136 && !h.present; ++i) {
      const std::string path = "/dev/dri/renderD" + std::to_string(i);
      int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
      if (fd < 0) continue;
      sawNode = true;
      void* dpy = getDisplay(fd);
      int major = 0, minor = 0;
      if (dpy && init(dpy, &major, &minor) == 0) {
        h.present = true;
        h.detail = path + (vendor ? std::string(", ") + vendor(dpy) : "");
        // VAProfile / VAEntrypoint values from va.h (stable ABI).
        const int kH264Main = 6, kH264High = 7, kH264CBase = 13, kHevcMain = 17;
        const int kVld = 1, kEncSlice = 6, kEncSliceLp = 8;
        std::vector<int> eps(size_t(std::max(1, maxEp(dpy))));
        auto has = [&](int profile, int ep) {
          int n = 0;
          if (queryEp(dpy, profile, eps.data(), &n) != 0) return false;
          for (int k = 0; k < n; ++k)
            if (eps[size_t(k)] == ep) return true;
          return false;
        };
        for (int p : {kH264Main, kH264High, kH264CBase}) {
          h.h264Encode = h.h264Encode || has(p, kEncSlice) || has(p, kEncSliceLp);
          h.h264Decode = h.h264Decode || has(p, kVld);
        }
        h.hevcEncode = has(kHevcMain, kEncSlice) || has(kHevcMain, kEncSliceLp);
        h.hevcDecode = has(kHevcMain, kVld);
        term(dpy);
      }
      close(fd);
    }
  }
  if (!h.present) h.detail = sawNode ? "render node found but no VA-API driver answered" : "no /dev/dri render node";
  dlclose(drm);
  dlclose(va);
  return h;
}

// ---- V4L2 memory-to-memory probe ----

LinuxVideoProbe::Hardware probeV4l2() {
  LinuxVideoProbe::Hardware h;
  bool sawM2m = false;
  for (int i = 0; i < 64 && !h.present; ++i) {
    const std::string path = "/dev/video" + std::to_string(i);
    int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    v4l2_capability cap{};
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
      const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
      if (caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)) {
        sawM2m = true;
        const bool mplane = caps & V4L2_CAP_VIDEO_M2M_MPLANE;
        auto formats = [&](uint32_t type) {
          std::vector<uint32_t> out;
          for (uint32_t k = 0; k < 64; ++k) {
            v4l2_fmtdesc d{};
            d.index = k;
            d.type = type;
            if (ioctl(fd, VIDIOC_ENUM_FMT, &d) != 0) break;
            out.push_back(d.pixelformat);
          }
          return out;
        };
        auto contains = [](const std::vector<uint32_t>& v, uint32_t f) {
          for (auto x : v)
            if (x == f) return true;
          return false;
        };
        const auto capture = formats(mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE);
        const auto output = formats(mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT);
        h.h264Encode = contains(capture, V4L2_PIX_FMT_H264);
        h.hevcEncode = contains(capture, V4L2_PIX_FMT_HEVC);
        h.h264Decode = contains(output, V4L2_PIX_FMT_H264);
        h.hevcDecode = contains(output, V4L2_PIX_FMT_HEVC);
        if (h.h264Encode || h.hevcEncode || h.h264Decode || h.hevcDecode) {
          h.present = true;
          h.detail = path + ", " + reinterpret_cast<const char*>(cap.card);
        }
      }
    }
    close(fd);
  }
  if (!h.present) h.detail = sawM2m ? "memory-to-memory device without H.264 / HEVC" : "no memory-to-memory video device";
  return h;
}

#if defined(JOWE_CONVERT_HAS_OPENH264)

// ---- Cisco OpenH264, loaded at run time so Cisco's binary (and its patent licence) is what users run ----

struct OpenH264 {
  bool ok = false;
  std::string detail;
  int (*createEncoder)(ISVCEncoder**) = nullptr;
  void (*destroyEncoder)(ISVCEncoder*) = nullptr;
  long (*createDecoder)(ISVCDecoder**) = nullptr;
  void (*destroyDecoder)(ISVCDecoder*) = nullptr;
};

const OpenH264& openh264() {
  static OpenH264 lib;
  static std::once_flag once;
  std::call_once(once, [] {
    std::vector<std::string> candidates;
    if (const char* env = std::getenv("JOWE_CONVERT_OPENH264")) candidates.push_back(env);
    candidates.push_back("libopenh264.so.8");
    candidates.push_back("libopenh264.so");
    std::string why = "libopenh264.so.8 not found (set JOWE_CONVERT_OPENH264 to its path)";
    for (auto& c : candidates) {
      void* h = dlopen(c.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (!h) continue;
      using Version = OpenH264Version (*)();
      auto version = reinterpret_cast<Version>(dlsym(h, "WelsGetCodecVersion"));
      lib.createEncoder = reinterpret_cast<int (*)(ISVCEncoder**)>(dlsym(h, "WelsCreateSVCEncoder"));
      lib.destroyEncoder = reinterpret_cast<void (*)(ISVCEncoder*)>(dlsym(h, "WelsDestroySVCEncoder"));
      lib.createDecoder = reinterpret_cast<long (*)(ISVCDecoder**)>(dlsym(h, "WelsCreateDecoder"));
      lib.destroyDecoder = reinterpret_cast<void (*)(ISVCDecoder*)>(dlsym(h, "WelsDestroyDecoder"));
      if (!version || !lib.createEncoder || !lib.destroyEncoder || !lib.createDecoder || !lib.destroyDecoder) {
        why = c + " is not OpenH264";
        dlclose(h);
        continue;
      }
      const OpenH264Version v = version();
      const std::string vs = std::to_string(v.uMajor) + "." + std::to_string(v.uMinor) + "." + std::to_string(v.uRevision);
      // The C++ interfaces and structs are only stable within a minor version.
      if (v.uMajor != OPENH264_MAJOR || v.uMinor != OPENH264_MINOR) {
        why = c + " is OpenH264 " + vs + "; this build needs " + std::to_string(OPENH264_MAJOR) + "." +
              std::to_string(OPENH264_MINOR) + ".x";
        dlclose(h);
        continue;
      }
      lib.ok = true;
      lib.detail = "Cisco OpenH264 " + vs + " (" + c + ")";
      return;
    }
    lib.detail = why;
  });
  return lib;
}

constexpr long long kTimeBias = 1000000000000LL;

}  // namespace

}  // namespace jowe::convert::detail

struct jc_video_decoder {
  ISVCDecoder* dec = nullptr;
  jowe::convert::detail::ParameterSets ps;
  std::vector<uint8_t> y, uv;
};

struct jc_video_encoder {
  ISVCEncoder* enc = nullptr;
  jowe::convert::detail::ParameterSets ps;
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> i420;
};

namespace jowe::convert::detail {

namespace {

bool isAvc(const char* c) { return c && (std::string(c) == "avc1" || std::string(c) == "avc3"); }

int32_t ohCanDecode(const char* codec, char* reason, size_t n) {
  if (!openh264().ok) {
    setErr(reason, n, openh264().detail);
    return 0;
  }
  if (isAvc(codec)) return 1;
  setErr(reason, n, "HEVC sources need a hardware decoder (VA-API / V4L2); OpenH264 decodes H.264 only");
  return 0;
}

int32_t ohCanEncode(const char* codec, char* reason, size_t n) {
  if (!openh264().ok) {
    setErr(reason, n, openh264().detail);
    return 0;
  }
  if (isAvc(codec)) return 1;
  setErr(reason, n, "OpenH264 encodes H.264 only; HEVC needs a VA-API / V4L2 encoder");
  return 0;
}

int32_t emitDecoded(jc_video_decoder* d, unsigned char* const* dst, const SBufferInfo& info, jc_video_frame_fn fn,
                    void* user) {
  if (info.iBufferStatus != 1 || !dst[0]) return JC_OK;
  const auto& sb = info.UsrData.sSystemBuffer;
  const uint32_t w = uint32_t(sb.iWidth) & ~1u, h = uint32_t(sb.iHeight) & ~1u;
  d->y.resize(size_t(w) * h);
  d->uv.resize(size_t(w) * h / 2);
  for (uint32_t r = 0; r < h; ++r) std::memcpy(&d->y[size_t(r) * w], dst[0] + size_t(r) * size_t(sb.iStride[0]), w);
  for (uint32_t r = 0; r < h / 2; ++r) {
    const uint8_t* u = dst[1] + size_t(r) * size_t(sb.iStride[1]);
    const uint8_t* v = dst[2] + size_t(r) * size_t(sb.iStride[1]);
    uint8_t* o = &d->uv[size_t(r) * w];
    for (uint32_t c = 0; c < w / 2; ++c) {
      o[c * 2] = u[c];
      o[c * 2 + 1] = v[c];
    }
  }
  jc_video_frame f{};
  f.struct_size = sizeof f;
  f.format = JC_PIX_NV12;
  f.width = w;
  f.height = h;
  f.plane[0] = d->y.data();
  f.plane[1] = d->uv.data();
  f.stride[0] = f.stride[1] = w;
  f.pts_us = int64_t(info.uiOutYuvTimeStamp) - kTimeBias;
  return fn(user, &f);
}

jc_video_decoder* ohDecoderCreate(const char* codec, const uint8_t* config, size_t size, uint32_t, uint32_t,
                                  char* err, size_t n) {
  if (!ohCanDecode(codec, err, n)) return nullptr;
  auto d = std::make_unique<jc_video_decoder>();
  if (!parseAvcC(config, size, d->ps)) {
    setErr(err, n, "invalid H.264 configuration record");
    return nullptr;
  }
  if (openh264().createDecoder(&d->dec) != 0 || !d->dec) {
    setErr(err, n, "OpenH264 could not create a decoder");
    return nullptr;
  }
  SDecodingParam p{};
  p.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
  if (d->dec->Initialize(&p) != 0) {
    openh264().destroyDecoder(d->dec);
    setErr(err, n, "OpenH264 could not initialise the decoder");
    return nullptr;
  }
  return d.release();
}

int32_t ohDecoderPush(jc_video_decoder* d, const uint8_t* data, size_t size, int64_t pts, int32_t key,
                      jc_video_frame_fn fn, void* user, char* err, size_t n) {
  Bytes annexb;
  if (key)
    for (auto* list : {&d->ps.sps, &d->ps.pps})
      for (auto& x : *list) appendAnnexB(annexb, x);
  for (auto& nal : splitLengthPrefixed(data, size, d->ps.lengthSize)) appendAnnexB(annexb, nal);
  unsigned char* dst[3] = {nullptr, nullptr, nullptr};
  SBufferInfo info{};
  info.uiInBsTimeStamp = static_cast<unsigned long long>(pts + kTimeBias);
  const DECODING_STATE st = d->dec->DecodeFrameNoDelay(annexb.data(), int(annexb.size()), dst, &info);
  if (st & (dsInvalidArgument | dsInitialOptExpected | dsOutOfMemory)) {
    setErr(err, n, "OpenH264 decoding failed (state " + std::to_string(int(st)) + ")");
    return JC_ERR_FAILED;
  }
  return emitDecoded(d, dst, info, fn, user);
}

int32_t ohDecoderFlush(jc_video_decoder* d, jc_video_frame_fn fn, void* user, char*, size_t) {
  for (int i = 0; i < 64; ++i) {
    unsigned char* dst[3] = {nullptr, nullptr, nullptr};
    SBufferInfo info{};
    d->dec->FlushFrame(dst, &info);
    if (info.iBufferStatus != 1) break;
    if (int32_t rc = emitDecoded(d, dst, info, fn, user); rc != JC_OK) return rc;
  }
  return JC_OK;
}

void ohDecoderDestroy(jc_video_decoder* d) {
  if (d->dec) {
    d->dec->Uninitialize();
    openh264().destroyDecoder(d->dec);
  }
  delete d;
}

jc_video_encoder* ohEncoderCreate(const jc_video_encoder_config* c, char* err, size_t n) {
  if (!c || !ohCanEncode(c->codec, err, n)) return nullptr;
  if (!c->width || !c->height || !c->fps_num || !c->fps_den) {
    setErr(err, n, "invalid encoder configuration");
    return nullptr;
  }
  auto e = std::make_unique<jc_video_encoder>();
  e->width = c->width;
  e->height = c->height;
  if (openh264().createEncoder(&e->enc) != 0 || !e->enc) {
    setErr(err, n, "OpenH264 could not create an encoder");
    return nullptr;
  }
  SEncParamExt p;
  e->enc->GetDefaultParams(&p);
  const int bps = int(c->bitrate_kbps * 1000);
  const float fps = float(double(c->fps_num) / c->fps_den);
  p.iUsageType = CAMERA_VIDEO_REAL_TIME;
  p.iPicWidth = int(c->width);
  p.iPicHeight = int(c->height);
  p.iTargetBitrate = bps;
  p.iMaxBitrate = UNSPECIFIED_BIT_RATE;
  p.iRCMode = RC_BITRATE_MODE;
  p.fMaxFrameRate = fps;
  p.bEnableFrameSkip = false;
  p.uiIntraPeriod = c->keyframe_interval;
  p.iSpatialLayerNum = 1;
  p.iTemporalLayerNum = 1;
  p.iMultipleThreadIdc = 1;
  p.eSpsPpsIdStrategy = CONSTANT_ID;
  p.sSpatialLayers[0].iVideoWidth = int(c->width);
  p.sSpatialLayers[0].iVideoHeight = int(c->height);
  p.sSpatialLayers[0].fFrameRate = fps;
  p.sSpatialLayers[0].iSpatialBitrate = bps;
  p.sSpatialLayers[0].iMaxSpatialBitrate = UNSPECIFIED_BIT_RATE;
  if (e->enc->InitializeExt(&p) != 0) {
    openh264().destroyEncoder(e->enc);
    setErr(err, n, "OpenH264 rejected the encoder settings");
    return nullptr;
  }
  int format = videoFormatI420;
  e->enc->SetOption(ENCODER_OPTION_DATAFORMAT, &format);
  return e.release();
}

int32_t ohEncoderPush(jc_video_encoder* e, const jc_video_frame* f, jc_video_packet_fn fn, void* user, char* err,
                      size_t n) {
  if (f->width != e->width || f->height != e->height || f->format != JC_PIX_NV12) {
    setErr(err, n, "frame does not match the encoder");
    return JC_ERR_INVALID;
  }
  const uint32_t w = e->width, h = e->height;
  e->i420.resize(size_t(w) * h * 3 / 2);
  uint8_t* y = e->i420.data();
  uint8_t* u = y + size_t(w) * h;
  uint8_t* v = u + size_t(w / 2) * (h / 2);
  for (uint32_t r = 0; r < h; ++r) std::memcpy(y + size_t(r) * w, f->plane[0] + size_t(r) * f->stride[0], w);
  for (uint32_t r = 0; r < h / 2; ++r) {
    const uint8_t* uv = f->plane[1] + size_t(r) * f->stride[1];
    for (uint32_t c = 0; c < w / 2; ++c) {
      u[size_t(r) * (w / 2) + c] = uv[c * 2];
      v[size_t(r) * (w / 2) + c] = uv[c * 2 + 1];
    }
  }
  SSourcePicture pic{};
  pic.iColorFormat = videoFormatI420;
  pic.iPicWidth = int(w);
  pic.iPicHeight = int(h);
  pic.iStride[0] = int(w);
  pic.iStride[1] = pic.iStride[2] = int(w / 2);
  pic.pData[0] = y;
  pic.pData[1] = u;
  pic.pData[2] = v;
  pic.uiTimeStamp = f->pts_us / 1000;
  SFrameBSInfo info{};
  if (e->enc->EncodeFrame(&pic, &info) != cmResultSuccess) {
    setErr(err, n, "OpenH264 failed to encode a frame");
    return JC_ERR_FAILED;
  }
  if (info.eFrameType == videoFrameTypeSkip || info.eFrameType == videoFrameTypeInvalid) return JC_OK;
  Bytes packet;
  for (int l = 0; l < info.iLayerNum; ++l) {
    const SLayerBSInfo& layer = info.sLayerInfo[l];
    const uint8_t* p = layer.pBsBuf;
    for (int k = 0; k < layer.iNalCount; ++k) {
      for (auto& nal : splitAnnexB(p, size_t(layer.pNalLengthInByte[k]))) {
        const int t = avcNalType(nal);
        if (t == 7) e->ps.sps = {nal};
        else if (t == 8) e->ps.pps = {nal};
        else if (t != 9) appendLengthPrefixed(packet, nal);
      }
      p += layer.pNalLengthInByte[k];
    }
  }
  if (packet.empty()) return JC_OK;
  return fn(user, packet.data(), packet.size(), f->pts_us, f->pts_us, info.eFrameType == videoFrameTypeIDR ? 1 : 0);
}

int32_t ohEncoderFlush(jc_video_encoder*, jc_video_packet_fn, void*, char*, size_t) { return JC_OK; }

int32_t ohEncoderConfig(jc_video_encoder* e, uint8_t* out, size_t cap, size_t* size) {
  Bytes cfg = buildAvcC(e->ps);
  if (cfg.empty()) return JC_ERR_FAILED;
  if (size) *size = cfg.size();
  if (!out || cap < cfg.size()) return JC_ERR_INVALID;
  std::memcpy(out, cfg.data(), cfg.size());
  return JC_OK;
}

void ohEncoderDestroy(jc_video_encoder* e) {
  if (e->enc) {
    e->enc->Uninitialize();
    openh264().destroyEncoder(e->enc);
  }
  delete e;
}

const jc_video_backend kOpenH264 = {
    JC_VIDEO_BACKEND_ABI_VERSION, sizeof(jc_video_backend), "OpenH264",
    ohCanDecode, ohCanEncode, ohDecoderCreate, ohDecoderPush, ohDecoderFlush, ohDecoderDestroy,
    ohEncoderCreate, ohEncoderPush, ohEncoderFlush, ohEncoderConfig, ohEncoderDestroy};

#endif  // JOWE_CONVERT_HAS_OPENH264

const LinuxVideoChoice& linuxChoice() {
  static LinuxVideoChoice choice;
  static std::once_flag once;
  std::call_once(once, [] {
    LinuxVideoProbe probe;
    probe.vaapi = probeVaapi();
    probe.v4l2 = probeV4l2();
#if defined(JOWE_CONVERT_HAS_OPENH264)
    probe.openh264 = openh264().ok;
    probe.openh264Detail = openh264().detail;
#else
    probe.openh264Detail = "not built in (JOWE_CONVERT_WITH_OPENH264=OFF)";
#endif
    choice = chooseLinuxVideoBackend(probe);
  });
  return choice;
}

}  // namespace

const jc_video_backend* builtinVideoBackend() {
#if defined(JOWE_CONVERT_HAS_OPENH264)
  if (linuxChoice().backend == "openh264") return &kOpenH264;
#endif
  return nullptr;
}

std::string noBuiltinVideoBackendReason() { return linuxChoice().reason; }

std::vector<std::string> videoBackendNotes() { return linuxChoice().notes; }

}  // namespace jowe::convert::detail

#endif
