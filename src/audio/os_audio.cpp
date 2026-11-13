#include "audio.hpp"

#include <cmath>

#if defined(JOWE_CONVERT_OS_CODECS) && defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#elif defined(JOWE_CONVERT_OS_CODECS) && defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace jowe::convert::detail {

namespace {
int aacRate(int rate) { return rate <= 44100 ? 44100 : 48000; }
}  // namespace

#if defined(JOWE_CONVERT_OS_CODECS) && defined(_WIN32)

namespace {

using Microsoft::WRL::ComPtr;

struct MfScope {
  bool com = false, mf = false;
  MfScope() {
    com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
  }
  ~MfScope() {
    if (mf) MFShutdown();
    if (com) CoUninitialize();
  }
};

bool hasEncoder(const GUID& subtype) {
  MfScope mf;
  MFT_REGISTER_TYPE_INFO info{MFMediaType_Audio, subtype};
  IMFActivate** acts = nullptr;
  UINT32 n = 0;
  if (FAILED(MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_ALL, nullptr, &info, &acts, &n))) return false;
  for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
  CoTaskMemFree(acts);
  return n > 0;
}

std::string hr(HRESULT h) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(h));
  return buf;
}

}  // namespace

bool osAudioCanDecode(const std::string& format, std::string* reason) {
  if (format == "m4a" || format == "mp3" || format == "aac") return true;
  if (reason) *reason = "No OS decoder for " + format;
  return false;
}

bool osAudioDecode(const fs::path& path, const std::string&, Pcm& out, std::string& err) {
  MfScope mf;
  if (!mf.mf) { err = "Media Foundation is unavailable"; return false; }
  ComPtr<IMFSourceReader> reader;
  HRESULT h = MFCreateSourceReaderFromURL(path.wstring().c_str(), nullptr, &reader);
  if (FAILED(h)) { err = "Media Foundation can't open the file (" + hr(h) + ")"; return false; }
  reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
  ComPtr<IMFMediaType> want;
  MFCreateMediaType(&want);
  want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
  h = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, want.Get());
  if (FAILED(h)) { err = "no audio decoder for this stream (" + hr(h) + ")"; return false; }
  ComPtr<IMFMediaType> got;
  reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &got);
  UINT32 rate = 0, ch = 0;
  got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
  got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
  out = Pcm{};
  out.sampleRate = int(rate);
  out.channels = int(ch);
  out.sourceBits = 16;
  for (;;) {
    DWORD idx = 0, flags = 0;
    LONGLONG ts = 0;
    ComPtr<IMFSample> sample;
    h = reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &idx, &flags, &ts, &sample);
    if (FAILED(h)) { err = "decode failed (" + hr(h) + ")"; return false; }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    if (!sample) continue;
    ComPtr<IMFMediaBuffer> buf;
    sample->ConvertToContiguousBuffer(&buf);
    BYTE* p = nullptr;
    DWORD len = 0;
    if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
      const float* f = reinterpret_cast<const float*>(p);
      out.samples.insert(out.samples.end(), f, f + len / sizeof(float));
      buf->Unlock();
    }
  }
  if (out.samples.empty() || !out.channels) { err = "no audio decoded"; return false; }
  return true;
}

bool osAudioCanEncode(const std::string& format, std::string* reason) {
  if (format == "m4a") {
    if (hasEncoder(MFAudioFormat_AAC)) return true;
    if (reason) *reason = "No AAC encoder in this Windows installation (Media Feature Pack missing?)";
    return false;
  }
  if (format == "mp3") {
    if (hasEncoder(MFAudioFormat_MP3)) return true;
    if (reason) *reason = "No MP3 encoder in this Windows installation";
    return false;
  }
  if (reason) *reason = "No OS encoder for " + format;
  return false;
}

void osAudioNegotiate(const std::string& format, int& rate, int& channels, int& kbps) {
  channels = channels >= 2 ? 2 : 1;
  if (format == "m4a") {
    rate = aacRate(rate);
    if (!kbps) kbps = channels == 1 ? 96 : 192;
    if (channels == 1) kbps = std::min(kbps, 160);
    kbps = kbps <= 112 ? 96 : kbps <= 144 ? 128 : kbps <= 176 ? 160 : 192;
  } else {
    rate = rate <= 32000 ? 32000 : rate <= 44100 ? 44100 : 48000;
    if (!kbps) kbps = 192;
    static const int allowed[] = {64, 96, 128, 160, 192, 224, 256, 320};
    int best = 192;
    for (int a : allowed)
      if (a <= kbps) best = a;
    kbps = best;
  }
}

bool osAudioEncode(const Pcm& pcm, const std::string& format, int kbps, const fs::path& out, std::string& err) {
  MfScope mf;
  if (!mf.mf) { err = "Media Foundation is unavailable"; return false; }
  const bool aac = format == "m4a";
  ComPtr<IMFAttributes> attrs;
  MFCreateAttributes(&attrs, 2);
  attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, aac ? MFTranscodeContainerType_MPEG4 : MFTranscodeContainerType_MP3);
  ComPtr<IMFSinkWriter> writer;
  HRESULT h = MFCreateSinkWriterFromURL(out.wstring().c_str(), nullptr, attrs.Get(), &writer);
  if (FAILED(h)) { err = "can't create the output (" + hr(h) + ")"; return false; }

  ComPtr<IMFMediaType> outType;
  MFCreateMediaType(&outType);
  outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  outType->SetGUID(MF_MT_SUBTYPE, aac ? MFAudioFormat_AAC : MFAudioFormat_MP3);
  outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, UINT32(pcm.sampleRate));
  outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, UINT32(pcm.channels));
  outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, UINT32(kbps * 1000 / 8));
  if (aac) outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  DWORD stream = 0;
  h = writer->AddStream(outType.Get(), &stream);
  if (FAILED(h)) { err = "the encoder rejected the format (" + hr(h) + ")"; return false; }

  ComPtr<IMFMediaType> inType;
  MFCreateMediaType(&inType);
  inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
  inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, UINT32(pcm.sampleRate));
  inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, UINT32(pcm.channels));
  inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, UINT32(pcm.channels * 2));
  inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, UINT32(pcm.sampleRate * pcm.channels * 2));
  h = writer->SetInputMediaType(stream, inType.Get(), nullptr);
  if (FAILED(h)) { err = "the encoder rejected the input (" + hr(h) + ")"; return false; }
  if (FAILED(h = writer->BeginWriting())) { err = "BeginWriting failed (" + hr(h) + ")"; return false; }

  const size_t frames = pcm.frames(), chunk = 4096;
  for (size_t f = 0; f < frames; f += chunk) {
    const size_t n = std::min(chunk, frames - f);
    ComPtr<IMFMediaBuffer> buf;
    MFCreateMemoryBuffer(DWORD(n * pcm.channels * 2), &buf);
    BYTE* p = nullptr;
    buf->Lock(&p, nullptr, nullptr);
    auto* s = reinterpret_cast<int16_t*>(p);
    for (size_t i = 0; i < n * pcm.channels; ++i) {
      float v = std::clamp(pcm.samples[f * pcm.channels + i], -1.0f, 1.0f);
      s[i] = int16_t(std::lround(v * 32767.0f));
    }
    buf->Unlock();
    buf->SetCurrentLength(DWORD(n * pcm.channels * 2));
    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(LONGLONG(f) * 10000000 / pcm.sampleRate);
    sample->SetSampleDuration(LONGLONG(n) * 10000000 / pcm.sampleRate);
    if (FAILED(h = writer->WriteSample(stream, sample.Get()))) { err = "encode failed (" + hr(h) + ")"; return false; }
  }
  if (FAILED(h = writer->Finalize())) { err = "finalize failed (" + hr(h) + ")"; return false; }
  return true;
}

#elif defined(JOWE_CONVERT_OS_CODECS) && defined(__APPLE__)

namespace {

CFURLRef urlFor(const fs::path& p) {
  std::string s = p.string();
  return CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(s.data()), CFIndex(s.size()),
                                                 false);
}

AudioStreamBasicDescription floatFormat(double rate, UInt32 channels) {
  AudioStreamBasicDescription d{};
  d.mSampleRate = rate;
  d.mFormatID = kAudioFormatLinearPCM;
  d.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  d.mChannelsPerFrame = channels;
  d.mBitsPerChannel = 32;
  d.mFramesPerPacket = 1;
  d.mBytesPerFrame = 4 * channels;
  d.mBytesPerPacket = 4 * channels;
  return d;
}

}  // namespace

bool osAudioCanDecode(const std::string& format, std::string* reason) {
  if (format == "m4a" || format == "mp3" || format == "caf" || format == "aac") return true;
  if (reason) *reason = "No OS decoder for " + format;
  return false;
}

bool osAudioDecode(const fs::path& path, const std::string&, Pcm& out, std::string& err) {
  CFURLRef url = urlFor(path);
  ExtAudioFileRef file = nullptr;
  OSStatus st = ExtAudioFileOpenURL(url, &file);
  CFRelease(url);
  if (st != noErr || !file) { err = "AudioToolbox can't open the file (" + std::to_string(st) + ")"; return false; }
  AudioStreamBasicDescription src{};
  UInt32 sz = sizeof src;
  ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileDataFormat, &sz, &src);
  AudioStreamBasicDescription client = floatFormat(src.mSampleRate, src.mChannelsPerFrame);
  ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof client, &client);
  out = Pcm{};
  out.sampleRate = int(src.mSampleRate);
  out.channels = int(src.mChannelsPerFrame);
  out.sourceBits = src.mBitsPerChannel ? int(src.mBitsPerChannel) : 16;
  std::vector<float> buf(4096 * src.mChannelsPerFrame);
  for (;;) {
    AudioBufferList list;
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = src.mChannelsPerFrame;
    list.mBuffers[0].mDataByteSize = UInt32(buf.size() * sizeof(float));
    list.mBuffers[0].mData = buf.data();
    UInt32 frames = 4096;
    if (ExtAudioFileRead(file, &frames, &list) != noErr) { err = "decode failed"; break; }
    if (frames == 0) break;
    out.samples.insert(out.samples.end(), buf.begin(), buf.begin() + size_t(frames) * src.mChannelsPerFrame);
  }
  ExtAudioFileDispose(file);
  return !out.samples.empty() && out.channels > 0;
}

bool osAudioCanEncode(const std::string& format, std::string* reason) {
  if (format == "m4a") return true;
  if (reason) *reason = format == "mp3" ? "No MP3 encoder on Apple platforms" : "No OS encoder for " + format;
  return false;
}

void osAudioNegotiate(const std::string&, int& rate, int& channels, int& kbps) {
  channels = channels >= 2 ? 2 : 1;
  if (rate > 48000) rate = 48000;
  if (rate < 8000) rate = 8000;
  if (!kbps) kbps = channels == 1 ? 96 : 192;
  if (channels == 1) kbps = std::min(kbps, 160);
}

bool osAudioEncode(const Pcm& pcm, const std::string&, int kbps, const fs::path& out, std::string& err) {
  AudioStreamBasicDescription aac{};
  aac.mSampleRate = pcm.sampleRate;
  aac.mFormatID = kAudioFormatMPEG4AAC;
  aac.mChannelsPerFrame = UInt32(pcm.channels);
  aac.mFramesPerPacket = 1024;
  CFURLRef url = urlFor(out);
  ExtAudioFileRef file = nullptr;
  OSStatus st = ExtAudioFileCreateWithURL(url, kAudioFileM4AType, &aac, nullptr, kAudioFileFlags_EraseFile, &file);
  CFRelease(url);
  if (st != noErr || !file) { err = "can't create the AAC file (" + std::to_string(st) + ")"; return false; }
  AudioStreamBasicDescription client = floatFormat(pcm.sampleRate, UInt32(pcm.channels));
  ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof client, &client);
  AudioConverterRef conv = nullptr;
  UInt32 sz = sizeof conv;
  if (ExtAudioFileGetProperty(file, kExtAudioFileProperty_AudioConverter, &sz, &conv) == noErr && conv) {
    UInt32 bps = UInt32(kbps * 1000);
    AudioConverterSetProperty(conv, kAudioConverterEncodeBitRate, sizeof bps, &bps);
    CFArrayRef none = nullptr;
    ExtAudioFileSetProperty(file, kExtAudioFileProperty_ConverterConfig, sizeof none, &none);
  }
  const size_t frames = pcm.frames(), chunk = 4096;
  bool ok = true;
  for (size_t f = 0; f < frames && ok; f += chunk) {
    UInt32 n = UInt32(std::min(chunk, frames - f));
    AudioBufferList list;
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = UInt32(pcm.channels);
    list.mBuffers[0].mDataByteSize = n * UInt32(pcm.channels) * 4;
    list.mBuffers[0].mData = const_cast<float*>(&pcm.samples[f * pcm.channels]);
    ok = ExtAudioFileWrite(file, n, &list) == noErr;
  }
  ExtAudioFileDispose(file);
  if (!ok) err = "AAC encoding failed";
  return ok;
}

#else

bool osAudioCanDecode(const std::string& format, std::string* reason) {
  if (reason) *reason = (format == "m4a" ? std::string("M4A/AAC can't be read on this platform yet (no OS decoder)")
                                         : "No decoder for " + format);
  return false;
}
bool osAudioDecode(const fs::path&, const std::string& format, Pcm&, std::string& err) {
  osAudioCanDecode(format, &err);
  return false;
}
bool osAudioCanEncode(const std::string& format, std::string* reason) {
  if (reason)
    *reason = format == "m4a" ? "No AAC encoder on this platform (AAC uses the OS encoder)"
                              : "No " + format + " encoder on this platform";
  return false;
}
void osAudioNegotiate(const std::string&, int& rate, int& channels, int&) {
  rate = aacRate(rate);
  channels = std::min(channels, 2);
}
bool osAudioEncode(const Pcm&, const std::string& format, int, const fs::path&, std::string& err) {
  osAudioCanEncode(format, &err);
  return false;
}

#endif

}  // namespace jowe::convert::detail
