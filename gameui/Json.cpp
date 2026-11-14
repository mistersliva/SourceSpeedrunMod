// Copyright Source Authors. All rights reserved.

#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace json {
namespace {

const Value kNull;

class Parser {
 public:
  Parser(const char *p, size_t n) : p_(p), end_(p + n) {}

  bool ParseRoot(Value &out) {
    SkipWs();
    if (!ParseValue(out, 0)) return false;
    SkipWs();
    return p_ == end_;
  }

 private:
  static constexpr int kMaxDepth = 64;

  void SkipWs() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
  }

  bool Match(const char *lit) {
    const size_t n = strlen(lit);
    if (static_cast<size_t>(end_ - p_) < n || memcmp(p_, lit, n) != 0) return false;
    p_ += n;
    return true;
  }

  bool ParseValue(Value &out, int depth) {
    if (depth > kMaxDepth || p_ >= end_) return false;
    switch (*p_) {
      case '{': return ParseObject(out, depth);
      case '[': return ParseArray(out, depth);
      case '"': {
        std::string s;
        if (!ParseString(s)) return false;
        out = Value::MakeString(std::move(s));
        return true;
      }
      case 't': if (!Match("true")) return false; out = Value::MakeBool(true); return true;
      case 'f': if (!Match("false")) return false; out = Value::MakeBool(false); return true;
      case 'n': if (!Match("null")) return false; out = Value(); return true;
      default: return ParseNumber(out);
    }
  }

  bool ParseNumber(Value &out) {
    const char *start = p_;
    if (p_ < end_ && (*p_ == '-' || *p_ == '+')) ++p_;
    while (p_ < end_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' ||
                         *p_ == 'E' || *p_ == '-' || *p_ == '+'))
      ++p_;
    if (p_ == start) return false;
    std::string tmp(start, p_);
    char *endp = nullptr;
    const double d = strtod(tmp.c_str(), &endp);
    if (!endp || *endp) return false;
    out = Value::MakeNumber(d);
    return true;
  }

  static void AppendUtf8(std::string &s, unsigned cp) {
    if (cp < 0x80) {
      s += static_cast<char>(cp);
    } else if (cp < 0x800) {
      s += static_cast<char>(0xC0 | (cp >> 6));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      s += static_cast<char>(0xE0 | (cp >> 12));
      s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      s += static_cast<char>(0xF0 | (cp >> 18));
      s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  bool ParseHex4(unsigned &cp) {
    if (end_ - p_ < 4) return false;
    cp = 0;
    for (int i = 0; i < 4; ++i, ++p_) {
      const char c = *p_;
      cp <<= 4;
      if (c >= '0' && c <= '9') cp |= c - '0';
      else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
      else return false;
    }
    return true;
  }

  bool ParseString(std::string &s) {
    ++p_;  // opening quote
    while (p_ < end_) {
      const char c = *p_++;
      if (c == '"') return true;
      if (c != '\\') {
        s += c;
        continue;
      }
      if (p_ >= end_) return false;
      const char e = *p_++;
      switch (e) {
        case '"': s += '"'; break;
        case '\\': s += '\\'; break;
        case '/': s += '/'; break;
        case 'b': s += '\b'; break;
        case 'f': s += '\f'; break;
        case 'n': s += '\n'; break;
        case 'r': s += '\r'; break;
        case 't': s += '\t'; break;
        case 'u': {
          unsigned cp;
          if (!ParseHex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
            p_ += 2;
            unsigned lo;
            if (!ParseHex4(lo)) return false;
            if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          AppendUtf8(s, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool ParseArray(Value &out, int depth) {
    ++p_;
    out = Value::MakeArray();
    SkipWs();
    if (p_ < end_ && *p_ == ']') { ++p_; return true; }
    for (;;) {
      SkipWs();
      Value v;
      if (!ParseValue(v, depth + 1)) return false;
      out.Push(std::move(v));
      SkipWs();
      if (p_ >= end_) return false;
      if (*p_ == ',') { ++p_; continue; }
      if (*p_ == ']') { ++p_; return true; }
      return false;
    }
  }

  bool ParseObject(Value &out, int depth) {
    ++p_;
    out = Value::MakeObject();
    SkipWs();
    if (p_ < end_ && *p_ == '}') { ++p_; return true; }
    for (;;) {
      SkipWs();
      if (p_ >= end_ || *p_ != '"') return false;
      std::string key;
      if (!ParseString(key)) return false;
      SkipWs();
      if (p_ >= end_ || *p_ != ':') return false;
      ++p_;
      SkipWs();
      Value v;
      if (!ParseValue(v, depth + 1)) return false;
      out.Set(key, std::move(v));
      SkipWs();
      if (p_ >= end_) return false;
      if (*p_ == ',') { ++p_; continue; }
      if (*p_ == '}') { ++p_; return true; }
      return false;
    }
  }

  const char *p_;
  const char *end_;
};

void AppendEscaped(std::string &out, const char *s) {
  out += '"';
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s ? s : ""); *p; ++p) {
    const unsigned char c = *p;
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
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

}  // namespace

Value Value::MakeBool(bool b) { Value v; v.type_ = Type::Bool; v.bool_ = b; return v; }
Value Value::MakeNumber(double d) { Value v; v.type_ = Type::Number; v.number_ = d; return v; }
Value Value::MakeString(std::string s) { Value v; v.type_ = Type::String; v.string_ = std::move(s); return v; }
Value Value::MakeArray() { Value v; v.type_ = Type::Array; return v; }
Value Value::MakeObject() { Value v; v.type_ = Type::Object; return v; }

bool Value::AsBool(bool def) const {
  switch (type_) {
    case Type::Bool: return bool_;
    case Type::Number: return number_ != 0;
    case Type::String: return string_ == "1" || string_ == "true";
    default: return def;
  }
}

double Value::AsNumber(double def) const {
  switch (type_) {
    case Type::Number: return number_;
    case Type::Bool: return bool_ ? 1 : 0;
    case Type::String: return string_.empty() ? def : atof(string_.c_str());
    default: return def;
  }
}

std::string Value::AsString(const char *def) const {
  switch (type_) {
    case Type::String: return string_;
    case Type::Bool: return bool_ ? "1" : "0";
    case Type::Number: {
      char buf[64];
      if (std::floor(number_) == number_ && std::fabs(number_) < 1e15)
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(number_));
      else
        snprintf(buf, sizeof(buf), "%g", number_);
      return buf;
    }
    default: return def ? def : "";
  }
}

const Value &Value::operator[](const char *key) const {
  if (type_ != Type::Object) return kNull;
  auto it = object_.find(key);
  return it == object_.end() ? kNull : it->second;
}

bool Value::Has(const char *key) const {
  return type_ == Type::Object && object_.find(key) != object_.end();
}

const Value &Value::operator[](size_t i) const {
  return (type_ == Type::Array && i < array_.size()) ? array_[i] : kNull;
}

bool Parse(const char *text, size_t length, Value &out) {
  out = Value();
  if (!text) return false;
  Parser p(text, length);
  Value v;
  if (!p.ParseRoot(v)) return false;
  out = std::move(v);
  return true;
}

std::string Quote(const char *utf8) {
  std::string s;
  AppendEscaped(s, utf8);
  return s;
}

void Writer::Separator() {
  if (after_key_) {
    after_key_ = false;
    return;
  }
  if (!stack_.empty()) {
    if (stack_.back()) out_ += ',';
    stack_.back() = true;
  }
}

Writer &Writer::BeginObject() { Separator(); out_ += '{'; stack_.push_back(false); return *this; }
Writer &Writer::EndObject() { out_ += '}'; stack_.pop_back(); return *this; }
Writer &Writer::BeginArray() { Separator(); out_ += '['; stack_.push_back(false); return *this; }
Writer &Writer::EndArray() { out_ += ']'; stack_.pop_back(); return *this; }

Writer &Writer::Key(const char *key) {
  Separator();
  AppendEscaped(out_, key);
  out_ += ':';
  after_key_ = true;
  return *this;
}

Writer &Writer::String(const char *utf8) { Separator(); AppendEscaped(out_, utf8); return *this; }

Writer &Writer::WString(const wchar_t *text) {
  std::string utf8;
  if (text) {
    for (const wchar_t *p = text; *p; ++p) {
      unsigned cp = static_cast<unsigned>(*p);
      if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && p[1] >= 0xDC00 && p[1] <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<unsigned>(p[1]) - 0xDC00);
        ++p;
      }
      if (cp < 0x80) utf8 += static_cast<char>(cp);
      else if (cp < 0x800) { utf8 += static_cast<char>(0xC0 | (cp >> 6)); utf8 += static_cast<char>(0x80 | (cp & 0x3F)); }
      else if (cp < 0x10000) { utf8 += static_cast<char>(0xE0 | (cp >> 12)); utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); utf8 += static_cast<char>(0x80 | (cp & 0x3F)); }
      else { utf8 += static_cast<char>(0xF0 | (cp >> 18)); utf8 += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); utf8 += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
  }
  return String(utf8.c_str());
}

Writer &Writer::Number(double d) {
  Separator();
  if (!std::isfinite(d)) { out_ += "0"; return *this; }
  char buf[64];
  if (std::floor(d) == d && std::fabs(d) < 1e15)
    snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
  else
    snprintf(buf, sizeof(buf), "%.9g", d);
  out_ += buf;
  return *this;
}

Writer &Writer::Int(long long i) {
  Separator();
  out_ += std::to_string(i);
  return *this;
}

Writer &Writer::Bool(bool b) { Separator(); out_ += b ? "true" : "false"; return *this; }
Writer &Writer::Null() { Separator(); out_ += "null"; return *this; }
Writer &Writer::Raw(const char *json) { Separator(); out_ += json ? json : "null"; return *this; }

}  // namespace json
