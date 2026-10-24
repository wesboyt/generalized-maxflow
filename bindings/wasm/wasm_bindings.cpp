// WebAssembly entry points. Strings cross the boundary as UTF-8 C strings; the
// caller owns what gmf_solve_json returns and frees it with gmf_free.
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include "gmf/gmf.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define GMF_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define GMF_EXPORT
#endif

namespace {

char* duplicate(const std::string& text) {
  char* buffer = static_cast<char*>(std::malloc(text.size() + 1));
  if (buffer == nullptr) return nullptr;
  std::memcpy(buffer, text.c_str(), text.size() + 1);
  return buffer;
}

}  // namespace

extern "C" {

GMF_EXPORT char* gmf_solve_json(const char* request) {
  if (request == nullptr) return duplicate("{\"status\":\"invalid_input\",\"error\":\"no request\"}");
  try {
    return duplicate(gmf::solve_json(std::string(request)));
  } catch (const std::bad_alloc&) {
    return nullptr;
  } catch (...) {
    return duplicate("{\"status\":\"invalid_input\",\"error\":\"unknown error\"}");
  }
}

GMF_EXPORT void gmf_free(char* buffer) { std::free(buffer); }

GMF_EXPORT const char* gmf_version() { return gmf::version(); }

}  // extern "C"
