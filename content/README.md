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
- `Themes/`: themes that join G-Force's in the theme list. `Objects` turns on an object (a solid or an organic) every
  ~30 s, `Objects: <group>` one from that group only, `Dance Party` a dancer every ~15 s.
- `Controllers/`: DJ controller MIDI maps (faders, crossfader, assign switches, hot cue pads) for deck starts
  (docs/rhythm-design.md; format in src/sync/ControllerMap.hpp).
- `Categories/Object/`: the object groups (Trees, Pendulums, Curves, Cells, Water, Fractals, Arcade, Solids,
  Puzzles), which the Objects card browses and the group themes select.

## Naming scenes

When an object has more than one scene, every scene's name starts with the object's common type name, then what makes
that one different, so they sort together in the lists (Peter 2026-11-23): Dragon Fold / Dragon Rounded, Rain Downpour
/ Rain Floor / Rain Pool, Ripples Kick / Ripples Pond / Ripples Ring, Julia Drift / Julia Pulse / Julia Bands. A name
that describes the scene but starts elsewhere ("Four Dragons", "Downpour", "Hairpin Alley") hides it from its
siblings. A one-scene object just uses its type name (Tilt Maze).

reforce reads this folder from the source tree by default (an app built elsewhere uses the copy bundled in it); `reforce.ContentDir` or `--content DIR|none` changes it.
