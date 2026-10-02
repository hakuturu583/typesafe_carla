// Shared value types (issues #19 and #21): caller-owned string and transform
// lists, and texture conversions.
#include "internal.hpp"

#include <cstdlib>

namespace tsc {

void transform_list_assign(tsc_transform_list_t *out,
                           const std::vector<carla::geom::Transform> &values) {
  require_ptr(out, "out");
  tsc_transform_list_t list{alloc_list_items<tsc_transform_t>(values.size()), values.size()};
  for (size_t i = 0; i < values.size(); ++i) list.items[i] = from_carla(values[i]);
  *out = list;
}

namespace {

template <typename Texture, typename CTexture, typename Convert>
Texture to_texture(const CTexture *texture, const char *name, Convert &&convert) {
  const CTexture &t = *require_ptr(texture, name);
  const uint64_t n = static_cast<uint64_t>(t.width) * t.height;
  if (n > std::numeric_limits<uint32_t>::max()) {
    fail(TSC_INVALID_ARGUMENT, std::string(name) + " has more than 2^32 - 1 pixels");
  }
  require_array(t.pixels, static_cast<size_t>(n), name);
  Texture result(t.width, t.height);
  for (uint32_t y = 0; y < t.height; ++y) {
    for (uint32_t x = 0; x < t.width; ++x) {
      result.At(x, y) = convert(t.pixels[static_cast<size_t>(y) * t.width + x]);
    }
  }
  return result;
}

}  // namespace

carla::rpc::TextureColor to_carla_texture(const tsc_texture_color_t *texture, const char *name) {
  return to_texture<carla::rpc::TextureColor>(texture, name, [](const tsc_color_t &c) {
    return carla::sensor::data::Color(c.r, c.g, c.b, c.a);
  });
}

carla::rpc::TextureFloatColor to_carla_texture(const tsc_texture_float_color_t *texture,
                                               const char *name) {
  return to_texture<carla::rpc::TextureFloatColor>(texture, name, [](const tsc_float_color_t &c) {
    return carla::rpc::FloatColor(c.r, c.g, c.b, c.a);
  });
}

carla::rpc::MaterialParameter to_material_parameter(int32_t parameter) {
  if (parameter < TSC_MATERIAL_NORMAL || parameter > TSC_MATERIAL_EMISSIVE) {
    fail(TSC_INVALID_ARGUMENT, "invalid material parameter " + std::to_string(parameter));
  }
  // tsc_material_parameter_t has rpc::MaterialParameter's order.
  return static_cast<carla::rpc::MaterialParameter>(parameter);
}

}  // namespace tsc

extern "C" {

void tsc_transform_list_free(tsc_transform_list_t *list) { tsc::free_list_items(list); }

}  // extern "C"
