#include "gmf/json.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gmf {
namespace json {
namespace {

const Array kEmptyArray;
const Object kEmptyObject;

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  Value parse_document() {
    skip_whitespace();
    Value value = parse_value(0);
    skip_whitespace();
    if (pos_ != text_.size()) fail("unexpected trailing characters");
    return value;
  }

 private:
  [[noreturn]] void fail(const std::string& message) { throw ParseError(message, pos_); }

  bool done() const { return pos_ >= text_.size(); }
  char peek() const { return done() ? char(0) : text_[pos_]; }

  void skip_whitespace() {
    while (!done()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  void expect(char c) {
    if (peek() != c) fail(std::string("expected ") + c);
    ++pos_;
  }

  Value parse_value(int depth) {
    if (depth > 256) fail("nesting too deep");
    switch (peek()) {
      case '{':
        return parse_object(depth);
      case '[':
        return parse_array(depth);
      case '"':
        return Value(parse_string());
      case 't':
        return parse_literal("true", Value(true));
      case 'f':
        return parse_literal("false", Value(false));
      case 'n':
        return parse_literal("null", Value());
      case char(0):
        fail("unexpected end of input");
      default:
        return parse_number();
    }
  }

  Value parse_literal(const char* text, Value value) {
    const std::size_t len = std::strlen(text);
    if (text_.compare(pos_, len, text) != 0) fail("invalid literal");
    pos_ += len;
    return value;
  }

  Value parse_object(int depth) {
    expect('{');
    Object object;
    skip_whitespace();
    if (peek() == '}') {
      ++pos_;
      return Value(std::move(object));
    }
    while (true) {
      skip_whitespace();
      if (peek() != '"') fail("expected object key");
      std::string key = parse_string();
      skip_whitespace();
      expect(':');
      skip_whitespace();
      object.emplace_back(std::move(key), parse_value(depth + 1));
      skip_whitespace();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      expect('}');
      return Value(std::move(object));
    }
  }

  Value parse_array(int depth) {
    expect('[');
    Array array;
    skip_whitespace();
    if (peek() == ']') {
      ++pos_;
      return Value(std::move(array));
    }
    while (true) {
      skip_whitespace();
      array.push_back(parse_value(depth + 1));
      skip_whitespace();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      expect(']');
      return Value(std::move(array));
    }
  }

  static void append_utf8(std::string& out, unsigned int code_point) {
    if (code_point < 0x80) {
      out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
      out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
  }

  unsigned int parse_hex4() {
    if (pos_ + 4 > text_.size()) fail("truncated unicode escape");
    unsigned int value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<unsigned int>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<unsigned int>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<unsigned int>(c - 'A' + 10);
      } else {
        fail("invalid unicode escape");
      }
    }
    return value;
  }

  std::string parse_string() {
    expect('"');
    std::string out;
    while (true) {
      if (done()) fail("unterminated string");
      const char c = text_[pos_++];
      if (c == '"') return out;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (done()) fail("unterminated escape");
      const char esc = text_[pos_++];
      switch (esc) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          unsigned int code_point = parse_hex4();
          if (code_point >= 0xD800 && code_point <= 0xDBFF && pos_ + 1 < text_.size() &&
              text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
            const std::size_t saved = pos_;
            pos_ += 2;
            const unsigned int low = parse_hex4();
            if (low >= 0xDC00 && low <= 0xDFFF) {
              code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
            } else {
              pos_ = saved;
            }
          }
          append_utf8(out, code_point);
          break;
        }
        default:
          fail("invalid escape character");
      }
    }
  }

  Value parse_number() {
    const std::size_t start = pos_;
    if (peek() == '-' || peek() == '+') ++pos_;
    bool any_digits = false;
    while (!done() && text_[pos_] >= '0' && text_[pos_] <= '9') {
      ++pos_;
      any_digits = true;
    }
    if (!done() && text_[pos_] == '.') {
      ++pos_;
      while (!done() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
        any_digits = true;
      }
    }
    if (!any_digits) fail("invalid number");
    if (!done() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (!done() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
      bool exponent_digits = false;
      while (!done() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
        exponent_digits = true;
      }
      if (!exponent_digits) fail("invalid number exponent");
    }
    const std::string literal(text_.substr(start, pos_ - start));
    return Value(std::strtod(literal.c_str(), nullptr));
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

void escape_string(const std::string& in, std::string& out) {
  out.push_back('"');
  for (const char c : in) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
          out += buffer;
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

void dump_value(const Value& value, int indent, int depth, std::string& out) {
  const bool pretty = indent >= 0;
  const auto newline_indent = [&](int level) {
    if (!pretty) return;
    out.push_back('\n');
    out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(level), ' ');
  };

  switch (value.type()) {
    case Type::Null:
      out += "null";
      return;
    case Type::Boolean:
      out += value.as_bool() ? "true" : "false";
      return;
    case Type::Number:
      out += number_to_string(value.as_number());
      return;
    case Type::String:
      escape_string(value.as_string(), out);
      return;
    case Type::Array: {
      const Array& array = value.as_array();
      if (array.empty()) {
        out += "[]";
        return;
      }
      out.push_back('[');
      for (std::size_t i = 0; i < array.size(); ++i) {
        if (i) out.push_back(',');
        newline_indent(depth + 1);
        dump_value(array[i], indent, depth + 1, out);
      }
      newline_indent(depth);
      out.push_back(']');
      return;
    }
    case Type::Object: {
      const Object& object = value.as_object();
      if (object.empty()) {
        out += "{}";
        return;
      }
      out.push_back('{');
      for (std::size_t i = 0; i < object.size(); ++i) {
        if (i) out.push_back(',');
        newline_indent(depth + 1);
        escape_string(object[i].first, out);
        out.push_back(':');
        if (pretty) out.push_back(' ');
        dump_value(object[i].second, indent, depth + 1, out);
      }
      newline_indent(depth);
      out.push_back('}');
      return;
    }
  }
}

}  // namespace

const Value* Value::find(std::string_view key) const {
  if (type_ != Type::Object) return nullptr;
  for (const auto& entry : object_) {
    if (entry.first == key) return &entry.second;
  }
  return nullptr;
}

const Value* Value::find_any(std::initializer_list<std::string_view> keys) const {
  for (const std::string_view key : keys) {
    if (const Value* found = find(key)) return found;
  }
  return nullptr;
}

bool Value::as_bool(bool fallback) const {
  switch (type_) {
    case Type::Boolean:
      return bool_;
    case Type::Number:
      return number_ != 0.0;
    default:
      return fallback;
  }
}

double Value::as_number(double fallback) const {
  switch (type_) {
    case Type::Number:
      return number_;
    case Type::Boolean:
      return bool_ ? 1.0 : 0.0;
    case Type::String: {
      // Accept "inf" / "infinity" as well as numeric text, so a capacity may be
      // written either way.
      std::string lowered;
      lowered.reserve(string_.size());
      for (const char c : string_) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      }
      std::string_view body(lowered);
      bool negative = false;
      if (!body.empty() && (body.front() == '-' || body.front() == '+')) {
        negative = body.front() == '-';
        body.remove_prefix(1);
      }
      if (body == "inf" || body == "infinity") return negative ? -HUGE_VAL : HUGE_VAL;
      char* end = nullptr;
      const double parsed = std::strtod(string_.c_str(), &end);
      if (end == string_.c_str()) return fallback;
      return parsed;
    }
    default:
      return fallback;
  }
}

std::string Value::as_string(const std::string& fallback) const {
  switch (type_) {
    case Type::String:
      return string_;
    case Type::Number:
      return number_to_string(number_);
    case Type::Boolean:
      return bool_ ? "true" : "false";
    default:
      return fallback;
  }
}

const Array& Value::as_array() const { return type_ == Type::Array ? array_ : kEmptyArray; }

const Object& Value::as_object() const { return type_ == Type::Object ? object_ : kEmptyObject; }

void Value::set(std::string key, Value value) {
  if (type_ != Type::Object) {
    type_ = Type::Object;
    object_.clear();
  }
  for (auto& entry : object_) {
    if (entry.first == key) {
      entry.second = std::move(value);
      return;
    }
  }
  object_.emplace_back(std::move(key), std::move(value));
}

void Value::push_back(Value value) {
  if (type_ != Type::Array) {
    type_ = Type::Array;
    array_.clear();
  }
  array_.push_back(std::move(value));
}

Value parse(std::string_view text) { return Parser(text).parse_document(); }

std::string dump(const Value& value, int indent) {
  std::string out;
  dump_value(value, indent, 0, out);
  return out;
}

std::string number_to_string(double value) {
  if (std::isnan(value)) return "null";
  // 1e999 overflows to infinity in every JSON reader we care about (Python, JS),
  // which keeps infinite capacities round-tripping.
  if (std::isinf(value)) return value > 0 ? "1e999" : "-1e999";
  if (value == 0.0) return "0";
  if (value == std::floor(value) && std::fabs(value) < 1e15) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0f", value);
    return buffer;
  }
  char buffer[64];
  for (const int precision : {15, 16, 17}) {
    std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
    if (std::strtod(buffer, nullptr) == value) return buffer;
  }
  return buffer;
}

}  // namespace json
}  // namespace gmf
