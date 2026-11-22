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
```

`run` creates `.venv` (numpy, scipy) on first use. Only `keypoints3d/` is read (the 3D joints, `keypoints3d_optim`);
the pickles are opened with an unpickler that only builds numpy arrays.

- `aistlib.py`: loading, sequence names (genre, dancer, music ID -> tempo), COCO joint indices.
- `retarget.py`: one clip's frames -> the size-free pose channels the engine reads (engine/DanceClips).
- `build_moves.py`: 8-beat loops on bar lines, loop closing, filtering, scoring, the file format.
- `inspect_moves.py`: reads a moves file back; biggest jumps per move and the lowest hips.
