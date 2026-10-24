// gmaxflow: read a JSON graph, write the filled paths and the maximum flow.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "gmf/gmf.hpp"
#include "gmf/json.hpp"

namespace {

void print_usage() {
  std::cout << "gmaxflow " << gmf::version() << " - generalized maximum flow\n\n"
            << "usage:\n"
            << "  gmaxflow [options] [file.json]\n"
            << "  cat graph.json | gmaxflow\n\n"
            << "options:\n"
            << "  -o, --output FILE   write the answer to FILE instead of stdout\n"
            << "  -c, --compact       emit one line of JSON (default is indented)\n"
            << "  -h, --help          show this message\n"
            << "  -v, --version       show the version\n\n"
            << "input:\n"
            << "  {\"source\": \"s\", \"sink\": \"t\",\n"
            << "   \"adjacency\": {\"s\": [{\"to\": \"t\", \"capacity\": 10, \"multiplier\": 0.5}]}}\n";
}

// Injects the CLI switches into the request so the library sees them as options.
std::string apply_overrides(const std::string& request, bool compact) {
  gmf::json::Value document;
  try {
    document = gmf::json::parse(request);
  } catch (const std::exception&) {
    return request;  // Let the library report the parse error.
  }
  if (!document.is_object()) return request;

  gmf::json::Value options = gmf::json::Value::object();
  if (const gmf::json::Value* existing = document.find("options")) options = *existing;
  if (options.find("pretty") == nullptr) {
    options.set("pretty", gmf::json::Value(!compact));
  } else if (compact) {
    options.set("pretty", gmf::json::Value(false));
  }
  document.set("options", std::move(options));
  return gmf::json::dump(document);
}

}  // namespace

int main(int argc, char** argv) {
  std::string input_path;
  std::string output_path;
  bool compact = false;

  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "-h" || argument == "--help") {
      print_usage();
      return 0;
    }
    if (argument == "-v" || argument == "--version") {
      std::cout << gmf::version() << "\n";
      return 0;
    }
    if (argument == "-c" || argument == "--compact") {
      compact = true;
      continue;
    }
    if (argument == "-o" || argument == "--output") {
      if (i + 1 >= argc) {
        std::cerr << "gmaxflow: " << argument << " needs a file name\n";
        return 2;
      }
      output_path = argv[++i];
      continue;
    }
    if (!argument.empty() && argument[0] == '-' && argument != "-") {
      std::cerr << "gmaxflow: unknown option " << argument << "\n";
      return 2;
    }
    if (!input_path.empty()) {
      std::cerr << "gmaxflow: more than one input file given\n";
      return 2;
    }
    input_path = argument;
  }

  std::string request;
  if (input_path.empty() || input_path == "-") {
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    request = buffer.str();
  } else {
    std::ifstream file(input_path, std::ios::binary);
    if (!file) {
      std::cerr << "gmaxflow: cannot read " << input_path << "\n";
      return 2;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    request = buffer.str();
  }

  if (request.find_first_not_of(" \t\r\n") == std::string::npos) {
    std::cerr << "gmaxflow: no input. Pass a file or pipe JSON on stdin; -h for help.\n";
    return 2;
  }

  const std::string answer =
      gmf::solve_json(apply_overrides(request, compact));

  if (output_path.empty()) {
    std::cout << answer << "\n";
  } else {
    std::ofstream file(output_path, std::ios::binary);
    if (!file) {
      std::cerr << "gmaxflow: cannot write " << output_path << "\n";
      return 2;
    }
    file << answer << "\n";
  }

  // A non-zero exit status lets scripts notice a rejected graph.
  try {
    const gmf::json::Value parsed = gmf::json::parse(answer);
    const gmf::json::Value* status = parsed.find("status");
    if (status != nullptr && status->as_string() == "invalid_input") return 1;
  } catch (const std::exception&) {
    return 1;
  }
  return 0;
}
