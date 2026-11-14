#include "backend.hpp"

#if defined(JOWE_CONVERT_OS_CODECS) && defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
// The CODECAPI_* property GUIDs have no import library; this defines them here.
#include <initguid.h>
#include <codecapi.h>
#include <wrl/client.h>

#include <cstdio>
#include <memory>
#include <mutex>

#include "nal.hpp"

using Microsoft::WRL::ComPtr;

namespace jowe::convert::detail {

namespace {

void ensureMf() {
  static std::once_flag once;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  std::call_once(once, [] { MFStartup(MF_VERSION, MFSTARTUP_LITE); });
}

void setErr(char* err, size_t n, const std::string& msg, HRESULT hr = S_OK) {
  if (!err || !n) return;
  char code[24] = "";
  if (hr != S_OK) std::snprintf(code, sizeof code, " (0x%08lX)", static_cast<unsigned long>(hr));
  std::snprintf(err, n, "%s%s", msg.c_str(), code);
}

bool subtypeFor(const char* codec, GUID& out) {
  std::string c = codec ? codec : "";
  if (c == "avc1" || c == "avc3") { out = MFVideoFormat_H264; return true; }
  if (c == "hvc1" || c == "hev1") { out = MFVideoFormat_HEVC; return true; }
  return false;
}

ComPtr<IMFTransform> findMft(const GUID& category, const GUID& in, const GUID& out) {
  ensureMf();
  MFT_REGISTER_TYPE_INFO ti{MFMediaType_Video, in}, to{MFMediaType_Video, out};
  IMFActivate** acts = nullptr;
  UINT32 n = 0;
  ComPtr<IMFTransform> mft;
  const UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;
  if (FAILED(MFTEnumEx(category, flags, &ti, &to, &acts, &n))) return mft;
  for (UINT32 i = 0; i < n; ++i) {
    if (!mft) {
      ComPtr<IMFTransform> candidate;
      if (SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&candidate)))) mft = candidate;
    }
    acts[i]->Release();
  }
  CoTaskMemFree(acts);
  return mft;
}

bool isHevc(const char* codec) { return codec && (std::string(codec) == "hvc1" || std::string(codec) == "hev1"); }

int32_t canDecode(const char* codec, char* reason, size_t n) {
  GUID sub;
  if (!subtypeFor(codec, sub)) {
    setErr(reason, n, std::string("no decoder for ") + (codec ? codec : "?"));
    return 0;
  }
  if (findMft(MFT_CATEGORY_VIDEO_DECODER, sub, MFVideoFormat_NV12)) return 1;
  setErr(reason, n, isHevc(codec) ? "HEVC decoding needs the HEVC Video Extensions from the Microsoft Store"
                                  : "No H.264 decoder in this Windows installation (Media Feature Pack missing?)");
  return 0;
}

int32_t canEncode(const char* codec, char* reason, size_t n) {
  GUID sub;
  if (!subtypeFor(codec, sub)) {
    setErr(reason, n, std::string("no encoder for ") + (codec ? codec : "?"));
    return 0;
  }
  if (findMft(MFT_CATEGORY_VIDEO_ENCODER, MFVideoFormat_NV12, sub)) return 1;
  setErr(reason, n, isHevc(codec) ? "No synchronous HEVC encoder on this PC (hardware-only HEVC encoders are not used yet)"
                                  : "No H.264 encoder in this Windows installation (Media Feature Pack missing?)");
  return 0;
}

struct Output {
  ComPtr<IMFSample> sample;
  bool ok = false;
  bool needInput = false;
  bool streamChange = false;
  HRESULT hr = S_OK;
};

Output pullOutput(IMFTransform* mft) {
  Output o;
  MFT_OUTPUT_STREAM_INFO info{};
  mft->GetOutputStreamInfo(0, &info);
  MFT_OUTPUT_DATA_BUFFER buf{};
  ComPtr<IMFSample> own;
  if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
    ComPtr<IMFMediaBuffer> mb;
    MFCreateSample(&own);
    MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 1), &mb);
    own->AddBuffer(mb.Get());
    buf.pSample = own.Get();
  }
  DWORD status = 0;
  o.hr = mft->ProcessOutput(0, 1, &buf, &status);
  if (buf.pEvents) buf.pEvents->Release();
  if (o.hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
    o.needInput = true;
  } else if (o.hr == MF_E_TRANSFORM_STREAM_CHANGE) {
    o.streamChange = true;
  } else if (SUCCEEDED(o.hr)) {
    o.ok = true;
    o.sample = buf.pSample;
  }
  if (buf.pSample && buf.pSample != own.Get()) buf.pSample->Release();
  return o;
}

}  // namespace

}  // namespace jowe::convert::detail

struct jc_video_decoder {
  ComPtr<IMFTransform> mft;
  jowe::convert::detail::ParameterSets ps;
  bool hevc = false;
  UINT32 allocW = 0, allocH = 0, dispW = 0, dispH = 0;
  LONG stride = 0;
};

struct jc_video_encoder {
  ComPtr<IMFTransform> mft;
  jowe::convert::detail::ParameterSets ps;
  bool hevc = false;
  uint32_t width = 0, height = 0;
  LONGLONG frameDuration = 0;
  std::vector<uint8_t> scratch;
};

namespace jowe::convert::detail {

namespace {

bool setNv12Output(jc_video_decoder* d) {
  for (DWORD i = 0;; ++i) {
    ComPtr<IMFMediaType> t;
    if (FAILED(d->mft->GetOutputAvailableType(0, i, &t))) return false;
    GUID sub{};
    t->GetGUID(MF_MT_SUBTYPE, &sub);
    if (sub != MFVideoFormat_NV12) continue;
    if (FAILED(d->mft->SetOutputType(0, t.Get(), 0))) return false;
    MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &d->allocW, &d->allocH);
    d->dispW = d->allocW;
    d->dispH = d->allocH;
    MFVideoArea area{};
    if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof area, nullptr)) &&
        area.Area.cx > 0 && area.Area.cy > 0) {
      d->dispW = UINT32(area.Area.cx);
      d->dispH = UINT32(area.Area.cy);
    }
    UINT32 stride = 0;
    d->stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? LONG(int32_t(stride)) : LONG(d->allocW);
    return true;
  }
}

int32_t emitFrame(jc_video_decoder* d, IMFSample* s, jc_video_frame_fn fn, void* user) {
  LONGLONG t = 0;
  s->GetSampleTime(&t);
  ComPtr<IMFMediaBuffer> mb;
  if (FAILED(s->ConvertToContiguousBuffer(&mb))) return JC_ERR_FAILED;
  BYTE* base = nullptr;
  LONG pitch = 0;
  ComPtr<IMF2DBuffer> b2;
  bool locked2d = SUCCEEDED(mb.As(&b2)) && SUCCEEDED(b2->Lock2D(&base, &pitch));
  DWORD len = 0;
  if (!locked2d) {
    if (FAILED(mb->Lock(&base, nullptr, &len))) return JC_ERR_FAILED;
    pitch = d->stride > 0 ? d->stride : LONG(d->allocW);
  }
  jc_video_frame f{};
  f.struct_size = sizeof f;
  f.format = JC_PIX_NV12;
  f.width = d->dispW & ~1u;
  f.height = d->dispH & ~1u;
  f.plane[0] = base;
  f.plane[1] = base + size_t(pitch) * d->allocH;
  f.stride[0] = f.stride[1] = uint32_t(pitch);
  f.pts_us = t / 10;
  int32_t rc = fn(user, &f);
  if (locked2d) b2->Unlock2D();
  else mb->Unlock();
  return rc;
}

int32_t drainDecoder(jc_video_decoder* d, jc_video_frame_fn fn, void* user, char* err, size_t n) {
  for (int guard = 0; guard < 1000; ++guard) {
    Output o = pullOutput(d->mft.Get());
    if (o.needInput) return JC_OK;
    if (o.streamChange) {
      if (!setNv12Output(d)) {
        setErr(err, n, "the decoder has no NV12 output");
        return JC_ERR_FAILED;
      }
      continue;
    }
    if (!o.ok) {
      setErr(err, n, "decoder output failed", o.hr);
      return JC_ERR_FAILED;
    }
    if (int32_t rc = emitFrame(d, o.sample.Get(), fn, user); rc != JC_OK) return rc;
  }
  return JC_OK;
}

jc_video_decoder* decoderCreate(const char* codec, const uint8_t* config, size_t configSize, uint32_t w, uint32_t h,
                                char* err, size_t n) {
  GUID sub;
  if (!subtypeFor(codec, sub)) {
    setErr(err, n, "unsupported codec");
    return nullptr;
  }
  auto d = std::make_unique<jc_video_decoder>();
  d->hevc = isHevc(codec);
  if (!(d->hevc ? parseHvcC(config, configSize, d->ps) : parseAvcC(config, configSize, d->ps))) {
    setErr(err, n, "invalid decoder configuration record");
    return nullptr;
  }
  d->mft = findMft(MFT_CATEGORY_VIDEO_DECODER, sub, MFVideoFormat_NV12);
  if (!d->mft) {
    canDecode(codec, err, n);
    return nullptr;
  }
  ComPtr<IMFMediaType> in;
  MFCreateMediaType(&in);
  in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  in->SetGUID(MF_MT_SUBTYPE, sub);
  MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, w, h);
  in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  HRESULT hr = d->mft->SetInputType(0, in.Get(), 0);
  if (FAILED(hr)) {
    setErr(err, n, "the decoder rejected the stream", hr);
    return nullptr;
  }
  if (!setNv12Output(d.get())) {
    setErr(err, n, "the decoder has no NV12 output");
    return nullptr;
  }
  d->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  d->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  return d.release();
}

int32_t decoderPush(jc_video_decoder* d, const uint8_t* data, size_t size, int64_t pts, int32_t key,
                    jc_video_frame_fn fn, void* user, char* err, size_t n) {
  Bytes annexb;
  if (key) {
    for (auto* list : {&d->ps.vps, &d->ps.sps, &d->ps.pps})
      for (auto& x : *list) appendAnnexB(annexb, x);
  }
  for (auto& nal : splitLengthPrefixed(data, size, d->ps.lengthSize)) appendAnnexB(annexb, nal);
  ComPtr<IMFSample> s;
  ComPtr<IMFMediaBuffer> mb;
  MFCreateSample(&s);
  MFCreateMemoryBuffer(DWORD(annexb.size()), &mb);
  BYTE* p = nullptr;
  mb->Lock(&p, nullptr, nullptr);
  std::memcpy(p, annexb.data(), annexb.size());
  mb->Unlock();
  mb->SetCurrentLength(DWORD(annexb.size()));
  s->AddBuffer(mb.Get());
  s->SetSampleTime(pts * 10);
  if (key) s->SetUINT32(MFSampleExtension_CleanPoint, 1);
  for (int attempt = 0; attempt < 2; ++attempt) {
    HRESULT hr = d->mft->ProcessInput(0, s.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
      if (int32_t rc = drainDecoder(d, fn, user, err, n); rc != JC_OK) return rc;
      continue;
    }
    if (FAILED(hr)) {
      setErr(err, n, "decoder input failed", hr);
      return JC_ERR_FAILED;
    }
    break;
  }
  return drainDecoder(d, fn, user, err, n);
}

int32_t decoderFlush(jc_video_decoder* d, jc_video_frame_fn fn, void* user, char* err, size_t n) {
  d->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
  d->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
  return drainDecoder(d, fn, user, err, n);
}

void decoderDestroy(jc_video_decoder* d) { delete d; }

void setCodecApi(IMFTransform* mft, const GUID& key, UINT32 value) {
  ComPtr<ICodecAPI> api;
  if (FAILED(mft->QueryInterface(IID_PPV_ARGS(&api)))) return;
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_UI4;
  v.ulVal = value;
  api->SetValue(&key, &v);
}

int32_t takePackets(jc_video_encoder* e, jc_video_packet_fn fn, void* user, char* err, size_t n) {
  for (int guard = 0; guard < 1000; ++guard) {
    Output o = pullOutput(e->mft.Get());
    if (o.needInput) return JC_OK;
    if (o.streamChange) continue;
    if (!o.ok) {
      setErr(err, n, "encoder output failed", o.hr);
      return JC_ERR_FAILED;
    }
    ComPtr<IMFMediaBuffer> mb;
    o.sample->ConvertToContiguousBuffer(&mb);
    BYTE* p = nullptr;
    DWORD len = 0;
    mb->Lock(&p, nullptr, &len);
    auto nals = splitAnnexB(p, len);
    mb->Unlock();
    Bytes packet;
    bool key = MFGetAttributeUINT32(o.sample.Get(), MFSampleExtension_CleanPoint, 0) != 0;
    for (auto& nal : nals) {
      const int t = e->hevc ? hevcNalType(nal) : avcNalType(nal);
      if (e->hevc) {
        if (t == 32) { e->ps.vps = {nal}; continue; }
        if (t == 33) { e->ps.sps = {nal}; continue; }
        if (t == 34) { e->ps.pps = {nal}; continue; }
        if (t == 35) continue;
        if (t >= 16 && t <= 21) key = true;
      } else {
        if (t == 7) { e->ps.sps = {nal}; continue; }
        if (t == 8) { e->ps.pps = {nal}; continue; }
        if (t == 9) continue;
        if (t == 5) key = true;
      }
      appendLengthPrefixed(packet, nal);
    }
    if (packet.empty()) continue;
    LONGLONG t = 0;
    o.sample->GetSampleTime(&t);
    if (int32_t rc = fn(user, packet.data(), packet.size(), t / 10, t / 10, key ? 1 : 0); rc != JC_OK) return rc;
  }
  return JC_OK;
}

jc_video_encoder* encoderCreate(const jc_video_encoder_config* c, char* err, size_t n) {
  GUID sub;
  if (!c || !subtypeFor(c->codec, sub) || !c->width || !c->height || !c->fps_num || !c->fps_den) {
    setErr(err, n, "invalid encoder configuration");
    return nullptr;
  }
  auto e = std::make_unique<jc_video_encoder>();
  e->hevc = isHevc(c->codec);
  e->width = c->width;
  e->height = c->height;
  e->frameDuration = LONGLONG(10000000.0 * c->fps_den / c->fps_num);
  e->mft = findMft(MFT_CATEGORY_VIDEO_ENCODER, MFVideoFormat_NV12, sub);
  if (!e->mft) {
    canEncode(c->codec, err, n);
    return nullptr;
  }
  const UINT32 bps = c->bitrate_kbps * 1000;
  setCodecApi(e->mft.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
  setCodecApi(e->mft.Get(), CODECAPI_AVEncCommonMeanBitRate, bps);
  setCodecApi(e->mft.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
  if (c->keyframe_interval) setCodecApi(e->mft.Get(), CODECAPI_AVEncMPVGOPSize, c->keyframe_interval);

  ComPtr<IMFMediaType> out;
  MFCreateMediaType(&out);
  out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  out->SetGUID(MF_MT_SUBTYPE, sub);
  out->SetUINT32(MF_MT_AVG_BITRATE, bps);
  MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, c->fps_num, c->fps_den);
  MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, c->width, c->height);
  out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  if (!e->hevc) out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
  HRESULT hr = e->mft->SetOutputType(0, out.Get(), 0);
  if (FAILED(hr) && !e->hevc) {
    out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    hr = e->mft->SetOutputType(0, out.Get(), 0);
  }
  if (FAILED(hr)) {
    setErr(err, n, "the encoder rejected the output format", hr);
    return nullptr;
  }
  ComPtr<IMFMediaType> in;
  MFCreateMediaType(&in);
  in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, c->fps_num, c->fps_den);
  MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, c->width, c->height);
  in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  hr = e->mft->SetInputType(0, in.Get(), 0);
  if (FAILED(hr)) {
    setErr(err, n, "the encoder rejected NV12 input", hr);
    return nullptr;
  }
  ComPtr<IMFMediaType> cur;
  if (SUCCEEDED(e->mft->GetOutputCurrentType(0, &cur))) {
    UINT32 sz = 0;
    if (SUCCEEDED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &sz)) && sz) {
      Bytes hdr(sz);
      cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, hdr.data(), sz, nullptr);
      for (auto& nal : splitAnnexB(hdr.data(), hdr.size())) {
        const int t = e->hevc ? hevcNalType(nal) : avcNalType(nal);
        if (e->hevc ? t == 32 : false) e->ps.vps = {nal};
        if (t == (e->hevc ? 33 : 7)) e->ps.sps = {nal};
        if (t == (e->hevc ? 34 : 8)) e->ps.pps = {nal};
      }
    }
  }
  e->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  e->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  return e.release();
}

int32_t encoderPush(jc_video_encoder* e, const jc_video_frame* f, jc_video_packet_fn fn, void* user, char* err,
                    size_t n) {
  if (f->width != e->width || f->height != e->height || f->format != JC_PIX_NV12) {
    setErr(err, n, "frame does not match the encoder");
    return JC_ERR_INVALID;
  }
  const size_t ySize = size_t(e->width) * e->height;
  ComPtr<IMFSample> s;
  ComPtr<IMFMediaBuffer> mb;
  MFCreateSample(&s);
  MFCreateMemoryBuffer(DWORD(ySize * 3 / 2), &mb);
  BYTE* p = nullptr;
  mb->Lock(&p, nullptr, nullptr);
  for (uint32_t y = 0; y < e->height; ++y) std::memcpy(p + size_t(y) * e->width, f->plane[0] + size_t(y) * f->stride[0], e->width);
  for (uint32_t y = 0; y < e->height / 2; ++y)
    std::memcpy(p + ySize + size_t(y) * e->width, f->plane[1] + size_t(y) * f->stride[1], e->width);
  mb->Unlock();
  mb->SetCurrentLength(DWORD(ySize * 3 / 2));
  s->AddBuffer(mb.Get());
  s->SetSampleTime(f->pts_us * 10);
  s->SetSampleDuration(e->frameDuration);
  for (int attempt = 0; attempt < 2; ++attempt) {
    HRESULT hr = e->mft->ProcessInput(0, s.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
      if (int32_t rc = takePackets(e, fn, user, err, n); rc != JC_OK) return rc;
      continue;
    }
    if (FAILED(hr)) {
      setErr(err, n, "encoder input failed", hr);
      return JC_ERR_FAILED;
    }
    break;
  }
  return takePackets(e, fn, user, err, n);
}

int32_t encoderFlush(jc_video_encoder* e, jc_video_packet_fn fn, void* user, char* err, size_t n) {
  e->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
  e->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
  return takePackets(e, fn, user, err, n);
}

int32_t encoderConfig(jc_video_encoder* e, uint8_t* out, size_t cap, size_t* size) {
  Bytes cfg = e->hevc ? buildHvcC(e->ps) : buildAvcC(e->ps);
  if (cfg.empty()) return JC_ERR_FAILED;
  if (size) *size = cfg.size();
  if (!out || cap < cfg.size()) return JC_ERR_INVALID;
  std::memcpy(out, cfg.data(), cfg.size());
  return JC_OK;
}

void encoderDestroy(jc_video_encoder* e) { delete e; }

const jc_video_backend kMf = {
    JC_VIDEO_BACKEND_ABI_VERSION, sizeof(jc_video_backend), "Media Foundation",
    canDecode, canEncode, decoderCreate, decoderPush, decoderFlush, decoderDestroy,
    encoderCreate, encoderPush, encoderFlush, encoderConfig, encoderDestroy};

}  // namespace

const jc_video_backend* builtinVideoBackend() { return &kMf; }
std::string noBuiltinVideoBackendReason() { return {}; }

}  // namespace jowe::convert::detail

#endif
