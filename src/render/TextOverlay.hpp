#pragma once
// reforce extension: text over the finished frame (the now-playing overlay, sync/NowPlaying; the dancers' name tags).
//
// Text blocks are laid out with Core Text (the system font, white, like a browser's -apple-system) on a worker
// thread into premultiplied BGRA textures; the render thread swaps the newest one in and blends each block over the
// frame (premultiplied "over", alpha included, so the Syphon alpha keeps the text opaque). Each block is a stack of
// lines with CSS-like margins (collapsing between lines) and a line box of 1.2 x the font size; a line too wide for
// `maxWidth` ends in an ellipsis.
//
// Adaptive shadow: each block also carries a soft blurred copy of its letters (a CSS-like text-shadow). It shows
// only where the picture behind the text is bright: a 1/8-size luminance copy of the frame, averaged over ~0.25 s
// (so strobes and beats do not pump it), sets per pixel how much of the shadow is drawn, from none below
// `from` to `strength` at `from` + 0.3. On dark scenes the text is exactly plain white.

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace MTL { class Device; class Texture; class CommandBuffer; class RenderPipelineState; }

namespace reforce::render {

struct TextLine {
    std::string text;
    double sizePx = 48;
    bool bold = false;
    double marginTop = 0, marginBottom = 0;   // px
};

struct TextBlockSpec {
    std::vector<TextLine> lines;
    double maxWidth = 1000;   // px
    bool uppercase = false;
    // A speech bubble (the dancers' guest messages): the first line's text wrapped to maxWidth (at most 5 lines), white
    // in full color (emoji too) on a dark rounded box with a light outline and a tail at the bottom center pointing
    // down. Drawn as a color image (rasterizeBubble); the box's size is the whole image, tail included.
    bool bubble = false;
    bool operator==(const TextBlockSpec& o) const;
};

// CPU layout (any thread): BGRA rows, top to bottom; R = text coverage (= A), G = shadow coverage, B = 0.
// The text box (line boxes and margins) starts `inset` px in from each edge (room for the shadow).
// Empty spec: 0 x 0.
struct TextImage {
    int width = 0, height = 0, inset = 0;
    std::vector<std::uint8_t> bgra;
};
TextImage rasterizeText(const TextBlockSpec& spec);
// A speech bubble (TextBlockSpec::bubble): premultiplied BGRA in full color, inset 0.
TextImage rasterizeBubble(const TextBlockSpec& spec);

struct TextShadow {
    double strength = 0;    // 0..1; 0 = off
    double from = 0.55;     // background luminance where it starts (full at from + 0.3)
};

class TextOverlay {
public:
    static constexpr int kSlots = 2;   // the default slot count
    // `slots`: how many text blocks it holds (each laid out once, drawn any number of times).
    explicit TextOverlay(MTL::Device* device, int slots = kSlots);   // throws on a shader / pipeline failure
    ~TextOverlay();
    TextOverlay(const TextOverlay&) = delete;
    TextOverlay& operator=(const TextOverlay&) = delete;

    // Render thread: lay out `spec` for `slot` (a no-op when it is what the slot already has or is making).
    void set(int slot, const TextBlockSpec& spec);
    // Render thread: the slot's current text box size in px (0 x 0 while none is ready).
    int width(int slot) const { return ok(slot) && slots_[slot].tex ? slots_[slot].w - 2 * slots_[slot].inset : 0; }
    int height(int slot) const { return ok(slot) && slots_[slot].tex ? slots_[slot].h - 2 * slots_[slot].inset : 0; }
    int slots() const { return int(slots_.size()); }

    struct Draw {
        int slot = 0;
        double x = 0, y = 0;    // the text box's top-left, px of the output
        double opacity = 1;
        double scale = 1;       // drawn this much bigger / smaller around the box's center (a pop)
        bool image = false;     // the slot holds a color image (a speech bubble): drawn as it is, no shadow
    };
    using Shadow = TextShadow;
    // Render thread: picks up finished images, then blends the draws over `output`. `dt`: seconds since the last
    // call for this output size (the brightness average).
    void encode(MTL::CommandBuffer* cmd, MTL::Texture* output, const Draw* draws, int count, const Shadow& shadow = {},
                double dt = 0);

private:
    void worker();
    void adopt();
    bool ok(int slot) const { return slot >= 0 && slot < int(slots_.size()); }

    struct Slot {
        TextBlockSpec want;         // last requested
        bool haveWant = false;
        MTL::Texture* tex = nullptr;
        int w = 0, h = 0, inset = 0;
    };
    struct Job {
        int slot = 0;
        TextBlockSpec spec;
        std::uint64_t serial = 0;
    };
    struct Done {
        int slot = 0;
        MTL::Texture* tex = nullptr;   // retained; null = empty block
        int w = 0, h = 0, inset = 0;
        std::uint64_t serial = 0;
    };

    MTL::Device* device_;
    MTL::RenderPipelineState* pso_ = nullptr;
    MTL::RenderPipelineState* lumPso_ = nullptr;
    MTL::Texture* lum_ = nullptr;      // 1/8-size luminance of the frame, averaged over time
    bool lumFresh_ = true;             // next pass replaces it (first use, or after a pause)
    std::vector<Slot> slots_;
    std::vector<std::uint64_t> serial_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Job> jobs_;
    std::vector<Done> done_;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace reforce::render
