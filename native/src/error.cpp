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

void string_list_assign(tsc_string_list_t *out, const std::vector<std::string> &values) {
  require_ptr(out, "out");
  *out = tsc_string_list_t{};
  tsc_string_list_t result{};
  result.items = static_cast<tsc_string_t *>(std::calloc(values.empty() ? 1 : values.size(),
                                                         sizeof(tsc_string_t)));
  if (result.items == nullptr) throw std::bad_alloc();
  try {
    for (const auto &value : values) string_assign(&result.items[result.size++], value);
  } catch (...) {
    tsc_string_list_free(&result);
    throw;
  }
  *out = result;
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

void tsc_string_list_free(tsc_string_list_t *list) {
  if (list == nullptr) return;
  for (size_t i = 0; list->items != nullptr && i < list->size; ++i) tsc_string_free(&list->items[i]);
  std::free(list->items);
  list->items = nullptr;
  list->size = 0;
}

void tsc_actor_attribute_free(tsc_actor_attribute_t *attribute) {
  if (attribute == nullptr) return;
  tsc_string_free(&attribute->id);
  tsc_string_free(&attribute->value);
}

}  // extern "C"
