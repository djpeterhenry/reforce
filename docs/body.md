# Body tracking

reforce follows the person on the [camera backdrop](backdrop.md) and lets the visuals react. Web UI: the **Body
tracking** card, below Camera backdrop.

The backdrop is required: the tracker reads the backdrop's frames (camera or movie file, not Syphon yet). To track
without showing the camera, set the backdrop's Brightness to 0.

## Tracking

Apple Vision (`video/BodyTracker`): person segmentation (a silhouette mask, 512 × 384 at *balanced*, 256 × 192 at
*fast*) and 2D body pose (19 joints with confidence). A live backdrop hands each camera frame to the tracker on the
capture thread as it arrives (movie files are offered from the render thread). A frame arriving while the previous
one is still being processed is skipped, so nobody waits. The pose runs only when something uses it (hand trails,
Show tracking), on its own queue at the same time as the silhouette.

Measured with the BRIO at 1080p on an M4 Pro:

| | per frame |
|---|---|
| silhouette, balanced / fast (`reforce-body-probe`, alone) | ~10 / ~4 ms |
| silhouette inside reforce (GPU shared with the visuals) | ~17 ms |
| pose, alongside the silhouette | ~15 ms, in parallel: no extra time |

So the tracker keeps up with every camera frame; the rate is the camera's. Cameras are opened with their frame rate
allowed up to 30 fps (`raiseFrameRate` in video/VideoSourceMac.mm; the BRIO came up capped at 24) and still free to
slow down, to 5 fps, when they lengthen the exposure in the dark. The Body tracking card shows the camera's rate next
to the tracker's, and the silhouette and pose times.

The first frame takes about a second while Vision loads its models (off the render thread).

**In sync** (`reforce.Body.Sync`, *Sync camera*, on by default): the backdrop shows the camera frame the current
silhouette was computed from, not the newest one, so the person and their cut-out line up exactly; the camera image
runs one tracker step (~17 ms) later. If the tracker falls more than 3 frames behind (starting up), the newest frame
shows. Off: the newest frame, with the silhouette a frame or so behind it.

Joints are smoothed per simulation step (`engine::PoseSmoother`): a one-euro filter (little lag when moving, steady
when still); a joint fades in over 0.25 s once seen with confidence ≥ 0.3 and fades out, holding its place, when lost
or when no new pose came for 0.6 s.

Everything is in camera coordinates and mapped exactly like the backdrop (its Fit and Mirror), so effects line up
with the camera image.

## Reactions

| Panel | Setting | What it does |
|---|---|---|
| Person in front | `reforce.Body.Front` 0..1 | The mask cuts through the visuals on the backdrop pass: the camera shows the person in front of the visuals, behind the dancers, their floor and the now-playing text. |
| Silhouette in visuals | `reforce.Body.Burn` off / fill / outline / motion, `reforce.Body.BurnAmount` 0..1 | Drawn into the trail buffer every step (I = max(I, amount × x)), so the flow field carries and colours it: the whole silhouette, its edge, or only where it moved since the previous mask. |
| Hand trails | `reforce.Body.Trails` 0..1 (0 = off), `reforce.Body.TrailWidth` px at 1080 | A line from where each wrist was to where it is now, each step, into the trail buffer (light painting; the flow smears it). |
| Show tracking | `reforce.Body.Show` | Tints the mask red and marks the joints green on the output, to check the tracking. |
| Track / Quality | `reforce.Body.Track`, `reforce.Body.Quality` balanced / fast | |
| Sync camera | `reforce.Body.Sync` (default on) | See above. |

Commands: `{"action":"videoset","key":"body<key>","value":...}` with the keys above in lower case (`bodytrack`,
`bodyfront`, `bodyburn`, `bodyburnamount`, `bodytrails`, `bodytrailwidth`, `bodyshow`, `bodyquality`, `bodysync`).

## Where it happens

- `video/BodyTracker.{hpp,mm}`: Vision, the frame hand-off, the mask copy.
- `engine/Body.{hpp,cpp}`: settings, one-euro filter, `PoseSmoother`, `cameraToScreen`, `HandTrails` (unit tests:
  tests/test_engine_body.cpp).
- `Visualizer::stepBody` (feeds the tracker, uploads the mask into a ring of R8 textures, smooths, adds the hand
  trails to the step's line segments), the silhouette pass after the step (`ScenePipeline::encodeBodyBurn`,
  `body_burn_fs`), and the backdrop pass (`backdrop_fs`: front cut, show overlay; drawn before the dancers). GPU tests: tests/test_video.cpp.

## Ideas not built yet

- Joint values for scenes (hand positions and speeds as expression variables, like BASS).
- Particles or WaveShapes emitted from the hands; dancers that look at or mirror the person.
- Motion pushing the flow (optical-flow style); fingers (Vision hand pose).
- Tracking a Syphon source.
