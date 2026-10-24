// The plain C ABI: the JSON interface, behind three functions with no C++ or
// Python types at the boundary. The Python package loads it with ctypes and the
// WebAssembly build exports it, so neither links against an interpreter -- one
// wheel per OS and CPU serves every Python version.
//
// Strings cross the boundary as UTF-8. The caller owns what gmf_solve_json
// returns and frees it with gmf_free; a null return means memory ran out.
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include "gmf/gmf.hpp"

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define GMF_EXPORT EMSCRIPTEN_KEEPALIVE
#elif defined(_WIN32)
#define GMF_EXPORT __declspec(dllexport)
#else
#define GMF_EXPORT __attribute__((visibility("default")))
#endif

namespace {

char* duplicate(const std::string& text, std::size_t* size) {
  char* buffer = static_cast<char*>(std::malloc(text.size() + 1));
  if (buffer == nullptr) return nullptr;
  std::memcpy(buffer, text.c_str(), text.size() + 1);
  if (size != nullptr) *size = text.size();
  return buffer;
}

char* solve(const std::string& request, std::size_t* size) {
  try {
    return duplicate(gmf::solve_json(request), size);
  } catch (const std::bad_alloc&) {
    return nullptr;
  } catch (...) {
    return duplicate("{\"status\":\"invalid_input\",\"error\":\"unknown error\"}", size);
  }
}

}  // namespace

extern "C" {

// A NUL-terminated request.
GMF_EXPORT char* gmf_solve_json(const char* request) {
  if (request == nullptr) {
    return duplicate("{\"status\":\"invalid_input\",\"error\":\"no request\"}", nullptr);
  }
  return solve(std::string(request), nullptr);
}

// A request of known length, which may hold NUL bytes; the answer's length is
// written to *answer_size when it is not null.
GMF_EXPORT char* gmf_solve_json_n(const char* request, std::size_t request_size,
                                  std::size_t* answer_size) {
  if (request == nullptr && request_size > 0) {
    return duplicate("{\"status\":\"invalid_input\",\"error\":\"no request\"}", answer_size);
  }
  return solve(std::string(request == nullptr ? "" : request, request_size), answer_size);
}

GMF_EXPORT void gmf_free(char* buffer) { std::free(buffer); }

GMF_EXPORT const char* gmf_version() { return gmf::version(); }

}  // extern "C"
