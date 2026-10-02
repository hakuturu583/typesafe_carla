// libcarla builds only: the shim's color converters (native/src/sensor_image.cpp)
// against LibCarla's own image::ImageConverter / ImageView (Boost.GIL), the code
// the Python API's Image.convert and Image.save_to_disk run. Bit-exact.
#include "sensor_image.hpp"
#include "internal.hpp"

#include <carla/image/ImageConverter.h>
#include <carla/image/ImageView.h>

#include <cstdio>
#include <random>
#include <vector>

namespace gil = boost::gil;
namespace ci = carla::image;

static int g_failures = 0;

// LibCarla, in place on a BGRA buffer (as ConvertImage in PythonAPI/SensorData.cpp).
template <typename CC>
static void libcarla_convert(std::vector<uint8_t> &bgra, size_t w, size_t h) {
  auto view = gil::interleaved_view(w, h, reinterpret_cast<gil::bgra8_pixel_t *>(bgra.data()),
                                    w * 4);
  ci::ImageConverter::ConvertInPlace(view, CC());
}

// LibCarla's gray view for save_to_disk (Depth / LogarithmicDepth), copied out.
template <typename CC>
static std::vector<uint8_t> libcarla_gray(std::vector<uint8_t> bgra, size_t w, size_t h) {
  auto view = gil::interleaved_view(w, h, reinterpret_cast<gil::bgra8c_pixel_t *>(bgra.data()),
                                    w * 4);
  auto converted = ci::ImageView::MakeColorConvertedView(view, CC());
  std::vector<uint8_t> out(w * h);
  auto dst = gil::interleaved_view(w, h, reinterpret_cast<gil::gray8_pixel_t *>(out.data()), w);
  gil::copy_pixels(converted, dst);
  return out;
}

template <typename CC>
static void check(const char *name, int32_t cc, const std::vector<uint8_t> &input, size_t w,
                  size_t h) {
  std::vector<uint8_t> expected = input, actual = input;
  libcarla_convert<CC>(expected, w, h);
  tsc::image::convert_bgra(actual.data(), w * h, cc);
  size_t bad = 0;
  for (size_t i = 0; i < expected.size(); ++i) bad += expected[i] != actual[i];
  if (bad != 0) {
    std::fprintf(stderr, "%s: %zu bytes differ from LibCarla\n", name, bad);
    ++g_failures;
  }
}

int main() {
  // Every red/green value with a few blues, plus random pixels.
  std::vector<uint8_t> px;
  for (int b : {0, 1, 127, 255}) {
    for (int g = 0; g < 256; ++g) {
      for (int r = 0; r < 256; ++r) px.insert(px.end(), {uint8_t(b), uint8_t(g), uint8_t(r), 255});
    }
  }
  std::mt19937 rng(24);
  for (int i = 0; i < 100000; ++i) {
    const uint32_t v = rng();
    px.insert(px.end(), {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)});
  }
  const size_t w = 256, h = px.size() / 4 / w;
  px.resize(w * h * 4);

  check<ci::ColorConverter::Depth>("Depth", TSC_COLOR_CONVERTER_DEPTH, px, w, h);
  check<ci::ColorConverter::LogarithmicDepth>("LogarithmicDepth",
                                              TSC_COLOR_CONVERTER_LOGARITHMIC_DEPTH, px, w, h);
  check<ci::ColorConverter::CityScapesPalette>("CityScapesPalette",
                                               TSC_COLOR_CONVERTER_CITYSCAPES_PALETTE, px, w, h);
  // save_to_disk's gray: the R channel of the in-place result must equal
  // LibCarla's gray8 view.
  for (int32_t cc : {TSC_COLOR_CONVERTER_DEPTH, TSC_COLOR_CONVERTER_LOGARITHMIC_DEPTH}) {
    const auto gray = cc == TSC_COLOR_CONVERTER_DEPTH
                          ? libcarla_gray<ci::ColorConverter::Depth>(px, w, h)
                          : libcarla_gray<ci::ColorConverter::LogarithmicDepth>(px, w, h);
    std::vector<uint8_t> ours = px;
    tsc::image::convert_bgra(ours.data(), w * h, cc);
    size_t bad = 0;
    for (size_t i = 0; i < w * h; ++i) bad += gray[i] != ours[4 * i + 2];
    if (bad != 0) {
      std::fprintf(stderr, "gray view (converter %d): %zu pixels differ\n", int(cc), bad);
      ++g_failures;
    }
  }
  if (g_failures != 0) return 1;
  std::printf("test_image_converter: %zu pixels match LibCarla's ImageConverter\n", w * h);
  return 0;
}
