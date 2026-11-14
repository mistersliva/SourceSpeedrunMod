// Copyright Source Authors. All rights reserved.
//
// Minimal JSON reader/writer used by the HTML GameUI bridge.

#ifndef SE_GAMEUI_JSON_H_
#define SE_GAMEUI_JSON_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace json {

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Value() = default;
  static Value MakeBool(bool b);
  static Value MakeNumber(double d);
  static Value MakeString(std::string s);
  static Value MakeArray();
  static Value MakeObject();

  Type type() const { return type_; }
  bool IsNull() const { return type_ == Type::Null; }
  bool IsString() const { return type_ == Type::String; }
  bool IsNumber() const { return type_ == Type::Number; }
  bool IsBool() const { return type_ == Type::Bool; }
  bool IsArray() const { return type_ == Type::Array; }
  bool IsObject() const { return type_ == Type::Object; }

  bool AsBool(bool def = false) const;
  double AsNumber(double def = 0) const;
  int AsInt(int def = 0) const { return static_cast<int>(AsNumber(def)); }
  // Strings are returned as is; numbers/bools are stringified.
  std::string AsString(const char *def = "") const;

  // Object access; returns a Null value when missing.
  const Value &operator[](const char *key) const;
  bool Has(const char *key) const;
  const std::map<std::string, Value> &Members() const { return object_; }

  // Array access.
  size_t Size() const { return array_.size(); }
  const Value &operator[](size_t i) const;
  const std::vector<Value> &Items() const { return array_; }

  void Push(Value v) { array_.push_back(std::move(v)); }
  void Set(const std::string &key, Value v) { object_[key] = std::move(v); }

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double number_ = 0;
  std::string string_;
  std::vector<Value> array_;
  std::map<std::string, Value> object_;
};

// Parses |text|; on failure returns false and leaves |out| Null.
bool Parse(const char *text, size_t length, Value &out);

// Streaming writer producing compact JSON.
class Writer {
 public:
  Writer &BeginObject();
  Writer &EndObject();
  Writer &BeginArray();
  Writer &EndArray();
  Writer &Key(const char *key);

  Writer &String(const char *utf8);
  Writer &String(const std::string &s) { return String(s.c_str()); }
  Writer &WString(const wchar_t *text);
  Writer &Number(double d);
  Writer &Int(long long i);
  Writer &Bool(bool b);
  Writer &Null();
  // Inserts already-serialized JSON.
  Writer &Raw(const char *json);

  // Convenience: key + value.
  Writer &Prop(const char *key, const char *v) { return Key(key).String(v); }
  Writer &Prop(const char *key, const std::string &v) { return Key(key).String(v); }
  Writer &Prop(const char *key, double v) { return Key(key).Number(v); }
  Writer &Prop(const char *key, int v) { return Key(key).Int(v); }
  Writer &Prop(const char *key, long long v) { return Key(key).Int(v); }
  Writer &Prop(const char *key, bool v) { return Key(key).Bool(v); }

  const std::string &str() const { return out_; }
  std::string &&take() { return std::move(out_); }

 private:
  void Separator();

  std::string out_;
  // true = needs a comma before next value at this nesting level.
  std::vector<bool> stack_;
  bool after_key_ = false;
};

// Escapes |utf8| as a JSON string literal (with quotes).
std::string Quote(const char *utf8);

}  // namespace json

#endif  // SE_GAMEUI_JSON_H_
