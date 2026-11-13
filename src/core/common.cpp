#include "common.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <random>

namespace jowe::convert::detail {

std::string lower(std::string_view s) {
  std::string r(s);
  std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return r;
}

std::string extensionOf(const fs::path& p) {
  std::string e = p.extension().string();
  if (!e.empty() && e[0] == '.') e.erase(0, 1);
  return lower(e);
}

bool Io::canRead(const fs::path& p) const { return !access || !*access || (*access)(p, false); }
bool Io::canWrite(const fs::path& p) const { return !access || !*access || (*access)(p, true); }

Result Io::readFile(const fs::path& p, Bytes& out) const {
  if (!canRead(p)) return Result::error(Status::AccessDenied, "Access denied: " + p.string());
  std::ifstream f(p, std::ios::binary);
  if (!f) return Result::error(Status::Unreadable, "Cannot open " + p.string());
  f.seekg(0, std::ios::end);
  auto n = f.tellg();
  if (n < 0) return Result::error(Status::Unreadable, "Cannot read " + p.string());
  f.seekg(0);
  out.resize(size_t(n));
  if (n > 0 && !f.read(reinterpret_cast<char*>(out.data()), n))
    return Result::error(Status::Unreadable, "Cannot read " + p.string());
  return Result::ok();
}

Result Io::writeFile(const fs::path& p, const void* data, size_t size) const {
  if (!canWrite(p)) return Result::error(Status::AccessDenied, "Access denied: " + p.string());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) return Result::error(Status::Failed, "Cannot create " + p.string());
  if (size && !f.write(static_cast<const char*>(data), std::streamsize(size)))
    return Result::error(Status::Failed, "Cannot write " + p.string());
  f.close();
  if (!f) return Result::error(Status::Failed, "Cannot write " + p.string());
  return Result::ok();
}

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
  return ~crc;
}

PendingFile::PendingFile(const Io&, fs::path target) : target_(std::move(target)) {
  std::random_device rd;
  temp_ = target_;
  temp_ += ".jvpart" + std::to_string(rd() % 1000000);
}

PendingFile::~PendingFile() {
  if (!done_) {
    std::error_code ec;
    fs::remove(temp_, ec);
  }
}

Result PendingFile::commit() {
  std::error_code ec;
  fs::rename(temp_, target_, ec);
  if (ec) {
    fs::remove(target_, ec);
    ec.clear();
    fs::rename(temp_, target_, ec);
  }
  if (ec) return Result::error(Status::Failed, "Cannot move the output into place: " + ec.message());
  done_ = true;
  return Result::ok();
}

}  // namespace jowe::convert::detail
