# Dancers: 3D cartoon characters dancing to the beat (PLAN 5P)

Peter's decisions (2026-11-18):
- **Full color on top** of the visualizer (composited after colorize, like the color movie sprites), so skin,
  clothes and later friends' faces read as themselves.
- **Procedural, built in**: our own chibi cartoon style (big round head, pill body, mitten hands, rounded shoes)
  made from rounded primitives; dance moves written as poses on the beat phase, so hits land exactly on the beat.
- **Staging**: the dancers stand along the bottom of the frame and by default **each does its own moves**.
  Canon (the same move rippling along the line) and together are options. An optional **perspective disco floor**
  appears under them, only while a dancer is live.

## Where it sits

- `SceneType::Dancer` (scene files in `reforce.Dancer.package`, committed under `content/Packages/`) and
  `Layer::Dancer`, a spawn layer like Solids and Organics: `switch:dancer:NAME`, `next:dancer`, `kill:dancer`,
  `life:dancer:SEC`, `interval:dancer:SEC`. Settings `Dancer.*` like `Solid.*` (interval 0 = not in the default
  slideshow; Duration 90 s). A Theme may set `Dancer.Slideshow.Interval.Duration` (content theme "Dance Party").
- Up to `reforce.Dancer.MaxLive` (5; at most 32) generated dancers live at once; another replaces the oldest. Stream
  guests don't count against it (Peter, 2026-11-21): they come and go regardless; the stage holds 32 in all
  (the renderer's budget), past which characters make way first, then the oldest guest. **A crowd pulls the camera
  back** (2026-11-20): above 14 on stage the dancer size is scaled by sqrt(14 / n) (32: 0.66), the floor reaches
  further back and the camera rises to look down more, so the back rows stand higher in the frame; it eases over a
  couple of seconds as people come and go. Name tags shrink a little with it. A scene file is **one character**
  (looks and move preferences). Starting the same file twice gives two dancers with different seeds.
- **Stage line**: dancer k of n stands at x = evenly spread across the frame width near the bottom; when one
  arrives or leaves, the others glide to their new spots over a beat or two (a little side-step).
- **Entrance / exit**: a cartoon pop (scale up with overshoot, landing from a small hop) and a wave goodbye before
  shrinking away, instead of an alpha fade (alpha would show the overlapping body parts through each other).

## Rendering (render/DancerPipeline)

One pass per sim step into one frame-size premultiplied BGRA8 target (4x MSAA memoryless color + depth), then
alpha-over onto the colorized frame inside `Visualizer::colorize`, so the window, Syphon and the split-video window
all get it. Nothing is encoded while no dancer is live (goldens unchanged).

- **Geometry**: every body part is an instance of one "capsule" mesh: a unit sphere split at the equator, the top
  half moved to one end and the bottom half to the other, with tapered radii r0, r1. That one mesh gives spheres
  (length 0), ellipsoids (non-uniform scale), limbs (capsules) and cones (r1 ~ 0, hair spikes). Instances carry a
  model matrix, length, radii, color, a material id and a clip plane (hair shells cut open at the face).
  Hair: close styles (short, spiky, ponytail, bun) are shells just larger than the head, cut open at the face (a
  smooth per-pixel hairline; the inside seen through the cut is shaded as solid hair). Fuller styles (bob, long,
  pigtails, afro) are solid masses set up and back so the face pokes out in front: a shell standing off the head
  shows its hollow inside as a hood or bowl around the face (2026-11-19). A mass must cross the head steeply or the
  mesh steps along the hairline.
- **Look**: soft toon shading (wrapped Lambert through a soft two-band ramp), a sky/ground hemisphere ambient, a
  plastic specular highlight, a rim light tinted by the current palette (so they sit in the scene's colors), and a
  thin dark outline (inverted hull, width in pixels). Settings `reforce.Dancer.Outline` (px, 0 = none).
- **The visuals are the lighting** (Peter, 2026-11-18): the dancers are drawn after colorize, each display frame, and
  light themselves from a small blurred, mipmapped copy of the finished frame. A surface takes light from the visuals
  in the direction its normal faces (a red flash on the left lights their left side red), a rim light comes from the
  frame behind them, and the floor reflects the visuals and its tiles take their colors.
  `reforce.Dancer.Lighting` (0..1, default .75) mixes a studio key light (0) with lighting from the visuals only (1),
  so they stay readable when the screen is dark.
- **Face**: drawn in the head's fragment shader from the head-local direction: eyes (with highlights; blink),
  brows, mouth (smile; opens on the beat / on drops), blush. Later: `Face="faces/name.png"` maps a portrait image on
  the face area instead (friends' faces) — the key is reserved now.
- **Floor** (`reforce.Dancer.Floor` on/off, default on; no panel button (Peter, 2026-11-20: always wanted)): a perspective grid of tiles in the stage plane that light
  up in palette colors on the beat (patterns change each bar: checker, chase, ripple from the center, random),
  fading out towards the back and the sides so it melts into the visualizer, with a soft blob shadow under each
  dancer. It fades in when the first dancer arrives and out after the last leaves.
- **Name tags** (2026-11-20, for stream guests): `Name="Sam"` in a dancer file puts the name over the dancer's
  head, white bold system type with a soft dark shadow (a second render/TextOverlay with a slot per dancer). The tag
  stands on the dancer's floor spot at a low-passed head height plus room for a top hat or big hair, so it follows
  them about without bobbing to every move, and pops in and out with them. `reforce.Dancer.NameTags`: named (only
  files with Name) | all (every dancer, its own name otherwise) | off; the panel's Names select.
- **Stream guests** (2026-11-20; setup and protocol: docs/guests.md): viewers sign up once on a guest page (the
  maintainer's: a name, and a dancer picked by Claude from a photo, which is not kept; or built by hand) and tap
  "I'm here" while watching. The guest service is separate from reforce (the maintainer's: a Cloudflare Worker with a
  D1 database). `web/GuestFeed` polls its `/api/checkins` every second on its own thread (HTTPS through NSURLSession); each new
  check-in becomes a dancer from the guest's keys plus `Name`, staying `reforce.Guests.Minutes` (30); checking in
  again while dancing changes the dancer in place to the look it carries and renews the stay (the guest page
  sends one when a guest edits their dancer on the floor). Guests choose how social they are (2026-11-27): Solo,
  Mingle (as everyone: one personality for all since 2026-11-27) or Crew (always in a group), and may pick a guest on stage to dance with (they walk
  over and stay; engine::Mingle, MingleVibe); reforce sends who is on stage with each poll and gets these back. When the stage is full, characters make way before guests. Polling also tells the
  page reforce is live (LIVE while it polled in the last minute). The first poll takes every check-in of the last 3
  hours that were never handed over; `guests-seen.txt` next to settings.json remembers the last check-in handed over,
  so a restart never brings anyone twice. Off by default with no built-in service (2026-11-22, for open source:
  `reforce.Guests.Enabled`, `reforce.Guests.Url`, and `reforce.Guests.Key`, a read key sent as a bearer token; the
  Dancers card shows the status, no button). Never polls offline (`--deterministic`)
  or in a `--background` test instance.
- **Guest messages** (2026-11-20): a guest types a message on the page ("Say something", up to 140 characters, no
  filter, no rate limit: Peter's choice); the service keeps it (`/api/say`, also a check-in) and hands it out with the
  check-ins for 2 minutes. reforce shows it in a speech bubble above the guest's name tag for 30 s (a new one replaces
  it): white bold text wrapped to at most 5 lines, emoji in color, on a dark rounded box with a light outline and a
  tail (render/TextOverlay `bubble`: drawn by Core Text into a color image). It pops in and fades over the last
  second. `beatset dancesay NAME=TEXT` gives any dancer a bubble (tests, or by hand).
- **Hide dancers** (2026-11-20): the panel's Hide dancers (`beatset dancehide on|off`, session only) fades the whole
  stage and the name tags out over half a second; everything keeps running behind it (hold times count down, guests
  still arrive), so showing it again brings back whoever is still due.
- **More looks for stream guests** (2026-11-20, Peter: "match people's appearance"): hairstyles pixie, bangs, wavy,
  braid, side pony, space buns, top knot and locs; outfits skirt, crop top, jumpsuit and long dress (random
  characters use them too); clear lenses (`Lens="clear"`: only the frame is drawn, in its `Frame` color). No gender
  setting (Peter: just make the clothes and hair available).
- **Web preview** (`tools/dancer_web`): the dancer code (DancerRig, DanceMoves, Dancers, DancerInstances, Meshes)
  compiled to WebAssembly with Emscripten, drawn by a WebGL2 port of the body and outline passes with the studio
  light, so a guest page shows the exact character, dancing. Both renderers pack parts with
  `engine::packDancerPart`; when the Metal body shader changes, port the change to `dancer_gl.js`. Build with
  `tools/dancer_web/build.sh`, then copy `build-web/dancer/*.js` to the site's `assets/dancer/`.
- **Camera**: fixed perspective (about 28° vertical FOV) slightly above, looking down at the stage; dancers are about
  `reforce.Dancer.Height` (0.24; Peter: dancers in about the bottom quarter) of the frame height tall.

## Animation (engine/DancerRig, engine/DanceMoves)

- **Rig**: hips (root), spine, chest, neck/head, shoulders → upper arm → forearm → hand, hips → thigh → shin →
  foot. Arms and legs are **two-bone IK** chains (elbows bend back/out, knees forward), so moves are written as
  hand and foot targets plus hip, torso and head motion: feet stay planted, claps meet, points land.
- **Dance styles** (Peter, 2026-11-18: "different characters could have different dance styles, and within each
  style a lot of variety"): each character has a `Style`. Originally disco, hiphop, house, pop, robot, latin, rave,
  retro, chill; since 2026-11-21 the mocap genres (below, "Styles are the dance genres"). A style changes the feel
  (hip-hop grooves down on the beat, house bounces up) and which moves it draws from.
- **Moves** come in three parts: **footwork** (29: two-step, running man, T-step, heel-toe, salsa, cha-cha, samba,
  charleston, grapevine, popping walk, skate, ...), **arm patterns** (31: disco point, roll, hitchhike, raise the
  roof, robot arms, tut, tick-tock, swim, maracas, lighter, hand jive, finger guns, ...) and **signature full-body
  moves** (21: spin, jump, floss, dab, cabbage patch, moonwalk, body roll, shimmy, freeze poses, salsa turn, conga,
  slow-mo run, ...). A routine is footwork + arms, or a signature move, plus a variation: mirrored, half / double
  time, a move-specific variant (which arm leads, clap overhead, kick height...), and amplitude. Each style offers
  60-155 footwork x arms combinations before variations (unit-tested to stay >= 30).
- A move is a function of the beat phase over its length (1, 2, 4 or 8 beats) writing a pose, tagged with the
  styles it belongs to and its energy (chill, groove, hype).
- **Choreographer** (per dancer, seeded): changes move on bar lines every 2 or 4 bars (phrase lines always), with a
  half-beat blend between moves. The choice follows the music: breakdowns (`SYNC_BUILD`) pick chill moves (sway,
  clap overhead, hands-up wave), builds raise the pace, a **drop** (`SYNC_DROP`) fires a jump and then hype moves.
  Amplitude follows the bass. Without a beat lock the dancers groove gently at the clock's tempo.
- **Sync mode** (`reforce.Dancer.Sync`, control panel): `own` (default: each picks its own moves), `together`
  (everyone does the same move), `canon` (the same move, dancer k one beat behind dancer k-1), `mirror` (the same
  move, every other dancer mirrored). Gone: off the panel 2026-11-21, out of the code 2026-11-27 ("Mingle only, one
  personality" below).

## Scene keys (`reforce.Dancer.package`)

Every key is a literal (no expressions; nothing in a character needs per-frame maths). Full list: scene-format §5.9.

| Key | Default | Meaning |
|---|---|---|
| `Skin`, `Hair`, `Top`, `Bottom`, `Shoes`, `Eyes` | various | Colors, `"#rrggbb"`. |
| `HairStyle` | `short` | `short` (`crew`, `crop`), `buzz`, `shaved`, `sidepart`, `quiff`, `pompadour`, `slickback`, `undercut`, `coils` (`twists`, `hightop`), `waves` (360 waves), `curtains` (a middle part), `balding`, `manbun`, `mullet`, `bowl`, `spiky`, `mohawk`, `anime` (`messy`), `curly`, `curls` (long curls past the shoulders; `longcurls`), `bigcurls` (a bigger curly mane), `hugecurls` (a huge one; `mane`), `afro`, `bob` (`lob`), `long` (`wavy`, `locs`), `layers`, `bangs` (long with a fringe), `shag`, `halfup`, `pixie`, `ponytail` (`lowbun`), `sidepony`, `braid` (a side braid), `pigtails`, `bun`, `topknot`, `spacebuns`, `bald`. |
| `Outfit` | `tee` | `tee`, `tank`, `hoodie`, `dress`, `suit` (sleeve length, skirt, collar). |
| `Hat` | `none` | `none`, `cap`, `beanie`, `top`, `headphones`. |
| `Build` | 1 | Body width (0.8 slim .. 1.25 wide). `Tall` 1: height (0.85 .. 1.15). |
| `Style` | `pop` | Dance style: `disco`, `hiphop`, `house`, `pop`, `robot`, `latin`, `rave`, `retro`, `chill`. |
| `Moves` | none | Comma list of move names the character likes (picked 4x as often), e.g. `floss,dab`. |
| `Snap` | the style's | 0 = loose and floppy .. 1 = normal .. 2 = sharp and snappy (how hard the hits are). |
| `Face` | none | Later: a face image (friends' faces). Not read yet. |
| `PDur` | Dancer.Duration | Seconds on stage. |

## Phases

| # | Deliverable | Done when |
|---|---|---|
| 1 | `engine::DancerRig` (math, skeleton, IK, body parts), `engine::DanceMoves` (moves, choreographer) + unit tests | feet planted, IK reaches, every move finite and hits on the beat, deterministic |
| 2 | `render/DancerPipeline` (capsule instances, toon shading, face, outline, floor, composite), test hook `--dancer-test` | offline frames look right; < 0.5 ms GPU at 1080p for 5 dancers; 0 render allocations |
| 3 | `Layer::Dancer` through scene type, Director, Visualizer, commands, settings, themes, web API | `switch:dancer:NAME` renders offline |
| 4 | Control panel Dancers card (floor toggle, sync mode), content characters, Dance Party theme, docs | Peter tries it |

## Social mode and milling (Peter, 2026-11-18)

(History: the crew-wide social modes below, pickSocialMode, planStage and scatterSpots were removed 2026-11-27;
mingle is the only social mode. See "Mingle only, one personality".)

"I'd like the dancers to spend a lot more time facing each other and dancing with each other ... and then sometimes
they face the audience"; "more like people milling around" (not lined up); "fade the disco floor sooner behind the
dancers".

- **Social mode** (`reforce.Dancer.Social`, card "Social": auto | always | partners | call | facing | cypher | off).
  Every 8 bars the crew picks a mode (engine::pickSocialMode; auto: about 3/4 social, never two audience segments
  in a row):
  - *partners*: pairs face each other and dance the first one's routine as mirror images;
  - *call & response*: one dances (half turned to the audience), the partner watches and hypes them (claps, points,
    raise the roof, nods; engine::dance::reactionPose); they swap every 2 bars;
  - *face to face*: pairs face each other, each with their own moves;
  - *cypher* (3+): an arc facing the middle, one dancer solos in front to the audience, the rest cheer; the solo
    passes on every 2 bars;
  - *show*: facing the audience (where the Moves crew setting applies).
  With an odd count the last three make a trio. Changing source (own, leader, reacting) blends over 0.35 s; turning
  takes about half a second.
- **Milling** (engine::scatterSpots): dancers (and in social modes, the pairs) stand at scattered spots on the floor,
  at least 0.75 apart (depth counts half: one behind the other still overlaps on screen); each wanders to a new spot
  every 3-5 bars on its own schedule, the spots drift a little all the time, pairs stand at a random tilt. They walk
  at most 0.75 units/s with a bounce in the step, turned partly towards where they are going.
- The floor now fades out between z -1.05 and -2.3, just behind the furthest dancers (z -0.75).

## Walking steps and smoother transitions (Peter, 2026-11-18)

"Explore real walking steps and perhaps smoother transitions between dance moves."

- **Footsteps** (DancerRun::stepFeet): while a dancer travels, each foot stays planted on the floor (stage x, z) and
  the body moves over it; on each 8th note the foot furthest from where the dance wants it (a little ahead in the
  direction of travel) steps there in 0.4 beats with a lift. Blended in and out with how fast the dancer moves, so
  dancing in place keeps the moves' own footwork. Test: a planted foot never slides; several steps per walk.
- **Moves made continuous**: 25 moves had jumps (a sawtooth wrapping, an arm or foot swapping sides at full extension,
  a cycle shorter than its own sway, a full turn wrapping to 0). Stepping moves (step-touch, grapevine, cha-cha) are
  now keyframed footwork (each foot moves spot to spot with a lift), side swaps are eased (`sideWeight`), held poses
  (freeze, dab, spin) release before the cycle ends. Test: every move, every variant, < 0.14 units a frame at 60 fps
  / 120 BPM, also across the wrap (the robot's deliberate snaps are the fastest).
- **Routine changes**: the next routine is scheduled a beat ahead and crossfades over two beats centered on the bar
  line (smootherstep), the incoming one playing the end of its cycle so it arrives in motion; a drop's jump still
  cuts in. Turns blend the short way round.
- **Source changes** (own -> a partner's routine -> watching): inertial blending (the offset from the last pose and
  its velocity decay with a critically damped spring, ~0.4 s) instead of a fade from a frozen pose.
- Measured (frame-to-frame, 9 styles x 4 seeds x 4 min): at routine changes the 99th-percentile jump went from 0.15
  to 0.04 units a frame (inside routines 0.05), the median acceleration from 2x to ~1.4x that inside routines.

## Humanising (Peter, 2026-11-19)

"Add subtle randomization to the movements to make them more human." `reforce.Dancer.Human` (0 = machine-exact,
1 = as designed, 2 = sloppy; the Human slider). Everything is seeded per dancer and smooth (quintic value noise, no
per-frame jitter), so it reads as personality, not shake:

- **Timing**: each dancer dances to its own beat: a fixed lean (up to ~20 ms behind or ~15 ms ahead at 128 BPM;
  half that for snappy styles) plus a slow drift of +-0.015 beats and a beat-to-beat variation of +-0.008 beats
  (each hit lands a few ms differently). A crew in Together mode no longer moves as one.
  - Per body part (2026-11-19): the core (hips, chest, feet, face) is on the dancer's beat; the arms trail it by up
    to 0.03 beats (~14 ms), each hand by its own amount (+-0.008), and the head by up to 0.02 beats: the same moves
    sampled that much earlier. Always behind the core, never ahead of the clock the routines were planned to. The
    feet stay with the core (no sliding).
  - Feel within the beat (2026-11-19): the moves run on `danceBeat` = b + s sin(2 pi b) / 2 pi of the dancer's beat
    b: the hits stay on the beat, but a punchy dancer (s > 0, up to 0.15) snaps through them and hangs between, a
    laid-back one (s < 0) lands softly and hurries between. s drifts +-0.05 bar by bar; |s| <= 0.4 keeps the clock
    monotonic.
- **Size of the moves**: per-dancer gains on the arms (0.82-1.15), hips and chest (0.8-1.25), knee bend and foot lift
  (0.85-1.15) and head (0.7-1.3), one hand livelier than the other (+-10 %), an effort level that drifts +-12 % over
  a few bars, and hands that never hit the same spot twice (a slow wander of a centimeter or two).
- **Posture**: a slouch or an upright chest, a head tilt, a nod, weight on one leg, a narrower or wider stance;
  breathing (a 3-4.6 s chest rise); the head looks around slowly.
- **Follow-through**: the hands and head trail the body through a spring (damping 0.75, ~40-55 / 22-30 rad/s, stiffer
  for snappy styles) so hits overshoot a touch and settle. Head turns (spins, looking at a partner) stay crisp.
- **Not touched**: the feet's floor positions (no sliding) and the body's travel; the face.
- Test: two dancers sharing a leader with Human 0 are identical; with Human 1 they differ (mean part offset ~0.012
  units) but are no jerkier, every part stays finite and above the floor, and the dancer's clock never runs backwards.
  The dance clock: Human 0 is the music's beat exactly; with Human 1 and 2 it never runs backwards and stays within
  0.4 / 2 pi beats of the hits' clock; across 40 dancers the feel goes both ways.

## Arms not through the head (Peter, 2026-11-19)

"I notice the arms going through the dancers own heads quite a bit." The cartoon head (radius 0.18) is nearly as big
as an arm is long (0.235), so many moves as designed put a hand, forearm or elbow inside it: 19 % of frames across
all styles, up to 17 cm deep.

- `buildBody` keeps the forearm, the hand and the upper arm (from a third of the way out) outside the head ball
  (1.02 R plus the limb's radius): the whole arm, its bend kept, swings about the shoulder away from the head by the
  smallest angle that clears it (a coarse scan, then bisection). "Away" is the nearest way out across the arm, with a
  gentle preference for out to the own side and forward, so it stays steady where the arm reaches along the head.
- Stateless, so where a move's path crosses deep through the head the way out can switch sides: a hand may step a
  few cm in one frame (fist pumps, the tut). Two moves that asked for impossible poses were changed: Hands up is a V
  beside the head (an arm can't reach over a head this big), the Sprinkler's other hand is by the ear, not inside
  the back of the head.
- Test (28 samples a beat, every style, 3 dancers each, 160 beats): over 5 mm into the head in 4 of 120960 frames,
  at most 2.2 cm (a graze; a forearm is 8 cm thick); a hand's step from one frame to the next at most 6.6 cm more than
  the move itself.

## Not walking through each other (Peter, 2026-11-19)

"Make the dancers not pass through each other when moving... maybe this ends up being some complex path planning
problem." It doesn't need path planning: the floor is open and there are at most a dozen people, so local steering
as in crowd simulation is enough (engine::avoidEachOther before the step, engine::keepApart after):

- **Anticipation**: for each pair that would come within two body radii (0.42) in the next 2 s at their current
  velocities, both steer away from where they would meet, harder the sooner it is (time to collision). Head-on, both
  also keep to their right, so they pass instead of stalling face to face. The one walking takes most of the
  avoiding; someone dancing on their spot gives way a little and then drifts back.
- **Backstop**: any pair closer than 0.38 is pushed apart directly (someone popping in on top of a passer-by).
- Measured (tests): head-on swap, a four-way crossing, walking through someone standing: closest 0.37 by steering
  alone, the backstop never needed. A crowd of 3 / 6 / 9 through every social mode for two minutes: closest 0.355 by
  steering, backstop never pushing more than 4 cm. Without this the same crowds came within 1 cm of each other, and
  9 dancers were inside each other (< 0.2 apart) in 1126 of 7200 frames.

## More variety (Peter, 2026-11-19)

"A bunch more variety in the visual appearance of the dancers." Chosen from a list of ideas: generated characters
dressed for their style, body proportions, prints, fabrics that use the visuals as lighting, sunglasses (with
variety), glow sticks, and faces.

- **Generated characters** (engine::generateDancer; `Random="1"` scene files, one per style): a first name (86),
  a body type (stocky / lanky / in between, head size .85..1.3), skin (ten natural tones from light to dark, chosen independently of gender, style and hair),
  hair (natural, or dyed: often for rave / pop / robot), a color scheme (a base hue and a harmony: complementary,
  analogous, triad or mono, with the style's saturation and brightness: neon rave, candy pop, muted chill, grays
  and one accent for robot, reds / black / gold for latin, 70s oranges / teal for retro), the style's wardrobe
  (weighted outfits, hats, hair, prints and fabrics; metallic tops are often gold or silver), sunglasses (each
  style its favorite shapes; mirrored lenses), glow sticks (rave mostly) and a face. Tall hair never meets a hat;
  beards only with natural hair. The same seed gives the same character; each slideshow start is a new seed.
- **Proportions** (Head, Legs, Torso, Shoulders, Belly, Limbs): the skeleton and the part radii scale; hair, hats,
  ears, nose and glasses follow the head size.
- **Prints** (drawn in the shader, free): stripes, dots, plaid, gradient, zigzag, in the part's own coordinates
  (around and along it), antialiased; a dress takes the top's print.
- **Fabrics**: shiny (a tinted reflection of the visuals: the light texture sampled along the reflected view;
  a studio sky on a dark frame), sequins (facets of the print grid, each sparkling on its own beat), velvet (dark
  face-on, a sheen in its own color at the edges), neon (unlit, saturated, pulsing on the beat: glow sticks too).
- **Sunglasses**: flat lens discs in front of the eyes cut to their shape in the shader (round, aviator, visor:
  one wide lens, heart, star, cat-eye: the outer corner swept up, square), a frame rim drawn on the edge, a bridge
  and arms to the ears; dark, tinted or mirrored lenses.
- **Faces**: eye shapes (oval, round, sleepy with a lid line, happy arcs, big with two highlights, dot), lashes,
  brows (normal, thick in the hair color, thin, arched), mouths (smile, grin with teeth, small, smirk), lipstick,
  freckles, blush, beards (stubble, moustache, goatee, full; 3D and more since 2026-11-21, below).
- The 12 named characters keep their looks and gain a signature touch each (Coco sequins and lipstick, Gus a
  moustache, Ozzie a mirrored visor and glow sticks, Kiko heart shades, Moss square shades and dot eyes, ...).
- Cost: nine generated dancers wearing everything (240 parts) + the floor, 0.77 ms GPU at 1080p. With the new keys at
  their defaults the old characters render the same (largest difference 12/255 on a thin ring along head outlines,
  from reordered shader maths).

## More hairstyles (Peter, 2026-11-20)

"A bunch more hairstyles." Eight more, from the same pieces (18 in all):

- **Curly**: a close shell under ~20 small balls over the top, back and sides (a Fibonacci spread with the face
  and the forehead left out): a lumpy outline, looser than the afro.
- **Curls / BigCurls / HugeCurls** (2026-11-27): one long curly mane in three sizes, curly masses that merge into
  one lumpy outline over a curly curtain, close on top and widest at the shoulders, the lower ones swinging. The
  smaller sizes stand out less from the head, sit a little forward (framing the face) and are shorter.
- **Pompadour**: tight short sides and a big roll (a wide capsule) rising up and forward over the forehead.
- **Side part**: short hair under a second layer cut by a tilted plane, so the hairline runs from high over one
  brow down over the other brow and the outer corner of that eye. (Strands laid along the head looked beaded.)
- **Undercut**: the sides and back a thin shell in a fade color (hair over skin), a thick cap on top (its cut edge
  shows as a solid slab) and a flick at the front.
- **High top**: a tall flat-topped block (a capsule clipped flat, its inside shaded as solid hair), faded sides.
- **Bowl**: a shell cut level all round just above the brows.
- **Mullet**: short on top, a flattened mass hanging down the back of the neck with two tapering side pieces.
- **Shag**: a full mass with pointed tufts flicking out round the back and sides at the jaw and wispy bangs.

Generated characters wear them by style (pompadour, shag, mullet, bowl for retro; high top, undercut for hip-hop;
side part for pop and rave; curly for disco and latin; big and huge curls for disco and retro, long curls now and
then everywhere; ...). Curly, pompadour, high top and undercut count as tall
hair: never under a cap, beanie or top hat.

## Better everyday hair (Peter, 2026-11-21)

"Better looking variety of normal hairstyles for men and women; remove or edit existing styles." From a contact
sheet of all 33 styles on one character (`--dancer-test pop@STYLE`, `reforce.Dancer.Height` .75): every style read
as a smooth plastic helmet, the long styles (bob, long, wavy, bangs) were one ball round the head, several styles
looked the same from the front (short / crew / slick back; ponytail / braid / side pony / man bun / mullet) and a
few did not read at all (pompadour a ball on top, balding ear muffs, curtains a hood, pixie and shag fringes loose
teeth, locs sticks).

- **Hair material** (`Material::Strands`, every hair part; skirts and hats keep `Hair`): fine strands (periodic 1D
  value noise, faded where they would alias), a sheen band across them (Kajiya-Kay, broken up strand by strand)
  and, per part (`BodyPart::strands`), which way they run: along the axis (from the crown down a shell, down a lock),
  parted (combed sideways from a front-to-back parting, with the parting drawn as a line of scalp), combed back,
  360-wave rings, coils (a nubbly texture, no lines); a wavy flag. Very dark hair colors now read on the dark stage. A second
  clip plane (`clip2`) per hair part. Packed into slots only the head reads (face.w, eyes.w, the lips); ported to
  the WebGL preview.
- **Long hair** (bob, lob, long, layers, wavy, long curls, bangs, shag, half-up): a close top (parted) over a
  curtain: a tube round the head, open in front of the cheeks (its inside shows as the hair behind the neck), cut
  level at the hem by `clip2`, swinging with the dance. Layers: the hem slants up to the front. Wavy, shag and long
  curls flare out towards the ends (curls: coils and a ring of ringlet ends).
- **Lifts** (side part, quiff, pompadour, slick back, undercut, pixie's fringe, messy): a layer like the head's
  ball, off-center, so it stands off the head where it should (the front, the top) and sinks into the hair under it
  elsewhere, cut by planes across the forehead (and along the parting).
- **Merged / reworked**: crew is short (`crew` still parses); side part is now a classic short side part (the
  emo sweep is gone); pompadour and the new quiff rise from the forehead; balding is a smooth band round the back and
  sides; curtains are short at the sides; high top is lower, a little wider at the top; ponytail is high; pigtails
  low, parted; braid a side braid over the shoulder; man bun at the crown; locs 22 ropes plus two each side in front
  of the ears.
- **New** (41 styles): quiff, messy, coils, waves (360), lob, long curls, layers, half-up, low bun. `quiff` and
  `waves` now name their own styles (they were aliases of pompadour and wavy).
- **Generated characters**: every dance style draws on everyday hair, each with a small flavor of its own; any
  hairstyle can turn up on any style and any skin tone.
- **Cut the same day** (Peter, from the contact sheets): locs, low bun, long curls, wavy, lob, crop and messy. Their
  names still read, as the closest style left (wavy, locs: long; lob: bob; crop: short; long curls: curly; low bun:
  ponytail; messy: anime), so saved guests keep a look; the wavy strand flag went with wavy.
- **Anime** (in place of messy, "more like anime hair"): a full cap under big pointed clumps, each flat against the
  head and tapering to a point: a fringe of five over the forehead (straight pieces from under the cap: a bent
  lock's round joint stood out as a ridge along the hairline), a lock each side from the temple past the cheek to
  the jaw, flicks sticking out round the back, and a cowlick on the crown; the side and back locks bounce with the
  dance. Under a cap or a beanie it is short. 35 styles.

## Feminine or masculine generated characters (Peter, 2026-11-21)

"A better way to divide up the traditional men's vs women's hair." Generated characters picked hair, outfit,
beard, lashes and lipstick each on their own (pigtails with a full beard and a suit). Now each generated character is
feminine or masculine, half and half, drawn first (only for randomly generated characters: stream guests and scene
files combine hair, outfit, beard and face freely):

- Only feminine ones wear dresses, skirts and long dresses (their weights x1.5), lashes (65 %) and lipstick (38 %,
  disco 70 %); only masculine ones have beards (38 %, retro 70 %; natural hair only, as before).
- Hair leans (engine `hairLean`): masculine short, buzz, side part, quiff, pompadour, slick back, undercut, waves,
  balding, man bun, mullet, bald, bowl; feminine bob, long, layers, bangs, half-up, ponytail, side pony, braid,
  pigtails, bun, top knot, space buns, pixie; either curly, afro, coils, shag, curtains, spiky, mohawk, anime, high
  top. Each character's own lean and either's at full weight, the other's at 0.15 (a woman with an undercut, a man
  with a ponytail now and then). Robot gained a ponytail and a bun so feminine robots have hair of their own.
- Masculine shoulders .9-1.15 -> 1.0-1.15, feminine .88-1.02. Height (`Tall`): masculine .93-1.1 (lanky 1.05-1.12),
  feminine .88-1.05 (lanky 1.0-1.07): about 6 % apart on average. Tall hair under a hat: short (masculine) or long
  (feminine, hanging out below).
- First names in three lists (feminine, masculine, either); each picks from its own and either.
- `DancerSpec::feminine` records it (tests, statistics).
- No named characters anywhere (Peter, 2026-11-21: "delete the named characters entirely"): the built-in samples
  (Ruby, Dex, Kiko, ...) behind `--dancer-test` and the tests are gone; `--dancer-test STYLE` is a generated
  character seeded by its place in the list. (With the big generated heads arms grazed the head in 0.1 % of frames;
  with heads 0.95-1.05, 0.003 %: the test's 0.01 % stands.)
- Stream guests: nothing about body size or shape is guessed from their photo (Peter, 2026-11-21); Height is a
  slider in the /dance page's editor (0.9-1.1, 1 until moved); no build setting (the guest service no longer takes one).
  Their other proportions stay 1. The page's preview frames a fixed height (room for a 1.1 guest with an afro; more
  only for a top hat on a tall one), so Height shows as standing taller or shorter, not as the camera zooming.
- Not a setting, and stream guests choose their own look: their hair menu (the guest page) is grouped by
  kind (short, long, tied up, curly and textured, bold), not by gender.

## Hair variety per style, masculine-only styles, shaved (Peter, 2026-11-21)

- **Masculine only** (never on a feminine character): bald, balding, man bun, slick back, buzz, quiff, undercut,
  shaved, mullet, bowl, pompadour. **Feminine only**: bob, layers, half-up, braid, bangs, pixie, pigtails, bun, side
  pony, space buns, and the long curls (curls, big curls, huge curls). Short, side part, waves, spiky and mohawk lean masculine and long, ponytail and top knot
  feminine: on the other side's characters now and then (x0.15). Either way: curly, coils, afro, curtains, shag,
  anime.
- **Shaved** (new, masculine): a close shell barely darker than the skin, smooth (`kStrandsNone`: strands or the
  coil texture read as a cap). 36 styles.
- **Every style draws from the same everyday hair** (long, bob, ponytail, layers, half-up, bangs, bun, braid, pixie,
  side pony, pigtails, top knot; short, side part, quiff, buzz, undercut, shaved, slick back, waves, balding, man
  bun, bald, mullet, bowl; curly, coils, afro, curtains, shag, anime, spiky, mohawk, high top), plus a small flavor
  of its own on top (engine `generateDancer`); every hairstyle stays possible for every style, independent of skin
  tone.
- **The hat comes first**; under a cap, beanie or top hat tall hair is simply not picked (it used to become long or
  short, so long and short swamped hip-hop).
- Generated heads: 0.95-1.05 (Peter: no small / medium / large heads; they were 0.85-1.3, mean about 1.07, since
  the 2026-11-19 variety work).
- **High top cut** (`hightop` reads as coils). 35 styles.

## Fewer sunglasses, everyday glasses (Peter, 2026-11-21)

A third of generated dancers wore sunglasses (and nobody wore ordinary glasses), hiding a lot of faces on a dark
stage. Now about 19 % wear sunglasses (most in popping 35 %, disco 30 % and rave 40 %; everyday styles 8 %) and about
9 % ordinary glasses (clear lenses, just the frame: round and square mostly, cat-eye on feminine characters now and
then, an aviator rarely; black, tortoiseshell, gold, silver or the scheme's second color; a little more often for
lock and chill), the same for feminine and masculine characters.

## 3D beards (Peter, 2026-11-21)

"Broadly make beards look better": hair material, 3D structure, more options, aligned with the mouth, ears and hair.
They were painted on the face; now all but stubble are built (DancerRig buildBody, after the hats), in the hair
color, drawn with strands (Material::Strands), laid out in the face's own angles (drawFace: u around, v up):

- **Moustache**: a center piece under the nose and a tapering, drooping piece each side to the mouth's corners,
  flat against the face and tall enough to read as hair. **Handlebar**: slimmer, the ends curled up.
- **Goatee**: a rounded tuft set into the chin, a little pointed below. **Circle**: moustache and goatee joined
  down the sides of the mouth.
- **Full**: the moustache over a beard that grows out of the skin: a ball like the head's, shifted down and back,
  poking out of the head only on the lower face; where the two surfaces cross is its edge (zero thickness there:
  no gap), under the mouth at the front and rising to the ears, thickest at the chin and under the jaw; cut away
  behind the ears. (First a shell standing off the face cut by planes: it hovered, the cheek showing under its
  edge.) **Short**: close-cropped, the same but thin, with a slimmer moustache (in place of chinstrap, cut the
  same day). **Long**: full, and a hank hanging from the chin that swings.
- **Stubble** stays painted (a shadow), now on the full beard's area and the upper lip.
- **The mouth** sits a little lower (v -0.35, was -0.30) under any moustache, clear of it; open mouths stay above
  a full beard's line.
- **No sideburns**: strips stood off the head like sticks, a slice of a shell made a big patch in profile, and a
  band round the head would show over a bald crown; a beard ends at the jaw in front of the ear.
- Pieces on the face (moustache, goatee, circle, handlebar) are centered on the face's surface (flat against it), so
  their edges meet the skin.
- Glow sticks: generated house dancers only, about 1 in 5 (40 % in rave gear, 11 % otherwise); guests can't choose
  anything to hold yet.
- Test hook seeds: `--dancer-test house#8@short@none@full` (a chosen generated character).
- Generated (masculine only): stubble, moustache, goatee, full, circle as before, plus handlebar (retro, disco),
  short (hip-hop most), long (now and then). Test hook: `--dancer-test pop@short@none@full`.

## Hat-aware hair (Peter, 2026-11-20)

"I've seen some big hair poking out beyond hats." Checked every hairstyle under every hat (`--dancer-test
pop@HAIR@HAT`): bob, long and shag (allowed by the generator) and the tall styles (kept from hats by the generator,
but not in scene files) poked through caps, beanies and top hats; headphones are fine with everything.

- Under a cap or a beanie, big hair is built to hang out below the hat: bob, long and shag become a close layer, side
  locks from under the band and a back piece (shag keeps its tufts, loses its bangs); afro and curly become puffs /
  curls round the sides and back below the band; pigtails a close layer and the two tails.
- Then fitHairUnderHat (every frame, after the hair swings; stateless, no allocation): each hair part without a clip
  of its own is cut at the hat's band; a part mostly above the band (a spike, a bun, the pompadour roll) is left out;
  a part whose surface at or above the band would still be outside the hat shrinks until it fits (a shell towards
  the head center, a mass or lock towards its lowest point), or is dropped if it would have to shrink by over 55 %.
  Top hats keep the usual shapes (hair shows round and below the brim) through the same fitting.
- Test: every hairstyle under cap, beanie and top hat, at rest and through 5 s of dancing with swinging hair: no hair
  point outside the hat as built (its dome above its cut, or its crown above the brim). Without the fitting it fails
  84 checks.
- Cost: buildBody 1 us without a hat, about 12 us with a cap or beanie, 6.5 us with a top hat.

## Hair colors (Peter, 2026-11-20)

Natural hair: 12 colors, equally likely: black, soft black, dark brown, brown, chestnut, auburn, copper, dark blond,
blond, gray, silver, platinum (silver and platinum moved here from dyed). Dyed hair is a bright color of any hue and
only rave (50 %), robot (30 %) and house (20 %) dancers get it; pop, hip-hop, disco, retro and chill are always
natural. Beards still come only with natural hair.

## Latin style removed (Peter, 2026-11-20)

"I'd like to eliminate the latin style completely." Style::Latin is gone (8 styles), with `Random Latin` and the
five moves only it used (salsa, salsa turn, merengue, samba, the Latin frame arms): 76 moves now (26 footwork, 30
arms, 20 signature). Shared moves it had (latin side step, cha-cha, maracas, shimmy, conga, hand jive, ...) stay with
their other styles. Vera is now a disco dancer (same look). A saved `reforce.Dancer.RandomStyles` naming `latin` is
read without it; a scene file with `Style="latin"` gets the unknown-style warning and the default style.

## Roster trimmed (Peter, 2026-11-20)

Reviewed on a roster page: Gus, Kiko, Luna and Ruby were discarded (their scene files removed); Coco, Dex, Moss, Nia,
Ozzie, Pip, Theo and Vera stay. Pop, retro and chill now have no named character, only their `Random` files.
(The `--dancer-test` hook's built-in looks, engine::sampleDancer, were removed 2026-11-21 with the last named characters: the hook and the tests use generated characters.)

## Random styles for the next dancer (Peter, 2026-11-20)

"Select a subset of random dancer styles that will be selected for the next one." `reforce.Dancer.RandomStyles`
(`all` | `none` | a comma list such as `disco,rave`; beatset `dancerandom`), toggles under Max dancers on the
Dancers card. When Start next or the slideshow picks the next dancer file (Director::nextId), a `Random STYLE` file
in a style switched off is skipped and the shuffle moves on; named characters are never skipped. A file's style is
read once (Visualizer::dancerFileAllowed). Picking a file by name still starts it.

## Hair in motion (Peter, 2026-11-20)

Since 2026-11-27 the classic swing (`reforce.Dancer.Hair` 0): hair physics (chains that bend, collisions, loose
volumes) is the default, see [Hair physics](#hair-physics-peter-2026-11-27) below.

Hanging hair swings with the dance: ponytail, pigtails, the back of long hair, the mullet and the shag's tufts.

- Each dancer has two hair ends (DancerRun::stepHair), behind and below the left and right of the head, each a
  point on a bouncy spring (about 2 Hz, damping 0.22; the right side a little faster, so pigtails don't swing as
  one) pulled along by where the head carries it, damped relative to the head, sub-stepped at 240 Hz. Gravity pulls
  the target partly towards straight down, so when the head tilts the hair hangs the other way.
- The lag of each end behind the head, in the head's frame and per unit hair length (held to 0.9), is
  `DancePose::hairSwing[2]` (set after the pose is made: not one of the pose floats, not mixed or mirrored).
  buildBody swings the free end of each hanging piece by it, keeping the piece's length: pigtails and mullet sides
  their own side, the ponytail, the long hair and the mullet's back the average (the heavy ones by half), the shag's
  tufts a little. A swing into the head or neck is held back.
- Spins fling the hair out; robot snaps leave it behind for a moment; a bounce makes it bob. Entrances start it
  still.
- Measured (pigtails, every style, 20 s at 120 BPM): mean swing 0.08-0.24, at most 0.64; the two sides differ.

## Hair physics (Peter, 2026-11-27)

"Right now hair is pretty static and doesn't do much in response to head spins or arm motions." Wanted: more motion
detail (hair that bends, whips and trails instead of swinging stiffly) and arm and body collision (hands and arms push
hair aside, hair rests on the shoulders and back instead of going through them). Then: "make shorter styles move
too", subtly. Why it looked static: two end points for the whole head, driven by the head only (not the arms, not
the body), each hanging piece swung whole as one stiff capsule, nothing pushed it, and short and big hair (afro,
curls, buns, spikes, lifts, every close cut) did not move at all.

`reforce.Dancer.Hair` (0..2, default 1; Hair motion slider under Look on the Dancers card; `beatset dancehair`):
0 = the classic swing (as before, same parts), 1 = hair physics, 2 = looser (softer shapes, twice the volume lag).

- **Chains** (DancerRun::stepHairSim, stage space): each hanging piece is a chain of points pinned at its root on the
  head: tails (ponytail, pigtails, side pony) and the braid 4 links, the mullet's back and tufts and the locks under
  a cap or beanie 2, the long hair (bob, long, bangs, layers, half-up, shag) one 2-link chain for the part below the
  head; up to 4 chains a dancer. buildBody lays each piece out (root, rest tip, thickness, how much it droops) in
  `BodyOut::hairChains`; the sim reads that once from a rest build, so the shapes live in one place.
- **Each step** (sub-stepped at 120 Hz; the head frame and the body between frames interpolated): Verlet with air
  drag and gravity (6 units/s²); each point's motion relative to the one above it damped (hair is lossy: a swing dies
  in about a second) and held to 5 units/s (a pose that jumps flings the hair, it doesn't throw it over the head);
  each link pulled towards its rest direction (30 rad/s at the root, half at the tip, so a bend travels down the chain:
  whip and trail), below the first link partly carrying on its parent's direction; lengths kept root to tip; no joint
  bends more than 30 degrees (a curve, not a fold); then out of the body, and once more tip to root.
- **Collision** (`hairColliders`, character space, as buildBody builds them): the torso (flattened), the head, upper
  arms, forearms and hands. Each link where it comes closest to a collider, and its end point; the push removes the
  velocity into the body and some along it (it rests there). Each collider is shrunk, per chain, by how far the chain
  passes into it as designed at rest (the long hair's middle runs inside the torso's outline; a pigtail past the
  shoulder; a tail's top along the head), so the rest look stays as designed and only what moves into the hair pushes
  it. Above the floor too.
- **Upside down / near the floor**: as the head turns over the rest shape gives way to hanging straight down (the
  pull weaker), so the hair falls off the head, never through the floor. Test: tipped into a handstand the tips hang
  below the roots, all above the floor.
- **Long hair** (LongHair 0, as first built): the tube round the head stays one piece and turns about its top 60 %
  of the way towards the chain's end (pieces of a tube this wide showed their joins as dark shards through the opening
  at the face; all the way would show the tube's inside round the head). It trails, settles and is pushed by the arms
  but does not bend: bending below.
- **Loose volumes** (Peter: shorter styles move too): one lag per dancer, a point after the head's center (3.5 Hz,
  damping 0.3) and a frame after its turn (2.5 Hz, 0.35), both relative to the head's own motion plus a little air
  drag (a steady spin trails them a little), held to 0.3 head radii and 0.35 rad. buildBody moves loose parts by it,
  each by its own share: afro lobes (the middle 0.3, the outer lobes 0.6: the outline wobbles), curls (more on top),
  coils, buns, top knot, space buns, the half-up bun, spike and mohawk tips, the anime locks' tips and cowlick, lifts
  (pompadour, quiff, side part, slick back, undercut top, fringes; their hairline cut stays with the head), and every
  close shell turns a little (12 % of the turn: the hairline and strands drift a few millimeters; a shift would show
  the scalp). Hats don't move.
- **Classic kept**: Hair 0 passes no sim to buildBody: the same parts as before. DancePose::hairSwing is still worked
  out (the anime side and back locks still use it, plus the lag).
- Entrances start still (the chains start in their rest shape when the dancer appears); a change of size (the pop,
  the crowd scale) resizes the state, so it doesn't fling; a long frame (over 0.1 s) or a jump of the head (over half
  the dancer's size) restarts from rest. Deterministic (the determinism render test passes).
- Measured (13 hanging looks x all 543 mocap moves, 77000 s at 120 BPM): mean swing (tip off where it settles, in the
  head's frame, per unit length) 0.21-0.30 (classic 0.21 on the same moves), the busiest move 0.54-0.76. Into the
  body (each link at five points, the colliders as the sim sees them): over 1 cm in 0.17 % of frames, arms p99
  1.8 cm, p99.9 3.2 cm, worst 4.4 cm (arms), 4.8 cm (head), 7.7 cm (torso, a waacking move under a long hair chain);
  hand-made moves: over 1 cm in 3 of 74100 frames, worst 2.8 cm. Mostly where a joint can bend no further, so a link
  can't wrap round an arm.
- Cost: CPU per dancer per frame (hand-made moves, 60 fps) pigtails 0.55 -> 5.1 us, long 0.53 -> 2.3 us, mullet 0.53
  -> 3.4 us (step; building the parts ~1 us either way); a crowd of 30 adds about 0.06 ms a frame. GPU at 1080p:
  hanging pieces are more parts (pigtails 27 -> 33, mullet 26 -> 29, long the same): five dancers within noise
  (0.79-1.10 ms either way), thirty 2.49 -> 2.56 ms.
- Web preview: the same engine code (DancerRun), so `tools/dancer_web/build.sh` carries it; checked in node (every
  hanging style steps, all finite).

- Tests: tests/test_engine_hair.cpp (layout, every hanging look through the hand-made routines: finite, no jumps, out
  of the body; a spin flings the ponytail out and the volumes lag; upside down; entrances, stalls, Hair 0 and back;
  determinism; loose volumes move hair parts and never the head). Measurements: `reforce_engine_tests -tc="hair
  physics: measure*" --no-skip`.

### Long hair that bends (Peter, 2026-11-27)

"Long hair still doesn't really move": swings, bends and trails on spins, flicks where the arms push it, falls upside
down, the curls riding it. `reforce.Dancer.LongHair` (1 default = bends, 0 = the rigid turning tube above, rendered
exactly as before: offline frames byte-identical to main, Hair 1 and Hair 0); settings.json and `beatset
dancelonghair on|off` only: Peter compared, kept bending and dropped the panel selector (2026-11-27). Only with hair physics (Hair 0: the classic swing, never bent).
The long styles built on the curtain (long, bob, bangs, layers, half-up, shag; curls, bigcurls, hugecurls).

- **Why not pieces**: separate parts meet with round caps whose insides show through the face opening (the shards),
  and overlap or clip planes can't hide a seam both inside and out. So the tube stays **one part, drawn bent**: a
  second mesh (the capsule with 24 rings along its band, `capsuleMesh(32, 12, 24)`) and a vertex-shader bend
  (DancerPipeline `bendHair`; `bendHairPoint` the same in C++; the WebGL port in dancer_gl.js), one extra draw per
  pass for the bent parts (packed last). No seams anywhere: every point of the one surface moves continuously; the
  clip planes (face opening, hem, the layers' slant) are in the part's unbent local space, so they bend with it.
- **The bend** (`HairBend`, in `BodyOut::bend`; the instance grows 6 float4s, 304 -> 400 bytes): rigid with the head
  down to `y0`, below that each point follows one of three columns'
  curves, blended across the tube by local x / radius (back at 0, left / right sides at +-1). A curve is a cubic Bezier
  leaving the rigid part along the axis (no kink at y0), ending at the column's end along its last link (fitting the
  middle as well overshot into loops and shelves). A wall point is its column's anchor moved along the curve with its
  offset from the anchor turned as the curve turns (so a wall keeps its width; turning about the tube's axis instead
  lifted and dropped the side walls). Normals turn with it. Still, the bend is the identity (tests: < 0.2 mm).
- **Columns**: three chains (back, left, right; 3 links; root on the tube's wall at y0; parallel to the axis, the
  tube's own widening spreads the hem), all kept out of the head. `HairChainDef::cone`: the column's end may turn
  at most 66 degrees from hanging (link i i/3 of it), past that it turns back softly (30 % a sub-step: a hard limit
  fought the collisions and popped); upside down the cone opens towards 166 degrees, more near the root, so the hair
  falls. The first link (by the shoulder) doesn't collide with the arms: an upper arm raised through it flicked the
  whole side column round from its root in a frame; the arms push the lower two links.
- **Calmer, from higher** (Peter after trying it: "a bit calmer", and "could start higher vertically"): the bend
  starts 0.45 head radii below the tube's top (about ear height; first 1 radius, below the jaw), shape stiffness
  0.7 -> 0.9, the cone 80 -> 66 degrees, the relative damping x 1.35 (`HairChainDef::damp`; tails 1). Still looks the
  same (the bend is the identity at rest; rest views front, 3/4 and back: the settled droop differs by 0.2-5 % of
  the pixels, the curls up to 9 %, no shards, no scalp). Measured (`long hair that bends: measure the swing`; the
  columns' ends off where they settle, head frame, hand-made routines, five long styles; per unit length of the
  column, which is a third longer now): mean 0.169 -> 0.111, p90 0.316 -> 0.209, p99 0.632 -> 0.431, max 1.38 -> 1.07,
  in distance (long) mean about 0.29 -> 0.25 head radii; a hard spin flings the side column out 1.48 -> 1.26 R and it
  settles within 5 % 0.85 -> 0.48 s after; the hem in the spin test 1.18 -> 1.03 R.
- **Curls** (curls, bigcurls, hugecurls): every mass placed with onCurtain goes where the bend puts its rest point,
  so the lower masses ride the bent sheet and the top ones stay.
- **Looks** (contact sheets, scratchpad longhair/): long hair now swings out on spins (the hem up to 1 head radius
  off in a test spin), flicks to the side under waacking arms, wraps round the body and drops towards the floor in
  inversions instead of the rigid tube sticking out; hugecurls' lower masses sway with it. Not perfect: a side
  column pushed forward by an arm can fold so its inside (darker) shows for a moment; a fast flick reads as a flat
  blade from some angles; upside down the sheet curls (a C) rather than hanging straight, and an arm raised at the
  shoulder passes through the side hair (as it did through the old tube).
- **Measured**: hand-made moves, every hanging look (long styles bending): into the body over 1 cm in 3 of 74100
  frames (arms 2.8 cm, head 0.9 cm, torso 1.9 cm worst, the first links' arm contact not counted), largest point step
  0.59 per unit length (as before: < 0.8); the long columns' last link at most 74 degrees from hanging standing. CPU:
  step 2.4 -> 6.0 us per long-haired dancer, build the same (curls +0.9 us); 30 long-haired dancers at 1080p
  (offline, first version): CPU 0.31 -> 0.40 ms, GPU 2.75 -> 2.82 ms a frame; LongHair 0 costs what main did.
- Tests: tests/test_engine_hair.cpp ("long hair that bends": layout, still is the identity, smooth and length-keeping
  bend, normals, the curls ride it, switching restarts from rest, the cone, the bend starts at ear height);
  tests/test_render_dancers.cpp (drawn with the tube mesh: still, 2 of 108,000 pixels differ from the straight tube;
  spun, 15 %; skipped: rest views as PNGs, `REFORCE_REST_VIEWS=dir`). Swing and cost:
  `reforce_engine_tests -tc="long hair that bends: measure*" --no-skip`.

## Floor that reflects a wall of visuals (Peter, 2026-11-19)

"Seeing the visuals through the disco floor is a bit weird. It would be cooler if the floor reflected the visuals as
though the visuals were on a vertical wall behind the disco floor... or add this as an option while also maintaining
current behavior." `reforce.Dancer.FloorLook` = `mirror` (default) | `seethrough` (the original look); Floor selector
on the Dancers card. **2026-11-20: the see-through floor was removed** (Peter: "make the reflective dance floor the
only option"); the setting, the selector and the shader branch are gone.

- Mirror: the floor is opaque out to the frame's sides. Per pixel, the view ray reflects off the floor (it comes from
  the eye's mirror image below the floor) and is followed back to a wall at z = -2 (where the floor fades out behind
  the dancers); that wall point is projected with the stage camera and the visuals there (the full-resolution frame,
  with a little of its blurred copy: slightly glossy) are what the floor shows. Fresnel: stronger towards the back.
  Lit tiles glow on top; the grout dulls the reflection. Behind the floor's edge the visuals themselves are the wall.
- The reflection is of the visuals only (the frame before the dancers are drawn); the dancers aren't reflected yet.

## Mingle (Peter, 2026-11-20)

"I'm curious to explore a mode where they don't all shift from one 'mode' to another basically in sync. That looks
too much like a marching band formation ... something more natural in feel where people are just milling around."
The other social modes are crew-wide: one mode per 8-bar segment, pairs fixed by index (0+1, 2+3), call & response
and cypher turns on one shared 2-bar clock, so every pair re-forms and swaps at the same downbeat.

Social = `mingle` (`reforce.Dancer.Social`): engine::Mingle, a stateful planner in place of planStage. No crew plan;
each dancer is an agent:

- **Activities**, each with its own end (a whole number of beats, sometimes plus the off-beat, from when it began:
  changes land at different moments, not on the bar line): dancing on their spot (to the audience, turned their own
  way), showing off, wandering to a free spot (the one furthest from everyone among a few candidates, not too far a
  walk), taking a break near the sides or the back (moves at 55 %), walking over to someone free (who stops to wait
  and glances at them; they meet at arm's length from the side the walker comes from), walking over to join a group,
  watching someone showing off (in front of them, a little to the side; up to two watchers; the show-off half turns
  to them).
- **Personality** (seeded per dancer): sociable (more joining, longer in groups, more glances), restless (more
  wandering, shorter stays), showy (more showing off). Over many dancers the most sociable third spends ~40 % more
  time in groups than the least. (Since 2026-11-27 one personality for everyone: "Mingle only, one personality".)
- **Groups**: two meet -> a pair at the tilt they met at (partners: mirror images; face to face; call & response,
  swapping on the pair's own 2-bar clock); a pair may change how it dances at its own 8-bar marks. Someone joining
  makes 3-4 a horseshoe open to the audience (members keep their order round it, nobody crosses the middle): they
  dance to each other, and in about half of the group's own 2-bar turns one solos (half to the audience) while the
  rest cheer. Each member has their own time to leave; the last one left of a pair dances a moment and moves on.
- **Glances** (idea 4): every 1.5-6 s (sociable ones more often, in a group half as often, only at outsiders) a dancer
  looks at someone in front of them for 0.8-2.4 s, more likely someone showing off, walking past, or coming over to
  them; the head turns up to ~0.85 rad, eased.
- Its own beat clock follows the music but a jump (new track) moves it by at most half a beat, so schedules survive.
- Measured (6 dancers, 10 min at 124 BPM, simple walkers): alone 47 %, pairs 37 %, threes 12 %, fours 3 %; 653
  changes of activity, 13 % on a bar line; at most 4 of 6 change in any one beat (5 of 406 beats: separate events
  coinciding). With real dancers and steering, 3-12 dancers for 3 minutes: closest 0.355 by steering alone up to 6;
  with 9 or 12 one backstop push in 10800 frames.
- Not done: a drop moment everyone shares (idea 5, left out on purpose).

## Results (2026-11-18)

- All four phases built. 81 moves (29 footwork, 31 arms, 21 signature), 60-155 combinations per style; 12 content
  characters covering every style; Dance Party theme.
- Cost: 0.55 ms GPU for five dancers (113 parts) + the floor at 1080p, 4x MSAA; 0 render-thread allocations.
  2026-11-19: the capsule mesh is drawn indexed (378 unique vertices + 1920 16-bit indices per part instead of 1920
  vertices; pixel-identical output, 12 of 12 frames byte-equal): dancers alone 0.36 -> 0.25 ms for five at 1080p,
  0.53 -> 0.35 ms for nine; at 4K 0.82 -> 0.72 ms (five) and 1.10 -> 0.92 ms (nine).
  Later the same day the mesh went from 20 x 8 to 32 x 12 (858 unique vertices per part) so solid hair masses meet
  the head in a smooth hairline: five dancers + floor at 1080p 0.66 -> 0.83 ms GPU.
- Lighting: the visuals tint the dancers (a red frame makes them clearly redder than a blue one, test); they never go
  darker than 60 % of the studio light so they read on a black frame.
- Not done yet: `Face=` images, per-dancer trails into the feedback. (Hair in motion: 2026-11-20, below.)

## Later

Friends' faces (`Face=` images), props (glow sticks), solos/spotlights, a trail silhouette into the feedback, crowd
mode (many tiny dancers), walk-ons from the side.

## Mocap moves (Peter, 2026-11-21)

"I'm curious to explore some other sources for dance moves." Moves recorded from real dancers, played through the
same rig: the IK still places hands and feet, so feet stay planted and the cartoon proportions hold.

- **Source: AIST++** (Google, 2021; annotations CC BY 4.0, credited in content/README.md): 3D keypoints (COCO-17,
  cm, y up, 60 fps) of 30 dancers in 10 genres: break, pop, lock, middle hip-hop, LA hip-hop, house, waack, krump,
  street jazz, ballet jazz. Every clip is cut to its music: a basic clip (sBM) is exactly 16 beats, an advanced
  routine (sFM) 64, from frame 0 on the beat, at the tempo of its music ID (80-130 BPM, house 110-135). Checked:
  the lengths are 16.0 / 64.0 beats at those tempos, and dancers of the same choreography agree on where the beats
  fall measured from frame 0 (0.6-0.86 phase agreement against about 0.3 by chance). The music itself is no
  longer downloadable (its storage host is gone); it isn't needed.
- **Importer** (tools/aist_import, Python): `retarget.py` turns every frame into size-free channels (DanceClips.hpp
  lists them): the body's spot and heading (the pelvis smoothed over about a beat), pelvis offset and yaw / roll /
  pitch in the body frame, chest relative to the pelvis, head relative to the chest (neutral pitch calibrated over
  the set), each wrist from its own shoulder in arm lengths (chest frame; stretched arms clamped to 1.1), each ankle
  in the body frame (across in stance half-widths, else leg lengths), toes following the knee, heel lifts, and the
  **bend hints**. Joint angles take the smaller of the two Euler solutions and are clamped to what a body does
  (a head turned "backwards" is the tracking flipping); only the body's heading is unwrapped (spins add up).
- **Loops (Peter, 2026-11-21: "16 bar loops")**: a move is a whole clip. A basic move is its 4 bars (one step,
  repeated); a routine its 16 bars (a phrase); a routine that can't be used whole gives its better clean 8-bar half.
  Loop closing: the last 2 beats blend into the start (the rest stays as recorded; a ramp over 16 bars pushed arms
  far out), travel and turning are evened out over the loop, and travel stays within 0.75 leg lengths of the spot
  (eased, at most 1.3x: crowd avoidance only knows spots). Short tracking glitches (a hand, foot or hip jumping 0.6
  limb lengths in a 16th of a beat, an angle 0.8 rad) are bridged by a straight line, up to 4 % of a move.
  Left out: floor work (hips more than 0.45 leg down: freezes, drops; most break routines), lying or upside down
  (break floor work since 2026-11-27 in its own file: "Break floor work" below).
- **Tempo (Peter: "I generally DJ between 120 and 130")**: of each basic choreography's dozen performances, every
  usable one recorded at 120-130 BPM is kept (Peter, 2026-11-21: more performances per basic move, now that the
  dancers use only these; different dancers do the same step differently), at least 2 (the best of the rest by
  score, where a tempo outside 120-130 costs 0.03 per BPM, plus the loop closure, steady tracking and movement).
  Labels: "house basic 1 · dancer 19 · take 2" (takes numbered per dancer, slowest first). The choreographer weights
  mocap moves by tempo too (1 within 118-132, less further off, at least 1/3), since every usable routine is kept.
- Result: 543 moves: 376 basic (4 bars), 123 routines of 16 bars and 44 of 8. Basic / 16-bar / 8-bar per genre:
  house 60 / 13 / 4, pop 40 / 20 / 0, lock 40 / 16 / 4, break 20 / 1 / 6, middle hip-hop 39 / 17 / 3, LA hip-hop
  40 / 21 / 0, krump 37 / 9 / 8, waack 35 / 14 / 5, street jazz 36 / 3 / 12, ballet jazz 29 / 9 / 2. Recorded at
  120-130 BPM: 438. content/Moves/aist.moves, 11 MB: channels quantized to 8 bits over their range within a move
  (about 2 mm on a character, half a degree), the body's heading 16 (spins).
- **Bend hints** (`DancePose::elbowL/R`, `kneeL/R`): the direction the elbow (chest frame) or knee (body frame) bends,
  its length how clear the bend is (0 straight .. 1). The rig turns its own pole (elbows back and out, knees forward)
  towards the hint by that much, so procedural moves (no hints) are unchanged and a crossfade between the two is
  smooth. Mixed, mirrored and blended like the targets (`kPoseFloats` 35 -> 47).
- **Hands on cartoon shoulders**: our shoulders are wider for the arms than a person's (half-width 0.64 arm against
  0.38), so a hand moving in towards the middle is pulled in by the difference (claps meet in the middle), never
  further from its shoulder than the dancer had it or an arm's length. Arms through the head: the rig's swing-away
  (below) handles it as for every move.
- **Engine** (engine/DanceClips): `loadClips` at startup (main.cpp, `<content>/Moves/aist.moves`) adds each loop to
  `moveLibrary()` as a Full move (`MoveDef::clip`), tagged with the styles its genre joins: house -> house, rave;
  break, krump, LA hip-hop -> hip-hop; middle hip-hop -> hip-hop, pop; pop -> pop, robot; lock -> retro, disco, robot;
  waack -> disco; street jazz -> pop, chill; ballet jazz -> chill. Energy by thirds of motion within the genre.
  `clipPose` plays a loop at a beat phase (Catmull-Rom) for any skeleton (lengths times its arm / leg).
- **Choreographer**: `reforce.Dancer.Mocap` (0..1, default 1 since 2026-11-21, Peter: "move to using entirely these
  motion capture moves"; settings.json only: the card's slider was removed the same day, Peter: "if I really want the
  hand-made moves back I can mess with settings later") is the share of routines that are a mocap move of the dancer's style (by
  energy and tempo, never the same twice running). At 1 the hand-made moves are unused (kept: the setting brings them
  back). Still hand-made: the hello / goodbye waves and the reactions (watching, cheering: social modes and mingle).
  A **drop** starts a high-energy (hype) mocap move of the style on the drop, by the same share, instead of the jump. **Routines play whole**
  (Peter, 2026-11-21: "getting the 16-bar routines to always play at least the full 16-bar routine would be good
  ... fine to start them on either 8 or 16 bar points"): a routine (16 bars, or an 8-bar half) is only picked on an
  8-bar point of the phrase (bar 1 or 9) and plays from its own bar 1 to its end (a routine started on bar 9 runs
  over the phrase line to the next bar 9); anywhere else a basic move fills in: on the phrase's 4-bar grid
  (`Routine::origin`; joining mid-cycle when picked off it), finishing its cycle, changing on the phrase line at the
  latest. A drop still cuts in. **Balance** (Peter: routines "50% of the time", then "roughly 60%"): on an 8-bar point a
  routine is started with chance `kRoutineChance` (0.43), else basics, then the move within that group by energy and
  tempo (by count alone routines were 26-42 % of the time). Every routine fills 16 bars (an 8-bar half twice), so the
  share is 2p / (1 + p): measured 59 % in every style (20 dancers x 512 bars each, no breakdowns or drops). In steady play every basic move plays its
  full 4 bars; shorter only after a drop off the 4-bar grid (the next basic joins its cycle to get back on it). No style groove is added on top (the recording has its own).
- **Trying moves** (Dancers card, "Mocap moves"): Style + Move, Prev / Try / Next: `beatset dancetry ID` (or the
  label; `off` to stop; not saved) makes every dancer loop that move from its own bar 1, starting on the next bar
  line, no drops in between.
- Tests: tests/test_engine_danceclips.cpp (the file loads, every genre has basic moves and routines, broken files
  refused, every move finite / above the floor / in reach / continuous round its loop for three body shapes, sizes
  follow the skeleton, hints steer elbows and knees, the Mocap share, trying a move from the next bar line).

## Past guests (Peter, 2026-11-21)

"Keep guests last sign-on available as something I could launch manually and indefinitely." A guest's check-in
puts them on stage for `reforce.Guests.Minutes` (30) and then they leave; before this reforce kept nothing of them.

- **Guest book** (web/GuestBook): every check-in (not a blocked guest's) saves or updates the guest's latest dancer
  as a scene file in `Guests/` next to settings.json: their scene text (look, Style, Name) plus `Guest="<id>"` and
  `Seen="<ms>"`. Files are named after the guest; names need not be unique (the service tells guests apart by a random
  id), so a second "Sam" is "Sam 2.txt", and a guest who changes their name moves to a file of the new name. Written
  to a temporary file and renamed over the old one. Photos are never involved (the service never keeps them).
- **Bringing one back**: the Dancers card's **Past guests** list (newest first, blocked guests left out): name, style,
  when last seen, and **Bring on**, or "on stage" while they dance. `beatset guestcall ID|NAME`. A guest brought on by
  hand stays until sent off or Clear (no time limit), keeps their name tag, and is the same guest as far as reforce
  is concerned (`DancerSpec::guestId`, also read from the file's `Guest=` key): blocking, messages and "checked in
  again (already dancing)" work as for a check-in. Not in the slideshow.
- The book is read at startup even without the network (a test copy can bring guests on); only a real check-in
  writes it.
- **Before the guest book** existed guests are imported from the guest service with `reforce --import-guests
  FILE.json` (a JSON list of {id, name, dancer, at}, e.g. from the maintainer's Cloudflare D1 database: `npx wrangler d1 execute reforce-guests --remote
  --json --command "SELECT g.id, g.name, g.dancer, (SELECT MAX(at) FROM checkins c WHERE c.guest_id = g.id) AS at
  FROM guests g"`): done once (2026-11-21) for the guests from before.
- Tests: tests/test_web_guests.cpp (kept, named, numbered for repeated names, updated, renamed, read back, imported;
  file names from any name).

## Mingle only, watching with mocap moves (Peter, 2026-11-21)

- **Always mingle** ("make mingle the only social mode I do"): the Visualizer plans every step with engine::Mingle;
  the Social and Moves (crew sync) menus are gone from the Dancers card and `reforce.Dancer.Social` / `Sync` are not
  read (the formations and sync modes stayed in engine/Dancers and its tests until they were removed 2026-11-27).
- **Watching is AIST too** ("move to 100% AIST"): measured before, a mingling dancer spent 9-11 % of the time
  watching someone (cheering a soloist, hyping someone showing off, a partner's turn) with the hand-made claps,
  points and nods; now `watchPose` dances a basic mocap move of their own style (calm ones only until 2026-11-27, then any energy:
  Peter, "I don't want to restrict the watchers' moves as much"), a new one every
  4 bars (seeded per dancer); since 2026-11-27 also whole routines (Peter: "watchers should also dance full 16-bar
  routines"): per 16-bar block of the watcher's clock a routine about as often as a dancer picks one
  (kRoutineChance; an 8-bar one plays once and the second half is picked afresh), else basics; standing moves only, never floor work; turned towards the one watched (the stage sets the facing and the head turn). The
  hand-made reactions remain only when the Mocap share is 0. What is left hand-made: the hello and goodbye waves,
  and the walking steps (below).
- **Walking** (unchanged): the stage moves each dancer's spot; the dancer glides there on a critically damped
  spring (at most 0.75 units/s, a body is about 1 tall), turned partly towards where they're going. While moving
  (over about a third of top speed) the feet take real steps: on each 8th note the foot furthest from where it
  should be swings there in 0.4 beats with a small lift, the other stays planted; the body bobs a little on the
  beat. The rest of the body keeps dancing its move. Walking eases in and out (about a quarter of a second), so
  the dance's own footwork returns when they arrive.

## Name tags: guests only (Peter, 2026-11-21)

"Guests name tags always show, and generated dancers tags never show. No need for a control panel item": a tag is
drawn over a dancer whose file sets `Name` (stream guests, past guests brought back), never over generated dancers;
the Names menu (named / all / off, `reforce.Dancer.NameTags`) is gone.

## Jazz split, style buttons (Peter, 2026-11-21)

- **Jazz split in two** (Peter, after the numbers: ballet jazz turns 51 degrees a beat against 11-30 for every other
  style, a foot off the floor half the time, arms up a quarter; street jazz is slow, compact and the sharpest of all,
  closer to pop): **jazz** is street jazz, **ballet** is ballet jazz (`Style::Ballet`; the old "chill" reads as jazz, Peter).
  9 styles. Looks: jazz dresses from the old pop wardrobe, ballet from chill (60 %) or pop. Moves: jazz 36 basic, 3
  routines of 16 bars and 12 of 8; ballet 29 / 9 / 2.
- **Every routine fills 16 bars**: an 8-bar half plays twice (street jazz, with 3 whole routines, still dances
  routines half the time; it measured 40 % with halves played once). **Since 2026-11-27 an 8-bar routine plays
  once** (Peter: don't force repeats; a random one is fine): the next 8-bar point picks afresh. The routine chance
  allows for the candidates' length instead (kRoutineShare 0.6: p = s / (s + (1 - s) E), E their weighted length in
  8-bar units; 0.43 for whole routines, 0.6 for 8-bar ones): routines measured 57 % house, 55 % hip-hop, 58 % jazz.
- **Panel**: the "New dancers in these styles" chips (always all on, Peter: "doesn't do me much good") became one
  **New dancer** button per style (`switch:dancer:Random <Style>`), and the Dancers card has no library list any more
  (only the Random entries were left in it; Past guests has its own list). `reforce.Dancer.RandomStyles` stays, in
  settings.json only, for Start next and the slideshow. The buttons sit three to a row; blocked guests (✕ unblocks)
  are listed at the end of the Past guests section (Peter: blocking won't be common).

## Styles are the dance genres (Peter, 2026-11-21)

"Choosing a dance move genre should be the top-level thing, and then choosing an appearance to go with it should be
secondary." The dancer styles are now the AIST++ genres, two pairs combined and two renamed (Peter: "I'm at least not
familiar with those terms"): **house**, **pop** (popping), **lock** (locking), **break** (breaking; toprock only, the
floor work is left out), **disco** (waack), **stomp** (krump), **hip-hop** (middle hip-hop, labeled "party hip-hop",
and LA hip-hop) and **jazz** (street and ballet jazz). `Style` in engine/DanceMoves: House, Pop, Lock, Break, Disco,
Stomp, HipHop, Jazz (+ Ballet, below).

- **Moves**: each AIST genre is exactly one style (DanceClips `clipGenreStyle`), so a dancer dances only its own
  genre(s); the old genre -> styles table is gone. Basic / 16-bar / 8-bar moves per style: house 60 / 13 / 4, pop
  40 / 20 / 0, lock 40 / 16 / 4, break 20 / 1 / 6, disco 35 / 14 / 5, stomp 37 / 9 / 8, hip-hop 79 / 38 / 3, jazz
  65 / 12 / 14 (tested: every style has at least 20 basic moves and 5 routines).
- **Looks**: `generateDancer` keeps the wardrobes written for the old styles and dresses each new one from the closest:
  house from house (30 % rave gear: neon, glow sticks, dyed hair), pop from robot (grays and one bright accent,
  visors), lock from retro (70s: stripes, pompadours, moustaches, aviators), break and hip-hop from hip-hop (hoodies,
  caps), stomp from hip-hop in a hard, dark palette, disco from disco (sequins, suits, afros), jazz from pop (60 %) or
  chill (40 %).
- **No named characters**: the eight (Coco, Dex, Moss, Nia, Ozzie, Pip, Theo, Vera) and the old Random entries are
  gone; content has one `Random <Style>` per style. The panel's "New dancers in these styles" chips are the 8 styles.
- **Old names still read** (`parseStyle`): robot -> pop, rave -> house, retro -> lock, chill -> jazz, plus the dances'
  own names (popping, locking, breaking / b-boy, waack / waacking, krump, shuffle, street / ballet jazz, middle / LA /
  party hip-hop), so saved settings, old scene files and stream guests' Style= keep working.
- **Hand-made moves** (unused at Mocap 1): tagged with their old styles, each counting for the closest new ones
  (hip-hop -> hip-hop, break, stomp; robot -> pop, lock; retro -> lock, disco; rave -> house; chill -> jazz); the feel
  per style reuses the old styles' values.
- **Move browser**: Style is the 8 styles; within one, basic moves first, then routines; hip-hop and jazz keep the
  genre in the label ("party hip-hop basic 3 · dancer 23").
- **The /dance page's preview dances them too** (Peter: "can people see some actual example moves for their dance
  style?"): `tools/aist_import/web_moves.py` writes a web sample, per style 3 basic moves (120-130 BPM first, the
  most movement, different choreographies, both genres for hip-hop and jazz) and 1 routine (a whole 16 bars where
  there is one): 32 moves, 645 KB, `moves.bin` next to dancer.js (tools/dancer_web/build.sh makes it). dancer_gl.js
  fetches it after the first frame and calls `dancer_moves` (DanceClips `loadClipsFromMemory`), then makes the
  dancer again; dancer_wasm cycles through the style's moves in order, each from its bar 1 on a bar line (basics 4
  bars, then the routine), no button (Peter: "just cycle through what's there"). Until the file arrives, or without
  it, the hand-made moves.

## Dancers reflected in the floor (Peter, 2026-11-20)

"Can the dancers' reflections show up in the disco floor in a subtle but nice way?" `reforce.Dancer.Reflection`
0..1 (default 0.35; Reflections slider on the Dancers card).

- The bodies are drawn a second time mirrored under the floor plane (`body_reflect_vs`: world y and the normal's y
  negated), shaded like them (`shadeBody`, winding flipped), 15 % darker and fading with depth below the floor
  (exp(-2.4 depth)), **opaque with depth into their own half-size target** before the stage pass, so each pixel holds
  only the nearest surface. `floor_fs` then blends that image into the floor once (premultiplied, under the grout
  lines and shadows), where the floor is. A first version blended every body part into the floor on its own, which
  doubled the reflection wherever parts overlap (knees, elbows, hair over the head).
- Half size keeps it cheap and slightly soft, like a glossy floor. It never raises the Syphon alpha's opaque share.
- Test: tests/test_render_dancers.cpp (under the feet changes with the strength, the body and the floor elsewhere
  do not).


## Break floor work (Peter, 2026-11-27)

"Extend the dancers' moves to the break floor work the AIST++ import left out": footwork and drops, freezes, power
moves. A prototype: content/Moves/aist_floor.moves, loaded after aist.moves, off until weighted in.

- **What was dropped** (raw keypoints; per frame "floor" = hips more than 0.45 leg down, the spine within 60 degrees
  of level, or head below hips): break basics 41 % of frames (60 of 120 clips with some: choreographies 7-10 are
  floor footwork from start to end, 5 a toprock with a drop), break routines 30 % (20 of 21 clips; 6 % of frames
  upside down). Other genres hardly: street jazz routines 22 % (knee work), ballet jazz / house routines 6-8 %, the
  rest under 3 %; not imported (Peter asked for break).
- **Tracking on the floor**: bones' length off by 20 % or more in 0.4 % of standing break frames, 5.8 % of floor
  basic frames, 43 % of floor routine frames; joint jumps (15 cm in a frame) 0.2 / 1000 standing, 14 / 1000 floor
  basics, 230 / 1000 floor routines; left and right swapped 9 / 1000 there (never standing). Floor basics (footwork)
  track about as well as standing (median bone error 0.03-0.06). Upside down, the hips' width wobbles by half and
  the face (nose, ears) flips. The two routines with real power moves (7, dancer 4; 14, dancer 5: by the look of
  them windmills / back spins) are garbage (bone error median 0.3-0.9, limbs flying): unusable. What survives as
  "power" is short inversions: kick-ups into a handstand, hand hops, rolls over the shoulder.
- **Freezes**: none in AIST++ worth the name. The longest hold off the feet anywhere in break is 2 beats (routine 5,
  lying); everything else held still on the floor is a routine's final pose. The tier exists (engine and importer)
  with 0 moves.
- **Importer** (tools/aist_import/build_floor.py; retarget / build_moves get a `floor` path, the standing path
  untouched: aist.moves rebuilds byte-identical): joints smoothed over 1.5 frames, the hips' and shoulders'
  left-right lines over 4; the pelvis follows the spine (not 65 % "up"); its whole turn in the body frame is a
  quaternion (`hipQx..w`; the Euler hip angles stay 0: they lock lying on the side, and were clamped to +-1 rad);
  the body's heading from the pelvis's facing plus the spine's lie, smoothed as a vector (lying, the facing is
  vertical; upside down, the spine); head and toe angles smoothed (a 16th of a beat) and not counted as glitches;
  `clear`: how far the upper body's lowest point (hands, elbows, knees, shoulders, pelvis, the top of the head) is
  above the floor, / leg. Kept where the source bones hold (median error < 0.15, < 10 % of frames off by 30 %),
  glitches bridged up to 16 % of a move (standing: 4 %), loops close over 4 beats (standing 2; at most an 8th of a
  move). Moves: the floor basics' 120-130 BPM performances; every break routine build_moves left out, whole, else its
  better clean half, else clean 4-bar stretches with floor work (no overlap, the most daring first). Ids
  `aistfloor:` (a routine may be in both files: its clean standing half in aist.moves, the whole here).
- **Tiers** (the move kind's bits 4-5, `ClipInfo::tier`; per beat, on the floor = hips low, the pelvis past 60
  degrees or the upper body touching): flips (3; first called power) = upside down (pelvis past 120 degrees) or
  rolling face down to face up, 2 beats or more; else floor (1; first called footwork). A move's tier is the most
  daring thing in it. (Freezes, 2, were looked for and none found; dropped 2026-11-27, below.)
- **Result** (first build): 33 moves, 932 KB: 29 footwork (the 16 floor basics, choreographies 7-10, dancers 5 and 6,
  120 and 130 BPM; 10 whole routines, 1 half, 2 four-bar stretches), 4 power (routines 5, 20, 21; 13 bars 7-10).
  Most floor routines were mostly toprock with a drop in them; pruned 2026-11-27 (below).
- **Engine**: `DancePose::hipQx..w` (applied before hipYaw / roll / pitch; exactly identity for standing moves,
  so the rig skips it), `ground` (floor moves 1) and `clear` (`kPoseFloats` 47 -> 53). `mix` takes the quaternion
  the short way round, `mirrored` negates its y and z, clipPose lines up the four Catmull-Rom samples' signs, and
  DancerRun keeps the raw pose's quaternion on the side of its last output so inertial blending never crosses a
  sign flip. **groundPose** (DancerRig, after the energy mix in DancerRun::step, before the hair): the upper body
  (head ball, pelvis, chest, shoulders, elbows, hand balls, knees, by the rig's own IK) moved up or down so its
  lowest point is at `clear` where the move has it near the floor (fully below 0.05 leg, none above 0.25 leg: a
  toprock in a floor routine keeps its own hips), and never below the floor; feet in the air move with it, planted
  feet stay. It moves `hip.y` only (twice, the knees follow). 0.18 us a pose. Our big heads and short arms are why
  it is needed: a headstand's head is 2-3 times a person's, so the body stands higher on it.
- **What assumed an upright dancer** (first build; hair, name tags and shadows fixed 2026-11-27, below), and what happens now: the 0.45 hip and 1.2 rad tilt filter (floor file only:
  replaced by tracking checks); the clamped Euler hip angles and the "smaller solution" (quaternion); the pelvis
  leaning only partly with the spine; the heading from the pelvis's facing; only the feet touching the floor
  (groundPose); arms swung away from the head (faded out for `ground`: on the floor hands are planted by the head
  and the swing jumped by up to 0.35 a 16th of a beat); walking steps replacing the feet (not while `ground` >= 0.5:
  the spot glides instead); humanize's deeper knees (groundPose corrects them); watching someone (never a floor
  move). Left as they are (notes): **hair** hangs from the head's frame plus gravity, so upside down the tips sit
  above the head and big hair can brush through the floor (groundPose holds the head ball, not the hair; hats too);
  the hair code is being changed elsewhere, so not touched here. **Name tags** follow the head top low-passed over
  0.6 s: they sink to the floor in a drop (guests only; a floor guest's tag would want a floor of its own, e.g. at
  least half standing height). **Shadows**: the body's blob shrinks with the head's height (headCenter.y / 0.82), so
  a body lying down casts almost none. **Crowd spacing** knows only spots (`kMinApart` 0.38, `kBodyRadius` 0.21):
  a dancer lying is about a body long, so neighbors can overlap the legs. Travel is held to 0.75 leg lengths as
  for every move. Heading unwrapping (spins add up) and the floor reflection needed nothing.
- **Settings** (first build; settings.json only, 0..4, default 0 = off): `reforce.Dancer.Footwork`, `Freezes`,
  `Power`, each tier's weight against a standing break move. Since 2026-11-27 (below): `reforce.Dancer.FloorMoves`
  alone, default 0.5. Trying a move (the Dancers card's Mocap moves browser, or `beatset dancetry LABEL|ID`) plays
  any floor move regardless.
- **Web preview**: the same engine code (it compiles and grounds); its moves sample (web_moves.py, from aist.moves)
  has no floor moves, as the app's default has none.
- **Measured** (tests/test_engine_danceclips.cpp, "floor work"): standing dancers render byte-identical to before
  (12 offline frames of five generated dancers incl. two break, against a build of the previous commit); every floor
  move finite for three body shapes; never more than 3 cm through the floor (hair and shoes aside); where the source
  dancer's upper body touches the floor, ours does in 13,186 of 13,188 samples; a hand, foot or the head moving 0.2
  (a fifth of a body) in a 16th of a beat in 5 of 81 standing break moves, 11 of 87 footwork, 10 of 12 power (kick-
  ups and flips are fast, and power loops close by blending a 4-beat flip back to standing); fastest 0.33 / 0.38 /
  0.32. By eye (offline frames): footwork reads as footwork (low crouch, a hand down, legs sweeping; the face-down
  basics 10 come at the camera head first, as the source dancer faced); inversions land on the head and hands with
  the legs up; the big cartoon head fills the frame up close.
- **Other sources** (not downloaded; for power moves and freezes, which AIST++ can't give; licenses as I remember
  them, to be checked before any use): AMASS (SMPL fits of many mocap sets; non-commercial research license),
  Motorica Dance (CC BY-NC 4.0, I believe), BRACE (Red Bull BC One battles, break-specific, keypoints from video, so
  the same tracking problem upside down; license to check), FineDance and PopDanceSet (research only), Mixamo
  (keyframed mocap with windmills, headspins, flares and freezes; free to use in a project, not to redistribute the
  files, so the GPL repo couldn't ship them). None is as open as AIST++ (CC BY 4.0).
- Not done: freezes (no source); power moves worth the name (no source); floor work for other styles.

### Peter's feedback on the floor moves (2026-11-27)

Peter tried them: "footwork" vs "power" confused him, many looked like nothing new, the hands don't quite touch the
floor, the head-first floor basic 10 was hard to find; big hair, long hair and hats went through the floor. He
approved four fixes:

- **Sorted by how much is on the floor** (build_floor.py `MIN_FLOOR`, so a rebuild reproduces it): a move on the
  floor (`clear` under a quarter leg, or the pelvis upside down) less than 25 % of the time is mostly toprock with a
  drop or some floor work in it: first left out, then (Peter, same day: "more break variety") kept as a **standing
  break move** (tier 0, a plain label "break routine 3 · dancer 4", picked like any break move, not gated by
  FloorMoves): routines 3 (17 %), 6 (4 %), 9 (6 %), 10 (18 %), 19 bars 1-8 (14 %), and the 4-bar stretches 8 bars
  13-16 (17 %) and 13 bars 3-6 (0 %). None goes upside down for 2 beats (6 and 9 for a moment: 2 and 4 % of the
  samples). The rest are **floor moves** (22): the 16 floor basics (96-100 % on the floor) and routines 2, 4, 11,
  15, 17, 18 (29-55 %). aist.moves rebuilds byte-identical; aist_floor.moves 29 moves, 845 KB.
- **Renamed** floor (low work on the hands, knees and hips) and flips (goes upside down): labels "break floor basic
  10 · dancer 5 · take 1" (`reforce.Dancer.Floor` is the disco floor, hence FloorMoves). The freeze tier is gone (a
  file's tier 2 reads as floor). The Dancers card's Mocap moves list has the floor moves as their own entry after
  break, **break floor (22)** (`library.danceMoves[].tier`); the standing ones with floor work are under **break**;
  Try and `beatset dancetry` by id or label as before.
- **Flips eliminated** (Peter, same day): build_floor.py finds them (routine 5, 13 bars 7-10, 20, 21) and leaves them
  out; no `Flips` setting or list entry (the engine keeps the tier: a file's flips would never be picked). For coming
  back to flips / power moves, what was learned: the inversions in AIST++ are real (on the raw keypoints the
  shoulders 23-42 cm below the hips, the hips 49-87 cm up, in 92-100 % of the inverted samples, held 2-3 beats:
  kick-ups into a handstand), but upside down the tracking wobbles (the hips' width by half, the face flips; bone
  error 0.06-0.22 there against 0.03-0.06 standing), and the two routines with real power moves (7, 14) are garbage;
  our big head and short arms: upside down the head reaches the floor before the hands, so 23 % of the samples
  with the source's hand down still float (77 % within 1 cm after planting, 90 % within 6.8 cm), and a top hat
  stands the dancer on the hat (it would want to fall off or tilt); candidate sources (licenses to check): Mixamo
  (keyframed windmills, headspins, flares, freezes; usable in a project, files not redistributable, so not in the GPL
  repo), BRACE (Red Bull BC One battles, keypoints from video: the same tracking problem), AMASS, Motorica Dance,
  FineDance, PopDanceSet (research / non-commercial licenses). Ideas: stretch the arms a little (cartoon) while a
  hand is planted upside down, tuck the head, knock a hat off on a headstand.
- **Planted hands**: the importer writes each wrist's, knee's and the head's own clearance (`floorHandL/R`,
  `floorKneeL/R`, `floorHead`, / leg, 0..0.5; not scored in the loop closing, so no choice changed). loadClips makes
  a plant weight per hand from it (1 under 2 % of a leg, 0 over 7 %, widened and smoothed over an 8th of a beat so
  a slap eases down), `DancePose::plantL/R` (`kPoseFloats` 55; mixed, mirrored, blended like the rest). groundPose,
  after holding the body to `clear`: each planted hand's target moved down (chest frame, within the arm's reach) until
  the hand ball's lowest point (the ellipsoid buildBody draws) is on the floor, by `plant` of the way; where the arm
  is too short the body is lowered, at most 0.12 leg and never putting anything else through the floor. Measured
  over every floor move and three body shapes, where the source wrist was down: floor tier, hand ball bottom median
  0.04 cm, 90 % 0.9 cm, 91 % within 1 cm before; planted median 0, 90 % 0.02 cm, 99.3 % within 1 cm (worst 9.7 cm
  where the 0.12 leg cap binds); the standing break routines with floor work 89 / 93 % within 1 cm (flips, before
  they went: 55 / 77 %). Never more than 0.07 cm through the floor; no faster hand than before. Knees (not moved):
  where the source knee is down ours is at median 0.05 cm, 90 % 4.2 cm. The source head is on the floor in 36 of
  37,707 samples (median 0.8 cm off; all in the flips): the floor work is on the hands.
- **Hair and hats out of the floor**: `headExtent` (once per look, the first floor move): up to 64 points on the
  outside of every head part that moves with the head (hats, big hair, buns, spikes, shells, ears, glasses; not
  `BodyPart::hangs`: chain links, the long hair's tube, the curls riding it), in the head frame; groundPose counts
  them with the body (1 cm margin: big hair lags a little). Hanging hair: each chain kept as far above the floor as
  it is drawn thick (`HairChainDef::floor`; the curls' columns as far as the curls reach). What still dips under
  (the lag, hair lying on the floor) is cut off there when drawn (hair materials only, DancerPipeline). Measured
  (tests/test_engine_hair.cpp: every hairstyle, and cap / beanie / top hat / headphones on afro, huge curls, short,
  long and bun, on floor basic 10 and the three flips then): deepest through the floor before / after: hats and
  ears 25 / 0 cm (a top hat on a headstand), hair with the head 17.6 / 3.4 cm (anime spikes lagging; afro 17 / 1.2),
  hanging 17.8 / 4.4 cm (huge curls). (The test now uses floor basics 10 and 8, floor routine 11 and break routine 9:
  hats 0, hair with the head 1.9 cm.)
- **Name tags** keep at least 55 % of the highest the guest's head top has been (standing). **Shadows**: the body's
  blob as big as the body in floor work (it shrank with the head's height, made for the entrance pop), and one more
  under the head while it is near the floor (up to 4 a dancer, 136 in all).
- Standing dancers render byte-identical (offline frames of 5 and 6 generated dancers, against a build of main),
  except single pixels where HugeCurls' lowest curls already dipped through the floor in a deep crouch: now cut there.

### Floor moves by default, and bust a move (Peter, 2026-11-27)

- **Break dancers mix floor moves in by themselves** (generated dancers and guests whose style is break; nothing to
  set): `reforce.Dancer.FloorMoves` default 0.5 (each floor move's weight against a standing break move; 0 = never,
  as before up to 4), and the music has a say (Choreographer::pick): x1.6 in a breakdown (build 0.05-0.6), x0.15
  while a build climbs (over 0.6), x2.2 after a drop (drop over 0.2, also the drop's own hype pick), x1.4 on a phrase
  line; a floor routine (16 bars) never right after another. Measured (tests/test_engine_danceclips.cpp, six break
  dancers each held in a section): groove 28 % of the mocap picks are floor moves, breakdown 46 %, build 7 %, after
  drops 35 %; FloorMoves 1 instead: 41 % (92 of 222). Other styles never.
- **Bust a move** (first called break time / "Break it down"; a stream guest's "Bust a move", or `beatset dancebust
  ID|NAME|#N` and the Dancers card's per-dancer **Bust a move** button): any dancer, any style, from their next
  8-bar point plays two whole 16-bar break routines with floor work in them, different ones back to back (32 bars),
  then their own moves again
  (Choreographer::bustMove; the set: the floor routines 2, 4, 11, 15, 17, 18 and the standing ones with floor work
  3, 6, 9, 10; not the 4-bar floor basics). The routines play whole (no drop cuts in); routine changes cross-fade as
  usual, the stage's own blending carries the rest. Meanwhile the dancer isn't directed (no partner to mirror, no
  watching): Mingle (`MinglePerson::bustMove`, from the moment it is asked) takes them out of any group at once and
  has them show off where they are: nobody walks over to join them, they walk to nobody, neighbors may come over to
  watch (measured: watched 116 % of the time summed over watchers; moved at most 0.19 by the crowd). Asked again while
  on: ignored. The panel's dancer list marks it (`danceRuns[].bustMove`).
- Found on the way: a 16-bar routine starts only on an 8-bar point of the phrase, and the check was exact (1e-6
  beats), but each dancer's own beat (humanBeat: timing lean, drift, feel) drifts against the music's bar and phrase
  count, so live it hardly ever passed: with a 0.05-beat drift a house dancer started 1 routine in 248 mocap picks,
  against 33 in 153 without drift; basic moves filled in.

### Peter's calls (2026-11-27, later)

- **Routines start as easily as basics** ("I don't want anything keeping dancers from being able to start routines
  and moves just as easily regardless of the length"): a move due within a quarter beat of an 8-bar point starts
  exactly on it (Choreographer::pose's scheduling loop; the earlier move ends there), and `onEight` allows a quarter
  beat. A bust cuts a routine still running at its start (it used to be cut only when asked; a routine picked
  after the ask ran on through it).
- **Floor moves are ordinary break moves** ("into the normal break cycle without special distribution weighting"):
  FloorMoves default 1, no say of the music, floor routines may follow each other (the above's x1.6 / x0.15 / x2.2 /
  x1.4 and the no-two-in-a-row rule are gone). Measured (same test): groove 42 % of break's mocap picks are floor
  moves, breakdown 48 %, build 42 %, after drops 34 % (they are 22 of break's 56 moves).
- **"Bust a move" has no limit for now**: the maintainer's service takes every request from a guest on the floor
  while live (BREAK_REPEAT_MS 0); asked again while on, reforce ignores it as before.

### Floor moves held still, room to go down (Peter, 2026-11-27)

Measured first (tests/test_engine_floorslide.cpp, skipped "measure" cases; 124 BPM, FloorMoves 2, 600 s): floor
contacts (hands, elbows, knees, feet, head, hips within 1 cm of the floor) slid 22-24 cm a beat; the ones the move
itself holds still 8-10 (planted standing feet: 2-3). Causes: the spot gliding (Mingle kept sending floor dancers
places and stepFeet is off on the floor: 17-20 % of floor time over 0.15 units/s, any crowd size), keepApart
pushing them (14 dancers), the stage turning them, the runtime layers (lags, humanize, follow-through, the energy mix
towards the standing rest pose) and most of all the retargeted moves themselves (our proportions: the source's
hands are planted 58 % of their floor time, ours held 10 %). Neighbors overlapped a floor dancer's body 14 / 31 / 47 %
of the time with 5 / 9 / 14 dancers, up to 33 cm deep. Peter approved all of it:

- **Held still on the floor** (DancerRun::step): with `ground` (0.25 .. 0.6) the spot is held where they went down
  (any walk eases out in ~0.3 s), no steering, the stage's turn held; avoidEachOther / keepApart push only the other
  dancer. Mingle makes no plans for them meanwhile (due plans wait), nobody walks over to them, their spot stays put.
- **Runtime layers faded with `ground`**: the arm / head lags, humanize (gains, effort, hand wander, posture), the
  follow-through springs and the energy / effort mix towards the standing rest pose.
- **Planted contacts pinned**: build_floor.py writes `pinHandL/R`, `pinFootL/R`, `pinKneeL/R`, `pinHead` (0..1: the
  source's contact within 3 % of a leg of the floor and moving under 0.12 legs a beat to plant, 6 % / 0.3 to stay,
  at least an 8th of a beat, eased over a 16th inside the plant; `DancePose` pin*, `kPoseFloats` 62, mixed and
  mirrored). aist.moves rebuilds byte-identical, aist_floor.moves the same moves (954 KB). DancerRun::pinContacts (after
  groundPose): a contact takes hold where it is when its weight rises (no jump) and is held there on the stage: feet
  by their body-frame targets, hands by their chest-frame targets (`chestFrame`, within the arm's reach; groundPose's
  vertical planting as before), knees and the head (and a hand out of reach) by shifting the whole body (at most 0.15,
  0.6 units/s; eases back when nothing holds it); it lets go with the source's weight and gives up, eased out over
  0.15 s, past 8 cm (hands), 12 cm (feet), 20 cm (knees, head).
- **Room to go down** (Choreographer::setFloorRoom, Mingle): every floor-file move knows its reach (ClipInfo::reach:
  the body's support in 8 directions over the loop, grounded, standard body; 0.37-0.86; `headFirst` for the face-down
  basics 10). The choreographer picks the move it wants as before; if it doesn't fit round the dancer (or the dancer
  is walking: "not now") the dancer dances standing basics and the same move (mirroring too) starts on its next 4-bar
  (basic) or 8-bar (routine) point of the phrase (within a quarter beat, as routines start) once it fits. Meanwhile
  Mingle anchors the dancer (on their own spot, showing off, in a pair or group, busting a move; a walk stops; never
  inside someone else's room), turns them so the reach points into open space (towards the audience when close;
  head-first moves at most 0.25 rad, others 0.6) and clears the reach plus 0.4 round them: neighbors' spots pushed out
  (sideways where the front or back of the floor stops them), walking there with real steps; pickSpot keeps out, and
  avoidEachOther / keepApart already keep the others out of the reach. The room eases away over ~2 s as they get up:
  people drift back.
- **Never give up** (Peter, later the same day): the chosen move keeps waiting, on its grid, while the room is cleared
  ever more firmly: the air round the reach grows by up to 0.35 (0.04 a second waited), the turn is looked at again
  every 2 s, the last 12 cm are left to keepApart (it pushes the others out at about 1.4 units/s), after 4 s the
  dancer steps away (at most 0.3 units/s) from whoever is boxed in by the edge of the floor, someone else going down
  in the way makes them step out of it first (of two waiting, the later one), and in a pair or group one goes down at
  a time (whoever has waited longest; after 6 s waiting for the other one's turn they leave the group to go down on
  their own). Only after 6 grid points does a move start anyway (`kFloorLastResort`; never seen in the runs below);
  a bust waits the same way on its 8-bar points.
- **Pairs and groups**: the one going down stays; the others make room in an arc open to the audience at the reach +
  0.45, facing them, dancing as watchers do (watchPose); the pair or group carries on afterwards (Mingle's
  `Group::floorId`). The same for a bust. **Partners**: a follower doesn't mirror a floor move, they make room and
  watch (`kFollowerMirrorsFloor` in Dancers.hpp: true copies it, as before).
- **Measured** (600 s, before / after; all fixes): floor contacts 23.7 / 15.2 cm a beat (5 dancers), 22.5 / 14.0 (9),
  21.9 / 14.8 (14); held by the move 9.6 / 1.6, 9.4 / 1.4, 9.0 / 1.6; planted in the source 16.9 / 3.9, 16.1 / 3.4,
  15.3 / 3.5; spot over 0.15 units/s 17-20 % / 0 %; overlap 13.9 / 0.0 %, 31.7 / 0.8 %, 47.4 / 1.0 % (max 30-33 / 8-19
  cm). How long a floor move waits (6 seeds x 600 s, in grid points: 4 bars for a basic, 8 for a routine): 5 dancers
  mean 0.30, 90 % within 1, at most 2 (340 floor moves); 9 dancers 0.60, 1, at most 5 (591); 14 dancers 0.79, 1 (one
  seed 2), at most 6 (828); never the last resort; waiting 5 / 10 / 12 % of break dancers' time. Pops: every floor
  move tried in turn, the fastest hand, foot or head a frame p99.9 12.2 / 11.8 cm, max 46.5 / 43.3. One by one (9
  dancers, first version): fade alone 21.3, pins alone 20.1 (held 6.1, planted 6.4), room alone 19.4 (overlap 1.0 %),
  hold + room 18.6. Test "floor slide: floor moves held still, given room, no new pops" checks these on 150 s (and
  that every floor move starts within 2 grid points); standing dancers render byte-identical to before (9 generated
  dancers, 40 s, 4 offline frames).
- **Renamed**: break time is **Bust a move** throughout (Peter, 2026-11-27): the panel's button, the guest request
  (`kind` `bust`), `beatset dancebust`, Choreographer::bustMove / bustMoveAt, MinglePerson::bustMove,
  `danceRuns[].bustMove`.

### Bust a move on the guest page: what's really happening (Peter, 2026-11-27)

- The guest page's "Bust a move" used to say "Coming up at the next 8 bars! 🔥" for good after a tap. reforce now
  tells the service where each guest's bust stands with every poll (`&bust=ID:STATE:S`, docs/guests.md):
  Choreographer::bustPhase (DancerRun::bustPhase, on the dancer's own beat): `wait` (asked, or planned for its 8-bar
  point), `room` (its point came without room to go down: put off, the crowd making room; from the first put-off
  on), `on` (the 32 bars), with the seconds until it starts / ends at the current tempo. The page shows "Coming up at
  the next 8 bars! 🔥 In 12 s.", "Making room on the floor… 🔥 Next try in 20 s.", "Busting a move now! 🔥 68 s to
  go.", then "That was 🔥" for a few seconds and the button again (disabled while one is coming up or on: asked again
  then, reforce ignores it, and the service returns the one under way instead of asking again). Not taken within
  15 s (the dancer left), or the stream not live: it says so.
- Measured on the local stack (14 generated dancers plus guests; six guests asking together, twice): from the tap,
  the page shows it coming up within 1-2 s and busting within 2-3 s of its 8-bar point; no bust waited for room in
  these runs (the `room` state is covered by test_engine_danceclips "floor moves wait for room" and by hand-made
  polls against the service).

## Mingle only, one personality (Peter, 2026-11-27)

- **Mingle is the only social mode** (code cleanup): the app has planned every step with engine::Mingle since
  2026-11-21; now the leftovers are gone: `SocialMode` (show, partners, call, face to face, cypher, mingle),
  pickSocialMode, planStage and its formations, scatterSpots (only planStage used it; Mingle picks its own spots,
  Mingle::pickSpot), DanceDirection::delay (canon, crew sync), the app's planStage branch and `socialMode_`, the
  "dancers: mingling" log line, the panel's mode after the On stage count (`danceMode` in the API) and their tests.
  Kept, all used by Mingle: partners (leader / mirror), call & response, watching (`react`, watchPose), horseshoes,
  pickSpot, steering (avoidEachOther / keepApart), Bust a move, room for floor moves. Behavior unchanged: an offline
  render of a mingling crew (`--deterministic --dancer-test house,pop,break,disco,lock,stomp`, 3600 frames, 72 saved)
  is byte-identical before and after.
- **One personality for everyone** (Peter: "I don't want guest dancers to have hidden different personalities. So I
  want just a single generally decent personality for all guest dancers (and generated dancers as well)"): Mingle no
  longer seeds sociable / restless / showy per dancer (each was uniform 0..1); every dancer has `Mingle::kPersonality`
  = sociable 0.45, restless 0.5, showy 0.6. The plain mean (0.5 each) left people a little less alone (45 / 44 %
  against 48 / 45 %) and showing off less (4.4 / 3.8 % against 5.3 / 4.5 %; the showy ones used to show off more
  than their share), so sociable went down and showy up until the shares matched. Guests' choices still apply (vibe
  Solo / Mingle / Crew, `with`); the variety left is each dancer's own clock and their seeded choices (what next,
  how long, where to wander, who to go over to) and the humanising's own personality (timing, posture: DancerRun).
  Measured (tests/test_engine_dancers.cpp, skipped "measure the shares" case: 24 runs x 10 minutes at 124 BPM,
  simple walkers), before / after:

  | | 6 dancers | 12 dancers |
  |---|---|---|
  | alone | 47.8 / 47.0 % | 45.0 / 45.3 % |
  | pairs | 40.7 / 41.1 % | 41.1 / 41.2 % |
  | threes | 9.4 / 9.7 % | 11.3 / 10.9 % |
  | fours | 2.2 / 2.2 % | 2.7 / 2.5 % |
  | changes of activity a dancer-minute | 10.90 / 10.78 | 10.69 / 10.75 |
  | dancing on their own spot | 31.9 / 30.9 % | 28.8 / 28.9 % |
  | showing off | 5.3 / 5.7 % | 4.5 / 4.9 % |
  | wandering | 5.3 / 5.3 % | 6.7 / 6.5 % |
  | taking a break | 2.4 / 2.2 % | 1.9 / 1.7 % |
  | going over to someone | 1.9 / 1.9 % | 1.8 / 1.9 % |
  | watching | 1.0 / 1.0 % | 1.3 / 1.4 % |

  The test that the most sociable third spends longer in groups is gone; a new one checks that dancers with
  different ids on differently seeded floors (a Crew guest among them) all have the same personality.
- Found on the way: the "no jitter" crowd test checked one 3-minute run per case, and reversing steps are
  heavy-tailed: most runs have a few, but now and then two walkers stall against each other for a few seconds (seen:
  head-on at the back of the floor, where keeping right pushes one into the back edge) and one run gets 50-130.
  Over 64 seeds (8 mingling, 3 minutes each): 0.40 per thousand moving steps before, 4 runs over the 1 per thousand
  limit; after 0.43 (0.5 each) / 0.51 (as chosen), 5 / 8 runs over. The new trajectories put one of those stalls in
  the test's own run, so each case now pools four seeds (same limits). Not fixed: the stall itself (avoidEachOther).

## Spin glitches fixed (Peter, 2026-11-27)

Dancers sometimes whipped round super-rapidly. Measured with tests/test_engine_spin.cpp ("spin: measure*", skipped:
the body heading as drawn split by cause, mingle crowds of 5 / 9 / 14 at 124 BPM, 10 minutes, a bust every 90 s) and
tools/aist_import/heading_speed.py (per move). Fixed, each where it came from:

- **Energy mix**: `mix(restPose, pose, e)` (effort under 1: wandering, resting, unlocked beat) scaled the heading,
  stage facing included, so it snapped by 2 pi (1 - e) whenever it passed +-pi. The heading stays out of the mix.
- **Routine churn**: Choreographer::advance snaps a routine's start to an 8-bar point within a quarter beat, on the
  dancer's own (drifting) grid; Bust a move's start and end, planned on an earlier frame's grid, sat a few hundredths
  of a beat off, and the routine between was cut to nothing, picked again up to 64 times a frame, every frame, for up
  to 24 s. Now a bust's start / end there moves with the snap (bustPhase reads the moved one), the snap never leaves
  the last routine under a beat, and no routine is under a beat (the last one plays on instead; always progress).
- **Inertial blending**: the output's heading speed (taken over at a change of source) now the short way; a source's
  heading jumping by whole turns (a loop's seam, a crossfade's end) whipped them round 2-7 times.
- **Crossfades**: the heading's difference between the two routines followed continuously from the fade's start
  (Routine::fadeYaw), not chosen the short way every frame (a flip of 2 pi x w when it passed half a turn).
  Overlapping fades stack (a drop cutting in while the fade into the routine playing still runs dropped that fade:
  a jump); a drop keeps a routine already fading in (cut to nothing; its fade carries on under the drop's).
- **Watchers** (watchPose): a change of watch move (every 4 bars at most; reactions every 2) crossfades over 2 beats
  like the routines, the heading's difference followed in eighth beats from the fade's start (either may spin).
- **Floor importer** (retarget.py, floor file only): the heading smoothed as an angle (unwrapped) weighted by the
  facing-plus-spine vector's length, not as the vector (it shrank in fast turns and its angle whipped). aist.moves
  is byte-identical. Per move, peak / p99 turns a second at 124 BPM, before -> after: break routine 10 · dancer 5
  3.77 -> 1.10 / 1.62 -> 0.99; routine 19 · dancer 6 2.72 -> 0.99; floor routine 17 · dancer 6 2.70 -> 1.46; routine
  6 · dancer 4 2.07 -> 1.11; floor routine 11 1.95 -> 1.04; none of the floor file over 1.5 (7 moves before).
  **Break routine 9 · dancer 5 is gone**: its whip was taken for a tracking glitch and bridged straight over, which
  also flattened its moment upside down; without it, it is upside down long enough to count as flips (left out,
  as all flips). 22 floor moves, 6 standing break routines with floor work (7). Floor tests all pass; their numbers
  moved a little with the move gone (floor slide: worst pop 29 -> 19 cm; hair through the floor 2 cm -> 0).
- **Partners copy only within a style**: a mingle pair dancing partners (one mirroring the other's routine) does so
  only when both dance the same style (MinglePerson::style); otherwise face to face, each their own moves. Call &
  response and watching unchanged (a watcher dances their own style).
- **Facing the audience** (Peter: busting dancers spent a lot of time facing the back): each clip gets a fixed turn
  at load (DanceClips faceAudience; the files unchanged): the one with the most samples within 90 degrees of the
  camera, ties by the mean heading nearest it; none for under 2 points gained. Heading and travel turn together (the
  dance is unchanged, mirroring mirrors it), before the floor reach is measured. Turned noticeably (over 20 degrees, 5
  points less facing away): 140 of 543 standing clips, 12 of 28 floor-file clips. The bust routines facing away
  before -> after: floor routine 2 89 -> 9 %, 11 93 -> 2 %, 15 87 -> 6 %, 17 74 -> 16 %, 18 82 -> 14 %, break routine
  10 79 -> 8 %, 6 31 -> 10 % (floor routine 4 and break routine 3 faced the camera already). In the crowds: a
  busting dancer faces away 60 -> 10 % of the time, everyone 30 -> 10 %.
- Kept as recorded: the moves' own spins (ballet jazz pirouettes and the like): no caps.

Visible spin events (a run over 1.5 turns a second turning at least 1 rad, or a one-frame step over 0.5 rad) per
dancer-minute, before -> after, by what led them (move / crossfade / cut / inertial / stage facing / energy mix):

| crowd | all | move | crossfade | cut | inertial | facing | energy | churn s | snaps |
|---|---|---|---|---|---|---|---|---|---|
| 5 | 3.12 -> 0.66 | 1.16 -> 0.44 | 0.26 -> 0.16 | 0.78 -> 0 | 0.20 -> 0.04 | 0.10 -> 0.02 | 0.62 -> 0 | 0.5 -> 0.1 | 85 -> 0 |
| 9 | 3.66 -> 1.19 | 0.98 -> 1.01 | 0.34 -> 0.11 | 0.47 -> 0 | 0.32 -> 0.06 | 0.02 -> 0.01 | 1.53 -> 0 | 0.8 -> 0.1 | 220 -> 0 |
| 14 | 3.39 -> 0.64 | 0.51 -> 0.48 | 0.23 -> 0.11 | 0.41 -> 0 | 0.25 -> 0.04 | 0.04 -> 0.01 | 1.95 -> 0 | 13.8 -> 0.3 | 381 -> 0 |
| 9, seed 7 | 3.76 -> 0.87 | 0.90 -> 0.62 | 0.34 -> 0.19 | 0.59 -> 0 | 0.40 -> 0.04 | 0.01 -> 0.02 | 1.52 -> 0 | 24.3 -> 0.2 | 237 -> 0 |
| 9, unlocked | 9.17 -> 1.20 | 0.43 -> 0.99 | 0.24 -> 0.16 | 0.31 -> 0 | 0.06 -> 0.03 | 0.01 -> 0.02 | 8.12 -> 0 | 0.8 -> 0.1 | 799 -> 0 |

What is left: the moves' own spins and gentle blends (no one-frame step over 0.41 rad; the fastest not led by the
move about 3.3 turns a second, a crossfade or a change of source into a turning move). The churn left is a drop
cutting in right after a routine started (two changes in a row, blended). The checks: "spin: no routine churn, no
heading snaps, nothing fast but the moves' own spins; busts face the camera". Standing crowds look a little
different: the heading fixes, and above all the clips turned to the audience.
