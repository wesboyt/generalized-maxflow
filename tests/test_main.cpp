#include "harness.hpp"

#include <iostream>

namespace test {

std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

int run_all(const std::string& filter) {
  int passed = 0;
  std::vector<std::string> failures;
  for (const Case& entry : registry()) {
    if (!filter.empty() && entry.name.find(filter) == std::string::npos) continue;
    try {
      entry.body();
      ++passed;
      std::cout << "  ok   " << entry.name << "\n";
    } catch (const std::exception& error) {
      failures.push_back(entry.name + "\n       " + error.what());
      std::cout << "  FAIL " << entry.name << "\n       " << error.what() << "\n";
    }
  }
  std::cout << "\n" << passed << " passed, " << failures.size() << " failed\n";
  return failures.empty() ? 0 : 1;
}

}  // namespace test

int main(int argc, char** argv) {
  const std::string filter = argc > 1 ? argv[1] : "";
  return test::run_all(filter);
}
