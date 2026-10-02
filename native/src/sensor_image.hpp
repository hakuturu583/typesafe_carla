// Image conversion and file output for sensor measurements (issue #24).
// Pure C++: shared by the LibCarla and mock backends.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tsc::image {

// Fails with TSC_INVALID_ARGUMENT unless cc is a tsc_color_converter_t.
void check_converter(int32_t cc);

// Converts BGRA pixels in place with the formulas of LibCarla's
// image::ColorConverter, as Image.convert does in the Python API.
void convert_bgra(uint8_t *bgra, size_t pixel_count, int32_t cc);

// The checks of write_png, for running them before any file or directory is
// created: a valid converter, a size PNG's single IDAT chunk can hold (2^31 - 1
// bytes, about 0.5 Gpx in RGBA), and a buffer of at least width * height
// pixels.
void check_png(size_t width, size_t height, size_t pixel_count, int32_t cc);

// Image.save_to_disk: writes the BGRA pixels (pixel_count of them), converted
// with cc, as an uncompressed 8-bit PNG (gray for Depth and LogarithmicDepth,
// RGBA otherwise), one row at a time.
void write_png(const std::string &path, size_t width, size_t height, const uint8_t *bgra,
               size_t pixel_count, int32_t cc);

// OpticalFlowImage.get_color_coded_flow of the Python API: BGRA, alpha 0.
void color_coded_flow(const float *xy, size_t pixel_count, uint8_t *bgra_out);

}  // namespace tsc::image
