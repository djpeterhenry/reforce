#pragma once
// PNG read/write through ImageIO (no third-party image libraries).

#include <cstdint>
#include <string>
#include <vector>

namespace reforce::render {

struct ImageRgba8 {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;  // width * height * 4, rows top to bottom, straight alpha
};

// Decodes any ImageIO-supported file (PNG, JPEG, ...) to RGBA8. False on failure.
// ignoreAlpha: drop the file's alpha channel and keep the straight colour of every pixel, even fully
// transparent ones (opaque output), as G-Force does for sprites (probe #42). Without it, transparent
// pixels decode premultiplied (their colour is lost where alpha is 0).
bool loadImageRgba8(const std::string& path, ImageRgba8& out, bool ignoreAlpha = false);

// Writes BGRA8 pixels (alpha ignored, as in the render target) as an opaque PNG.
bool writePngBgra8(const std::string& path, const std::uint8_t* bgra, int width, int height,
                   std::size_t bytesPerRow);

}  // namespace reforce::render
