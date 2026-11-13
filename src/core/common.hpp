#pragma once

#include <jowe/convert.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace jowe::convert::detail {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

std::string lower(std::string_view s);
std::string extensionOf(const fs::path& p);

struct Io {
  const AccessCheck* access = nullptr;

  bool canRead(const fs::path& p) const;
  bool canWrite(const fs::path& p) const;
  Result readFile(const fs::path& p, Bytes& out) const;
  Result writeFile(const fs::path& p, const void* data, size_t size) const;
  Result writeFile(const fs::path& p, const Bytes& b) const { return writeFile(p, b.data(), b.size()); }
};

struct Context {
  Io io;
  const ConvertOptions* options = nullptr;
  std::vector<std::string>* warnings = nullptr;
  std::vector<fs::path>* written = nullptr;
  ProbeInfo* input = nullptr;

  bool cancelled() const {
    return options && options->cancel && options->cancel->load(std::memory_order_relaxed);
  }
  void progress(double v) const {
    if (options && options->progress) options->progress(v < 0 ? 0 : (v > 1 ? 1 : v));
  }
  void warn(std::string w) const {
    if (warnings) warnings->push_back(std::move(w));
  }
};

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0);

class ByteWriter {
 public:
  Bytes data;
  void u8(uint8_t v) { data.push_back(v); }
  void be16(uint32_t v) { u8(uint8_t(v >> 8)); u8(uint8_t(v)); }
  void be24(uint32_t v) { u8(uint8_t(v >> 16)); be16(v); }
  void be32(uint32_t v) { be16(v >> 16); be16(v & 0xffff); }
  void be64(uint64_t v) { be32(uint32_t(v >> 32)); be32(uint32_t(v)); }
  void le16(uint32_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
  void le32(uint32_t v) { le16(v & 0xffff); le16(v >> 16); }
  void le64(uint64_t v) { le32(uint32_t(v)); le32(uint32_t(v >> 32)); }
  void fourcc(const char* s) { bytes(s, 4); }
  void bytes(const void* p, size_t n) {
    auto b = static_cast<const uint8_t*>(p);
    data.insert(data.end(), b, b + n);
  }
  void bytes(const Bytes& b) { data.insert(data.end(), b.begin(), b.end()); }
  void str(std::string_view s) { bytes(s.data(), s.size()); }
  void zeros(size_t n) { data.insert(data.end(), n, 0); }
  size_t size() const { return data.size(); }
  void patchBe32(size_t at, uint32_t v) {
    data[at] = uint8_t(v >> 24); data[at + 1] = uint8_t(v >> 16);
    data[at + 2] = uint8_t(v >> 8); data[at + 3] = uint8_t(v);
  }
  void patchLe32(size_t at, uint32_t v) {
    data[at] = uint8_t(v); data[at + 1] = uint8_t(v >> 8);
    data[at + 2] = uint8_t(v >> 16); data[at + 3] = uint8_t(v >> 24);
  }
};

class ByteReader {
 public:
  ByteReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  explicit ByteReader(const Bytes& b) : p_(b.data()), n_(b.size()) {}
  size_t pos() const { return pos_; }
  size_t size() const { return n_; }
  size_t remaining() const { return pos_ <= n_ ? n_ - pos_ : 0; }
  bool ok() const { return ok_; }
  void seek(size_t at) { if (at > n_) { ok_ = false; pos_ = n_; } else pos_ = at; }
  void skip(size_t n) { seek(pos_ + n); }
  const uint8_t* ptr() const { return p_ + pos_; }
  const uint8_t* base() const { return p_; }
  uint8_t u8() { return need(1) ? p_[pos_++] : 0; }
  uint16_t be16() { if (!need(2)) return 0; uint16_t v = uint16_t(p_[pos_] << 8 | p_[pos_ + 1]); pos_ += 2; return v; }
  uint32_t be24() { uint32_t hi = u8(); return hi << 16 | be16(); }
  uint32_t be32() { uint32_t hi = be16(); return hi << 16 | be16(); }
  uint64_t be64() { uint64_t hi = be32(); return hi << 32 | be32(); }
  uint16_t le16() { if (!need(2)) return 0; uint16_t v = uint16_t(p_[pos_] | p_[pos_ + 1] << 8); pos_ += 2; return v; }
  uint32_t le32() { uint32_t lo = le16(); return lo | uint32_t(le16()) << 16; }
  uint64_t le64() { uint64_t lo = le32(); return lo | uint64_t(le32()) << 32; }
  std::string fourcc() { if (!need(4)) return {}; std::string s(reinterpret_cast<const char*>(p_ + pos_), 4); pos_ += 4; return s; }
  bool read(void* dst, size_t n) { if (!need(n)) return false; std::memcpy(dst, p_ + pos_, n); pos_ += n; return true; }

 private:
  bool need(size_t n) { if (!ok_ || n > n_ - pos_) { ok_ = false; return false; } return true; }
  const uint8_t* p_;
  size_t n_;
  size_t pos_ = 0;
  bool ok_ = true;
};

// Writes go to a sibling temp file that is renamed over the target on commit and removed otherwise.
class PendingFile {
 public:
  PendingFile(const Io& io, fs::path target);
  ~PendingFile();
  const fs::path& tempPath() const { return temp_; }
  const fs::path& target() const { return target_; }
  Result commit();

 private:
  fs::path target_;
  fs::path temp_;
  bool done_ = false;
};

}  // namespace jowe::convert::detail
