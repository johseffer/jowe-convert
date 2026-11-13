#include "image.hpp"

#if defined(JOWE_CONVERT_OS_CODECS) && defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#elif defined(JOWE_CONVERT_OS_CODECS) && defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

namespace jowe::convert::detail {

#if defined(JOWE_CONVERT_OS_CODECS) && defined(_WIN32)

namespace {

using Microsoft::WRL::ComPtr;

struct ComScope {
  bool owns = false;
  ComScope() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    owns = SUCCEEDED(hr);
  }
  ~ComScope() {
    if (owns) CoUninitialize();
  }
};

const GUID* containerFor(const std::string& format) {
  if (format == "heic") return &GUID_ContainerFormatHeif;
  if (format == "avif") return &GUID_ContainerFormatHeif;
  return nullptr;
}

}  // namespace

bool osImageCanDecode(const std::string& format, std::string* reason) {
  const GUID* container = containerFor(format);
  if (!container) {
    if (reason) *reason = "No OS decoder for this format";
    return false;
  }
  ComScope com;
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
    if (reason) *reason = "Windows Imaging Component is unavailable";
    return false;
  }
  ComPtr<IWICBitmapDecoder> dec;
  if (FAILED(factory->CreateDecoder(*container, nullptr, &dec))) {
    if (reason)
      *reason = format == "avif" ? "AVIF needs the AV1 Video Extension from the Microsoft Store"
                                 : "HEIC needs the HEIF Image Extensions from the Microsoft Store";
    return false;
  }
  return true;
}

bool osImageDecode(const Bytes& in, const std::string& format, Image& out, std::string& err) {
  ComScope com;
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
    err = "Windows Imaging Component is unavailable";
    return false;
  }
  ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(in.data()), DWORD(in.size())))) {
    err = "WIC stream failed";
    return false;
  }
  ComPtr<IWICBitmapDecoder> dec;
  if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec))) {
    err = format == "heic" ? "Windows could not decode this HEIC (HEIF and HEVC Video Extensions are needed)"
                           : "Windows could not decode this image";
    return false;
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(dec->GetFrame(0, &frame))) {
    err = "no image frame";
    return false;
  }
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(factory->CreateFormatConverter(&conv)) ||
      FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeCustom))) {
    err = "pixel format conversion failed (the HEVC Video Extension may be missing)";
    return false;
  }
  UINT w = 0, h = 0;
  conv->GetSize(&w, &h);
  out.width = int(w);
  out.height = int(h);
  out.rgba.resize(size_t(w) * h * 4);
  if (FAILED(conv->CopyPixels(nullptr, w * 4, UINT(out.rgba.size()), out.rgba.data()))) {
    err = "decode failed (the HEVC Video Extension may be missing)";
    return false;
  }
  out.hasAlpha = false;
  for (size_t i = 3; i < out.rgba.size(); i += 4)
    if (out.rgba[i] != 255) { out.hasAlpha = true; break; }
  return true;
}

#elif defined(JOWE_CONVERT_OS_CODECS) && defined(__APPLE__)

bool osImageCanDecode(const std::string& format, std::string* reason) {
  if (format == "heic" || format == "avif") return true;
  if (reason) *reason = "No OS decoder for this format";
  return false;
}

bool osImageDecode(const Bytes& in, const std::string&, Image& out, std::string& err) {
  CFDataRef data = CFDataCreate(nullptr, in.data(), CFIndex(in.size()));
  CGImageSourceRef src = data ? CGImageSourceCreateWithData(data, nullptr) : nullptr;
  CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, nullptr) : nullptr;
  int orientation = 1;
  if (src) {
    if (CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(src, 0, nullptr)) {
      if (auto num = static_cast<CFNumberRef>(CFDictionaryGetValue(props, kCGImagePropertyOrientation)))
        CFNumberGetValue(num, kCFNumberIntType, &orientation);
      CFRelease(props);
    }
  }
  bool ok = false;
  if (img) {
    const size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
    out.width = int(w);
    out.height = int(h);
    out.rgba.assign(w * h * 4, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(out.rgba.data(), w, h, 8, w * 4, cs,
                                             kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    if (ctx) {
      CGContextDrawImage(ctx, CGRectMake(0, 0, CGFloat(w), CGFloat(h)), img);
      CGContextRelease(ctx);
      ok = true;
    }
    CGColorSpaceRelease(cs);
    CGImageRelease(img);
  }
  if (src) CFRelease(src);
  if (data) CFRelease(data);
  if (!ok) {
    err = "ImageIO could not decode this image";
    return false;
  }
  out.hasAlpha = false;
  for (size_t i = 0; i < out.rgba.size(); i += 4) {
    uint8_t a = out.rgba[i + 3];
    if (a != 255) {
      out.hasAlpha = true;
      if (a) for (int c = 0; c < 3; ++c) out.rgba[i + c] = uint8_t(std::min(255, out.rgba[i + c] * 255 / a));
    }
  }
  applyOrientation(out, orientation);
  return true;
}

#else

bool osImageCanDecode(const std::string& format, std::string* reason) {
  if (reason)
    *reason = format == "heic" ? "HEIC can't be read on this platform yet (no permissive HEVC decoder)"
                               : "No decoder for this format on this platform";
  return false;
}

bool osImageDecode(const Bytes&, const std::string& format, Image&, std::string& err) {
  osImageCanDecode(format, &err);
  return false;
}

#endif

}  // namespace jowe::convert::detail
