# reforce content

reforce's own scene content, laid out like a G-Force scene root (`Packages/`, `Themes/`, `Categories/`). Unlike
`scenes/` (a local copy of G-Force's files, never committed), everything here is ours and is committed.

- `Packages/reforce.Solid.package/`: 3D solid scene files (docs/scene-format.md "Solids", docs/3d-design.md).
- `Packages/reforce.Organic.package/`: organics, procedural growth (docs/scene-format.md "Organic",
  docs/organics-design.md).
- `Packages/reforce.Dancer.package/`: dancer characters (docs/scene-format.md "Dancer", docs/dancers-design.md).
- `Moves/aist.moves`: mocap dance moves (docs/dancers-design.md "Mocap moves"), built by tools/aist_import from
  **AIST++** (Li, Yang, Ross, Kanazawa: "AI Choreographer: Music Conditioned 3D Dance Generation with AIST++", ICCV
  2021; https://google.github.io/aistplusplus_dataset/), whose annotations are licensed by Google LLC under
  CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/). Changed: retargeted to our characters, cut into 8-beat
  loops, and filtered. The motion was recorded from the AIST Dance Video Database (Tsuchida et al., ISMIR 2019).
- `Themes/`: themes that join G-Force's in the theme list. `Solids` turns on a solid every ~30 s, `Organics` an
  organic, `Organics: <group>` an organic from that group only, `Dance Party` a dancer every ~15 s.
- `Controllers/`: DJ controller MIDI maps (faders, crossfader, assign switches, hot cue pads) for deck starts
  (docs/rhythm-design.md; format in src/sync/ControllerMap.hpp).
- `Categories/Organic/`: the organic groups (Trees, Pendulums, Spirals, Curves, Cells, Water, Fractals), which the
  Organics card browses and the group themes select.

reforce reads this folder from the source tree by default (an app built elsewhere uses the copy bundled in it); `reforce.ContentDir` or `--content DIR|none` changes it.
