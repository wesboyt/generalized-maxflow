// Minimal dependency-free JSON reader/writer for the generalized max-flow library.
#pragma once

#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gmf {
namespace json {

class Value;

using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

enum class Type { Null, Boolean, Number, String, Array, Object };

class ParseError : public std::runtime_error {
 public:
  ParseError(const std::string& message, std::size_t offset)
      : std::runtime_error(message + " (at offset " + std::to_string(offset) + ")"),
        offset(offset) {}
  std::size_t offset;
};

// An order-preserving JSON value. Object lookup is linear, which is the right
// trade-off here: the documents this library reads have a handful of keys per
// object, and keeping insertion order makes the emitted JSON stable.
class Value {
 public:
  Value() = default;
  Value(bool b) : type_(Type::Boolean), bool_(b) {}
  Value(double d) : type_(Type::Number), number_(d) {}
  Value(int i) : type_(Type::Number), number_(static_cast<double>(i)) {}
  Value(std::size_t i) : type_(Type::Number), number_(static_cast<double>(i)) {}
  Value(const char* s) : type_(Type::String), string_(s) {}
  Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
  Value(Array a) : type_(Type::Array), array_(std::move(a)) {}
  Value(Object o) : type_(Type::Object), object_(std::move(o)) {}

  static Value array() { return Value(Array{}); }
  static Value object() { return Value(Object{}); }

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Boolean; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  // Returns nullptr when this is not an object or the key is absent.
  const Value* find(std::string_view key) const;
  // First present key wins; lets callers accept aliases such as capacity/cap.
  const Value* find_any(std::initializer_list<std::string_view> keys) const;

  bool as_bool(bool fallback = false) const;
  double as_number(double fallback = 0.0) const;
  std::string as_string(const std::string& fallback = std::string()) const;
  const Array& as_array() const;
  const Object& as_object() const;

  void set(std::string key, Value value);
  void push_back(Value value);

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  Array array_;
  Object object_;
};

Value parse(std::string_view text);
std::string dump(const Value& value, int indent = -1);

// Shortest decimal form that round-trips; integral values print without a
// fractional part so that capacities read back the way they were written.
std::string number_to_string(double value);

}  // namespace json
}  // namespace gmf
