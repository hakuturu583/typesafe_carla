// Image conversion and file output for sensor measurements (issue #24).
//
// The color converters reproduce LibCarla's image::ColorConverter as applied
// through Boost.GIL by the Python API (Image.convert / Image.save_to_disk),
// including GIL's float-to-uint8 rounding (x * 255 + 0.5, truncated) and its
// gray-to-RGBA expansion (r = g = b, alpha 255). The CityScapes palette is
// LibCarla's own (image::CityScapesPalette), so it follows the CARLA ref.
//
// PNG output does not use LibCarla's ImageIO: that needs libpng's headers in
// the shim's include path, and LibCarla's build does not export them (the
// compiler would silently pick up a system png.h, if any). The writer below
// has no dependency; it stores the image uncompressed.
#include "sensor_image.hpp"

#include "internal.hpp"

#include <array>
#include <fstream>
#include <vector>

namespace tsc::image {

namespace {

uint8_t channel_from_float(float x) {
  return static_cast<uint8_t>(static_cast<uint32_t>(x * 255 + 0.5f));
}

// ColorConverter::Depth: 24-bit depth from R (low), G and B (high), in [0, 1].
float normalized_depth(const uint8_t *bgra) {
  const float depth = bgra[2] + (bgra[1] * 256) + (bgra[0] * 256 * 256);
  return depth / static_cast<float>(256 * 256 * 256 - 1);
}

// ColorConverter::LogarithmicLinear applied to the normalized depth.
float logarithmic_depth(const uint8_t *bgra) {
  const float value = 1.0f + std::log(normalized_depth(bgra)) / 5.70378f;
  return std::max(std::min(value, 1.0f), 0.005f);
}

uint8_t gray_of(const uint8_t *bgra, int32_t cc) {
  return channel_from_float(cc == TSC_COLOR_CONVERTER_DEPTH ? normalized_depth(bgra)
                                                            : logarithmic_depth(bgra));
}

// Converts one BGRA pixel in place (ColorConverter as applied through GIL).
void convert_pixel(uint8_t *p, int32_t cc) {
  if (cc == TSC_COLOR_CONVERTER_RAW) return;
  if (cc == TSC_COLOR_CONVERTER_CITYSCAPES_PALETTE) {
    const auto color = carla::image::CityScapesPalette::GetColor(p[2]);
    p[0] = color[2u];
    p[1] = color[1u];
    p[2] = color[0u];
  } else {
    p[0] = p[1] = p[2] = gray_of(p, cc);
  }
  p[3] = 255u;
}

// --- PNG -------------------------------------------------------------------

const std::array<uint32_t, 256> &crc_table() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[n] = c;
    }
    return t;
  }();
  return table;
}

// One PNG chunk written straight to the file: its CRC covers the type and
// the data, which callers stream through write().
class Chunk {
 public:
  Chunk(std::ofstream &file, const char *type, uint32_t length) : file_(file) {
    write_u32_raw(length);
    write(reinterpret_cast<const uint8_t *>(type), 4);
  }
  ~Chunk() { write_u32_raw(crc_ ^ 0xFFFFFFFFu); }

  void write(const uint8_t *p, size_t n) {
    const auto &table = crc_table();
    uint32_t crc = crc_;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    crc_ = crc;
    file_.write(reinterpret_cast<const char *>(p), static_cast<std::streamsize>(n));
  }
  void write_u32(uint32_t v) {
    const uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    write(b, 4);
  }

 private:
  void write_u32_raw(uint32_t v) {  // the length and the CRC are outside the CRC
    const char b[4] = {char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
    file_.write(b, 4);
  }
  std::ofstream &file_;
  uint32_t crc_ = 0xFFFFFFFFu;
};

// A zlib stream of stored deflate blocks (at most 65535 bytes each) inside
// one IDAT chunk, fed in pieces of any size.
class StoredZlib {
 public:
  static constexpr size_t kBlock = 65535;
  static uint64_t encoded_size(uint64_t raw) {
    return 2 + 5 * ((raw + kBlock - 1) / kBlock) + raw + 4;
  }

  StoredZlib(Chunk &chunk, uint64_t raw) : chunk_(chunk), left_(raw) {
    const uint8_t header[2] = {0x78, 0x01};
    chunk_.write(header, 2);
  }

  void write(const uint8_t *p, size_t n) {
    while (n > 0) {
      if (in_block_ == 0) start_block();
      const size_t k = std::min(n, in_block_);
      chunk_.write(p, k);
      adler(p, k);
      p += k;
      n -= k;
      in_block_ -= k;
    }
  }
  void finish() { chunk_.write_u32((b_ << 16) | a_); }

 private:
  void start_block() {
    const size_t n = static_cast<size_t>(std::min<uint64_t>(kBlock, left_));
    left_ -= n;
    const uint8_t header[5] = {uint8_t(left_ == 0 ? 1 : 0),  // BFINAL, BTYPE = stored
                               uint8_t(n), uint8_t(n >> 8), uint8_t(~n), uint8_t(~n >> 8)};
    chunk_.write(header, 5);
    in_block_ = n;
  }
  // Adler-32, reducing modulo 65521 every 5552 bytes (zlib's NMAX).
  void adler(const uint8_t *p, size_t n) {
    while (n > 0) {
      const size_t k = std::min<size_t>(n, 5552);
      for (size_t i = 0; i < k; ++i) {
        a_ += p[i];
        b_ += a_;
      }
      a_ %= 65521u;
      b_ %= 65521u;
      p += k;
      n -= k;
    }
  }

  Chunk &chunk_;
  uint64_t left_;
  size_t in_block_ = 0;
  uint32_t a_ = 1, b_ = 0;
};

}  // namespace

void check_converter(int32_t cc) {
  if (cc < TSC_COLOR_CONVERTER_RAW || cc > TSC_COLOR_CONVERTER_CITYSCAPES_PALETTE) {
    fail(TSC_INVALID_ARGUMENT, "invalid color converter " + std::to_string(cc));
  }
}

void convert_bgra(uint8_t *bgra, size_t pixel_count, int32_t cc) {
  check_converter(cc);
  if (cc == TSC_COLOR_CONVERTER_RAW) return;
  for (size_t i = 0; i < pixel_count; ++i) convert_pixel(bgra + 4 * i, cc);
}

void check_png(size_t width, size_t height, size_t pixel_count, int32_t cc) {
  check_converter(cc);
  const bool gray = cc == TSC_COLOR_CONVERTER_DEPTH || cc == TSC_COLOR_CONVERTER_LOGARITHMIC_DEPTH;
  const uint64_t raw = (uint64_t{width} * (gray ? 1 : 4) + 1) * height;  // filter byte per row
  // One IDAT chunk holds at most 2^31 - 1 bytes: about 0.5 Gpx in RGBA.
  if (width == 0 || height == 0 || width > 0x7FFFFFFFu || height > 0x7FFFFFFFu ||
      StoredZlib::encoded_size(raw) > 0x7FFFFFFFu) {
    fail(TSC_INVALID_ARGUMENT, "cannot write a " + std::to_string(width) + "x" +
                                   std::to_string(height) + " image as PNG");
  }
  if (uint64_t{width} * height > pixel_count) {
    fail(TSC_ERROR, "image buffer holds " + std::to_string(pixel_count) + " pixels, not " +
                        std::to_string(width) + "x" + std::to_string(height));
  }
}

void write_png(const std::string &path, size_t width, size_t height, const uint8_t *bgra,
               size_t pixel_count, int32_t cc) {
  check_png(width, height, pixel_count, cc);
  const bool gray = cc == TSC_COLOR_CONVERTER_DEPTH || cc == TSC_COLOR_CONVERTER_LOGARITHMIC_DEPTH;
  const size_t channels = gray ? 1 : 4;
  const uint64_t raw = (uint64_t{width} * channels + 1) * height;  // filter byte per row
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) fail(TSC_ERROR, "cannot open " + path + " for writing");
  static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  file.write(reinterpret_cast<const char *>(signature), sizeof(signature));
  {
    Chunk ihdr(file, "IHDR", 13);
    ihdr.write_u32(static_cast<uint32_t>(width));
    ihdr.write_u32(static_cast<uint32_t>(height));
    // 8 bits, gray or RGBA, deflate, adaptive filtering, no interlace.
    const uint8_t rest[5] = {8, uint8_t(gray ? 0 : 6), 0, 0, 0};
    ihdr.write(rest, 5);
  }
  {
    // Scanlines (filter type 0), converted one row at a time.
    Chunk idat(file, "IDAT", static_cast<uint32_t>(StoredZlib::encoded_size(raw)));
    StoredZlib z(idat, raw);
    std::vector<uint8_t> row(1 + width * channels, 0);
    for (size_t y = 0; y < height; ++y) {
      const uint8_t *src = bgra + 4 * width * y;
      uint8_t *dst = row.data() + 1;
      for (size_t x = 0; x < width; ++x, src += 4) {
        if (gray) {
          dst[x] = gray_of(src, cc);
          continue;
        }
        uint8_t p[4] = {src[0], src[1], src[2], src[3]};
        convert_pixel(p, cc);
        uint8_t *q = dst + 4 * x;  // BGRA -> RGBA
        q[0] = p[2];
        q[1] = p[1];
        q[2] = p[0];
        q[3] = p[3];
      }
      z.write(row.data(), row.size());
    }
    z.finish();
  }
  Chunk(file, "IEND", 0);
  file.flush();
  if (!file) fail(TSC_ERROR, "failed writing " + path);
}

// The Python API's ColorCodedFlow (PythonAPI/carla/src/SensorData.cpp),
// pixel by pixel, in the same float arithmetic.
void color_coded_flow(const float *xy, size_t pixel_count, uint8_t *bgra_out) {
  constexpr float pi = 3.1415f;
  constexpr float rad2ang = 360.f / (2.f * pi);
  const float shift = 0.999f;
  const float a = 1.f / std::log(0.1f + shift);
  for (size_t index = 0; index < pixel_count; ++index) {
    const float vx = xy[2 * index];
    const float vy = xy[2 * index + 1];

    float angle = 180.f + std::atan2(vy, vx) * rad2ang;
    if (angle < 0) angle = 360.f + angle;
    angle = std::fmod(angle, 360.f);

    const float norm = std::sqrt(vx * vx + vy * vy);
    const float intensity = std::min(std::max(a * std::log(norm + shift), 0.f), 1.f);  // Math::Clamp

    const float H = angle;
    const float S = 1.f;
    const float V = intensity;
    const float H_60 = H * (1.f / 60.f);

    const float C = V * S;
    const float X = C * (1.f - std::abs(std::fmod(H_60, 2.f) - 1.f));
    const float m = V - C;

    float r = 0, g = 0, b = 0;
    switch (static_cast<unsigned int>(H_60)) {
      case 0: r = C; g = X; b = 0; break;
      case 1: r = X; g = C; b = 0; break;
      case 2: r = 0; g = C; b = X; break;
      case 3: r = 0; g = X; b = C; break;
      case 4: r = X; g = 0; b = C; break;
      case 5: r = C; g = 0; b = X; break;
      default: r = 1; g = 1; b = 1; break;
    }
    bgra_out[4 * index] = static_cast<uint8_t>((b + m) * 255.f);
    bgra_out[4 * index + 1] = static_cast<uint8_t>((g + m) * 255.f);
    bgra_out[4 * index + 2] = static_cast<uint8_t>((r + m) * 255.f);
    bgra_out[4 * index + 3] = 0;
  }
}

}  // namespace tsc::image
