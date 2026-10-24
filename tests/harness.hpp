// A small test harness: enough to register cases, check values and report.
#pragma once

#include <cmath>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace test {

struct Case {
  std::string name;
  std::function<void()> body;
};

std::vector<Case>& registry();

struct Register {
  Register(std::string name, std::function<void()> body) {
    registry().push_back({std::move(name), std::move(body)});
  }
};

class Failure : public std::runtime_error {
 public:
  explicit Failure(const std::string& message) : std::runtime_error(message) {}
};

[[noreturn]] inline void fail(const std::string& message, const char* file, int line) {
  std::ostringstream out;
  out << file << ":" << line << ": " << message;
  throw Failure(out.str());
}

inline bool near(double lhs, double rhs, double tolerance) {
  if (std::isinf(lhs) || std::isinf(rhs)) return lhs == rhs;
  return std::fabs(lhs - rhs) <= tolerance * (1.0 + std::fabs(lhs) + std::fabs(rhs));
}

int run_all(const std::string& filter);

}  // namespace test

#define TEST(name)                                                   \
  static void name();                                                \
  static ::test::Register register_##name(#name, name);              \
  static void name()

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) ::test::fail("CHECK failed: " #condition, __FILE__, __LINE__); \
  } while (false)

#define CHECK_MESSAGE(condition, message)                                             \
  do {                                                                                \
    if (!(condition)) {                                                               \
      std::ostringstream check_stream;                                                \
      check_stream << "CHECK failed: " #condition " -- " << message;                  \
      ::test::fail(check_stream.str(), __FILE__, __LINE__);                           \
    }                                                                                 \
  } while (false)

#define CHECK_NEAR(lhs, rhs, tolerance)                                               \
  do {                                                                                \
    const double check_lhs = (lhs);                                                   \
    const double check_rhs = (rhs);                                                   \
    if (!::test::near(check_lhs, check_rhs, (tolerance))) {                           \
      std::ostringstream check_stream;                                                \
      check_stream << "CHECK_NEAR failed: " #lhs " = " << check_lhs                   \
                   << ", " #rhs " = " << check_rhs;                                   \
      ::test::fail(check_stream.str(), __FILE__, __LINE__);                           \
    }                                                                                 \
  } while (false)
