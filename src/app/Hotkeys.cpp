#include "app/Hotkeys.hpp"

#include "engine/Blend.hpp"

namespace reforce::app {

std::vector<engine::Command> hotkeyCommands(const platform::KeyEvent& key,
                                            const std::function<int(engine::Layer)>& blendMode) {
    using engine::Command;
    using engine::Layer;
    std::vector<Command> out;
    if (key.command || key.control || key.option) return out;
    auto sw = [&](Layer l) { out.push_back(key.shift ? Command::previous(l) : Command::next(l)); };
    auto cycleBlend = [&](Layer l) {
        const auto& names = engine::blendModeNames(l);
        if (names.empty()) return;
        const int cur = blendMode ? blendMode(l) : 0;
        out.push_back(Command::blendMode(l, names[std::size_t(cur + 1) % names.size()]));
    };
    const std::string& c = key.chars;
    if (c == "w") sw(Layer::WaveShape);
    else if (c == "c") sw(Layer::ColorMap);
    else if (c == "f") sw(Layer::FlowField);
    else if (c == "u") out.push_back(Command::next(Layer::Union));
    else if (c == "h") out.push_back(Command::toggleHoldAll());
    else if (c == "1") cycleBlend(Layer::WaveShape);
    else if (c == "2") cycleBlend(Layer::ColorMap);
    else if (c == "3") cycleBlend(Layer::FlowField);
    else if (c == "p") out.push_back(key.shift ? Command::kill(Layer::Particle) : Command::next(Layer::Particle));
    else if (c == "s") out.push_back(key.shift ? Command::kill(Layer::Sprite) : Command::next(Layer::Sprite));
    else if (c == "b") out.push_back(Command::look("ab"));  // Look A/B (engine/Look.hpp)
    // Sprite Mix nudges (PLAN 5K): [ / ] image sprites -/+ 0.1, Shift ({ / }) movie and live sprites.
    else if (c == "[" || c == "{") out.push_back(Command::spriteMix(key.shift || c == "{", -0.1, true));
    else if (c == "]" || c == "}") out.push_back(Command::spriteMix(key.shift || c == "}", +0.1, true));
    return out;
}

}  // namespace reforce::app
