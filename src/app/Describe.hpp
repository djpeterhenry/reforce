#pragma once
// `reforce --describe-config PATH...` (Phase 5B.4 corpus sweeps): static analysis of scene files.
// Prints one JSON object per file: type, diagnostics counts, and whether the evaluated keys (with their
// dependencies on other keys and A-D vars) use rnd-like builtins, time (t/DT) or audio (BASS/LEVEL/ENERGY,
// mag/fft), overall ("all") and per tier group ("load" = Pre/A/PostLoad, "frame" = B, "step" = C and step
// outputs, "pixel" = D and FlowField outputs), plus the builtins used. rnd in a load-tier key counts for the
// "load" group only (it is fixed once per instance). Reads files only; no Metal.

#include <string>
#include <vector>

namespace reforce::app {

int describeConfigs(const std::vector<std::string>& paths);

}  // namespace reforce::app
