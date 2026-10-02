// Thread-local error state and string ownership helpers.
#include "internal.hpp"

#include <cstdlib>

namespace {
thread_local std::string g_last_error;
}

namespace tsc {

void set_last_error(const std::string &message) { g_last_error = message; }

void string_assign(tsc_string_t *out, const std::string &value) {
  require_ptr(out, "out");
  char *data = static_cast<char *>(std::malloc(value.size() + 1));
  if (data == nullptr) throw std::bad_alloc();
  std::memcpy(data, value.data(), value.size());
  data[value.size()] = '\0';
  out->data = data;
  out->size = value.size();
}

}  // namespace tsc

extern "C" {

const char *tsc_last_error_message(void) { return g_last_error.c_str(); }

void tsc_clear_last_error(void) { g_last_error.clear(); }

void tsc_string_free(tsc_string_t *string) {
  if (string == nullptr) return;
  std::free(string->data);
  string->data = nullptr;
  string->size = 0;
}

void tsc_actor_attribute_free(tsc_actor_attribute_t *attribute) {
  if (attribute == nullptr) return;
  tsc_string_free(&attribute->id);
  tsc_string_free(&attribute->value);
}

}  // extern "C"
