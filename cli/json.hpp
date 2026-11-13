#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace jvc {

class Json {
 public:
  using Array = std::vector<Json>;
  using Object = std::vector<std::pair<std::string, Json>>;

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool b) : v_(b) {}
  Json(int n) : v_(double(n)) {}
  Json(int64_t n) : v_(double(n)) {}
  Json(uint64_t n) : v_(double(n)) {}
  Json(double n) : v_(n) {}
  Json(const char* s) : v_(std::string(s)) {}
  Json(std::string s) : v_(std::move(s)) {}
  Json(Array a) : v_(std::make_shared<Array>(std::move(a))) {}
  Json(Object o) : v_(std::make_shared<Object>(std::move(o))) {}

  static Json array() { return Json(Array{}); }
  static Json object() { return Json(Object{}); }

  bool isNull() const { return v_.index() == 0; }
  bool isBool() const { return v_.index() == 1; }
  bool isNumber() const { return v_.index() == 2; }
  bool isString() const { return v_.index() == 3; }
  bool isArray() const { return v_.index() == 4; }
  bool isObject() const { return v_.index() == 5; }

  bool asBool(bool def = false) const { return isBool() ? std::get<bool>(v_) : def; }
  double asNumber(double def = 0) const { return isNumber() ? std::get<double>(v_) : def; }
  const std::string& asString() const {
    static const std::string empty;
    return isString() ? std::get<std::string>(v_) : empty;
  }
  const Array& items() const {
    static const Array empty;
    return isArray() ? *std::get<4>(v_) : empty;
  }
  const Object& members() const {
    static const Object empty;
    return isObject() ? *std::get<5>(v_) : empty;
  }

  const Json* find(const std::string& key) const {
    if (!isObject()) return nullptr;
    for (auto& [k, v] : *std::get<5>(v_))
      if (k == key) return &v;
    return nullptr;
  }
  const Json& operator[](const std::string& key) const {
    static const Json null;
    const Json* j = find(key);
    return j ? *j : null;
  }
  Json& set(const std::string& key, Json value) {
    if (!isObject()) v_ = std::make_shared<Object>();
    auto& o = *std::get<5>(v_);
    for (auto& [k, v] : o)
      if (k == key) {
        v = std::move(value);
        return *this;
      }
    o.emplace_back(key, std::move(value));
    return *this;
  }
  Json& push(Json value) {
    if (!isArray()) v_ = std::make_shared<Array>();
    std::get<4>(v_)->push_back(std::move(value));
    return *this;
  }

  std::string dump(int indent = -1) const {
    std::string out;
    write(out, indent, 0);
    return out;
  }

  static Json parse(const std::string& text) {
    size_t i = 0;
    Json j = parseValue(text, i, 0);
    skipWs(text, i);
    if (i != text.size()) throw std::runtime_error("trailing characters");
    return j;
  }

 private:
  std::variant<std::nullptr_t, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> v_;

  static void escape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
          if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
          } else {
            out += char(c);
          }
      }
    }
    out += '"';
  }

  void write(std::string& out, int indent, int depth) const {
    auto nl = [&](int d) {
      if (indent < 0) return;
      out += '\n';
      out.append(size_t(indent * d), ' ');
    };
    switch (v_.index()) {
      case 0: out += "null"; break;
      case 1: out += std::get<bool>(v_) ? "true" : "false"; break;
      case 2: {
        double d = std::get<double>(v_);
        if (!std::isfinite(d)) { out += "null"; break; }
        char buf[32];
        if (d == std::floor(d) && std::fabs(d) < 9.0e15) std::snprintf(buf, sizeof buf, "%.0f", d);
        else std::snprintf(buf, sizeof buf, "%.10g", d);
        out += buf;
        break;
      }
      case 3: escape(out, std::get<std::string>(v_)); break;
      case 4: {
        const auto& a = *std::get<4>(v_);
        out += '[';
        for (size_t i = 0; i < a.size(); ++i) {
          if (i) out += ',';
          nl(depth + 1);
          a[i].write(out, indent, depth + 1);
        }
        if (!a.empty()) nl(depth);
        out += ']';
        break;
      }
      case 5: {
        const auto& o = *std::get<5>(v_);
        out += '{';
        for (size_t i = 0; i < o.size(); ++i) {
          if (i) out += ',';
          nl(depth + 1);
          escape(out, o[i].first);
          out += indent < 0 ? ":" : ": ";
          o[i].second.write(out, indent, depth + 1);
        }
        if (!o.empty()) nl(depth);
        out += '}';
        break;
      }
    }
  }

  static void skipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  }

  static void utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
  }

  static uint32_t hex4(const std::string& s, size_t& i) {
    if (i + 4 > s.size()) throw std::runtime_error("bad escape");
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) {
      char c = s[i++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
      else throw std::runtime_error("bad escape");
    }
    return v;
  }

  static std::string parseString(const std::string& s, size_t& i) {
    if (s[i] != '"') throw std::runtime_error("expected string");
    ++i;
    std::string out;
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= s.size()) break;
      char e = s[i++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t cp = hex4(s, i);
          if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
            i += 2;
            uint32_t lo = hex4(s, i);
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(out, cp);
          break;
        }
        default: throw std::runtime_error("bad escape");
      }
    }
    if (i >= s.size()) throw std::runtime_error("unterminated string");
    ++i;
    return out;
  }

  static Json parseValue(const std::string& s, size_t& i, int depth) {
    if (depth > 64) throw std::runtime_error("nesting too deep");
    skipWs(s, i);
    if (i >= s.size()) throw std::runtime_error("unexpected end");
    char c = s[i];
    if (c == '{') {
      ++i;
      Object o;
      skipWs(s, i);
      if (i < s.size() && s[i] == '}') { ++i; return Json(std::move(o)); }
      for (;;) {
        skipWs(s, i);
        std::string k = parseString(s, i);
        skipWs(s, i);
        if (i >= s.size() || s[i] != ':') throw std::runtime_error("expected ':'");
        ++i;
        o.emplace_back(std::move(k), parseValue(s, i, depth + 1));
        skipWs(s, i);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') { ++i; break; }
        throw std::runtime_error("expected ',' or '}'");
      }
      return Json(std::move(o));
    }
    if (c == '[') {
      ++i;
      Array a;
      skipWs(s, i);
      if (i < s.size() && s[i] == ']') { ++i; return Json(std::move(a)); }
      for (;;) {
        a.push_back(parseValue(s, i, depth + 1));
        skipWs(s, i);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == ']') { ++i; break; }
        throw std::runtime_error("expected ',' or ']'");
      }
      return Json(std::move(a));
    }
    if (c == '"') return Json(parseString(s, i));
    if (s.compare(i, 4, "true") == 0) { i += 4; return Json(true); }
    if (s.compare(i, 5, "false") == 0) { i += 5; return Json(false); }
    if (s.compare(i, 4, "null") == 0) { i += 4; return Json(); }
    size_t start = i;
    if (s[i] == '-') ++i;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == 'e' ||
                            s[i] == 'E' || s[i] == '+' || s[i] == '-'))
      ++i;
    if (start == i) throw std::runtime_error("unexpected character");
    try {
      return Json(std::stod(s.substr(start, i - start)));
    } catch (...) {
      throw std::runtime_error("bad number");
    }
  }
};

}  // namespace jvc
