# aist_import

Builds reforce's mocap dance moves (`content/Moves/aist.moves`) from AIST++ (docs/dancers-design.md "Mocap moves").

```sh
# once: the AIST++ annotations (CC BY 4.0), about 950 MB, kept outside the repo
mkdir -p ~/reforce/aist/dl ~/reforce/aist/data && cd ~/reforce/aist/dl
curl -LO https://github.com/google/aistplusplus_dataset/releases/download/v1.0/keypoints3d.zip
cd ../data && unzip -q ../dl/keypoints3d.zip

tools/aist_import/run build_moves.py ~/reforce/aist/data content/Moves/aist.moves          # all genres
tools/aist_import/run build_moves.py ~/reforce/aist/data /tmp/house.moves HO,LO            # just some
tools/aist_import/run inspect_moves.py content/Moves/aist.moves                             # jumps, low hips
tools/aist_import/run build_floor.py ~/reforce/aist/data content/Moves/aist_floor.moves     # break floor work
```

`run` creates `.venv` (numpy, scipy) on first use. Only `keypoints3d/` is read (the 3D joints, `keypoints3d_optim`);
the pickles are opened with an unpickler that only builds numpy arrays.

- `aistlib.py`: loading, sequence names (genre, dancer, music ID -> tempo), COCO joint indices.
- `retarget.py`: one clip's frames -> the size-free pose channels the engine reads (engine/DanceClips).
- `build_moves.py`: 8-beat loops on bar lines, loop closing, filtering, scoring, the file format.
- `inspect_moves.py`: reads a moves file back; biggest jumps per move and the lowest hips.
- `build_floor.py`: the break floor work build_moves leaves out (docs/dancers-design.md "Break floor work"), its own
  file with the pelvis as a quaternion and the floor clearances (the upper body; each hand, knee, the head), tiered
  floor / flips, moves mostly standing left out.
