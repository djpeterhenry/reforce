#pragma once
// Scene hotkeys (window key presses -> engine commands). Pure: App::onKey
// uses it with the live blend modes, tests feed it key events directly.
//
//   W / C / F      next WaveShape / ColorMap / FlowField (Shift: previous)
//   U              the next FlowField transition uses the next Union
//   H              toggle hold on all layers
//   1 / 2 / 3      cycle the WaveShape / ColorMap / FlowField blend mode
//   P / S          start the next particle / sprite (Shift: kill them all)
//   B              Look A/B toggle
//   [ / ]          image sprite Mix -/+ 0.1 (Shift, i.e. { / }: movie and live sprites; session only, the
//                  panel's Mix sliders save to settings.json)
// Any Command, Control or Option chord is not a scene hotkey.

#include <functional>
#include <vector>

#include "engine/Commands.hpp"
#include "platform/Platform.hpp"

namespace reforce::app {

// `blendMode(layer)` returns the layer's current blend mode index. Empty = not a hotkey.
std::vector<engine::Command> hotkeyCommands(const platform::KeyEvent& key,
                                            const std::function<int(engine::Layer)>& blendMode);

}  // namespace reforce::app
