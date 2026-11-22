# Dancers: 3D cartoon characters dancing to the beat (PLAN 5P)

Peter's decisions (2026-11-18):
- **Full colour on top** of the visualizer (composited after colorize, like the colour movie sprites), so skin,
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

One pass per sim step into one frame-size premultiplied BGRA8 target (4x MSAA memoryless colour + depth), then
alpha-over onto the colorized frame inside `Visualizer::colorize`, so the window, Syphon and the split-video window
all get it. Nothing is encoded while no dancer is live (goldens unchanged).

- **Geometry**: every body part is an instance of one "capsule" mesh: a unit sphere split at the equator, the top
  half moved to one end and the bottom half to the other, with tapered radii r0, r1. That one mesh gives spheres
  (length 0), ellipsoids (non-uniform scale), limbs (capsules) and cones (r1 ~ 0, hair spikes). Instances carry a
  model matrix, length, radii, colour, a material id and a clip plane (hair shells cut open at the face).
  Hair: close styles (short, spiky, ponytail, bun) are shells just larger than the head, cut open at the face (a
  smooth per-pixel hairline; the inside seen through the cut is shaded as solid hair). Fuller styles (bob, long,
  pigtails, afro) are solid masses set up and back so the face pokes out in front: a shell standing off the head
  shows its hollow inside as a hood or bowl around the face (2026-11-19). A mass must cross the head steeply or the
  mesh steps along the hairline.
- **Look**: soft toon shading (wrapped Lambert through a soft two-band ramp), a sky/ground hemisphere ambient, a
  plastic specular highlight, a rim light tinted by the current palette (so they sit in the scene's colours), and a
  thin dark outline (inverted hull, width in pixels). Settings `reforce.Dancer.Outline` (px, 0 = none).
- **The visuals are the lighting** (Peter, 2026-11-18): the dancers are drawn after colorize, each display frame, and
  light themselves from a small blurred, mipmapped copy of the finished frame. A surface takes light from the visuals
  in the direction its normal faces (a red flash on the left lights their left side red), a rim light comes from the
  frame behind them, and the floor reflects the visuals and its tiles take their colours.
  `reforce.Dancer.Lighting` (0..1, default .75) mixes a studio key light (0) with lighting from the visuals only (1),
  so they stay readable when the screen is dark.
- **Face**: drawn in the head's fragment shader from the head-local direction: eyes (with highlights; blink),
  brows, mouth (smile; opens on the beat / on drops), blush. Later: `Face="faces/name.png"` maps a portrait image on
  the face area instead (friends' faces) — the key is reserved now.
- **Floor** (`reforce.Dancer.Floor` on/off, default on; no panel button (Peter, 2026-11-20: always wanted)): a perspective grid of tiles in the stage plane that light
  up in palette colours on the beat (patterns change each bar: checker, chase, ripple from the centre, random),
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
  D1 database). `web/GuestFeed` polls its `/api/checkins` every 5 s on its own thread (HTTPS through NSURLSession); each new
  check-in becomes a dancer from the guest's keys plus `Name`, staying `reforce.Guests.Minutes` (30); checking in
  again while dancing does nothing. When the stage is full, characters make way before guests. Polling also tells the
  page reforce is live (LIVE while it polled in the last minute). The first poll takes every check-in of the last 3
  hours that were never handed over; `guests-seen.txt` next to settings.json remembers the last check-in handed over,
  so a restart never brings anyone twice. Off by default with no built-in service (2026-11-22, for open source:
  `reforce.Guests.Enabled`, `reforce.Guests.Url`, and `reforce.Guests.Key`, a read key sent as a bearer token; the
  Dancers card shows the status, no button). Never polls offline (`--deterministic`)
  or in a `--background` test instance.
- **Guest messages** (2026-11-20): a guest types a message on the page ("Say something", up to 140 characters, no
  filter, no rate limit: Peter's choice); the service keeps it (`/api/say`, also a check-in) and hands it out with the
  check-ins for 2 minutes. reforce shows it in a speech bubble above the guest's name tag for 30 s (a new one replaces
  it): white bold text wrapped to at most 5 lines, emoji in colour, on a dark rounded box with a light outline and a
  tail (render/TextOverlay `bubble`: drawn by Core Text into a colour image). It pops in and fades over the last
  second. `beatset dancesay NAME=TEXT` gives any dancer a bubble (tests, or by hand).
- **Hide dancers** (2026-11-20): the panel's Hide dancers (`beatset dancehide on|off`, session only) fades the whole
  stage and the name tags out over half a second; everything keeps running behind it (hold times count down, guests
  still arrive), so showing it again brings back whoever is still due.
- **More looks for stream guests** (2026-11-20, Peter: "match people's appearance"): hairstyles pixie, bangs, wavy,
  braid, side pony, space buns, top knot and locs; outfits skirt, crop top, jumpsuit and long dress (random
  characters use them too); clear lenses (`Lens="clear"`: only the frame is drawn, in its `Frame` colour). No gender
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
  move, every other dancer mirrored).

## Scene keys (`reforce.Dancer.package`)

Every key is a literal (no expressions; nothing in a character needs per-frame maths). Full list: scene-format §5.9.

| Key | Default | Meaning |
|---|---|---|
| `Skin`, `Hair`, `Top`, `Bottom`, `Shoes`, `Eyes` | various | Colours, `"#rrggbb"`. |
| `HairStyle` | `short` | `short` (`crew`, `crop`), `buzz`, `shaved`, `sidepart`, `quiff`, `pompadour`, `slickback`, `undercut`, `coils` (`twists`, `hightop`), `waves` (360 waves), `curtains` (a middle part), `balding`, `manbun`, `mullet`, `bowl`, `spiky`, `mohawk`, `anime` (`messy`), `curly` (`longcurls`), `afro`, `bob` (`lob`), `long` (`wavy`, `locs`), `layers`, `bangs` (long with a fringe), `shag`, `halfup`, `pixie`, `ponytail` (`lowbun`), `sidepony`, `braid` (a side braid), `pigtails`, `bun`, `topknot`, `spacebuns`, `bald`. |
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
- **Routine changes**: the next routine is scheduled a beat ahead and crossfades over two beats centred on the bar
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
  a few bars, and hands that never hit the same spot twice (a slow wander of a centimetre or two).
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
  hair (natural, or dyed: often for rave / pop / robot), a colour scheme (a base hue and a harmony: complementary,
  analogous, triad or mono, with the style's saturation and brightness: neon rave, candy pop, muted chill, greys
  and one accent for robot, reds / black / gold for latin, 70s oranges / teal for retro), the style's wardrobe
  (weighted outfits, hats, hair, prints and fabrics; metallic tops are often gold or silver), sunglasses (each
  style its favourite shapes; mirrored lenses), glow sticks (rave mostly) and a face. Tall hair never meets a hat;
  beards only with natural hair. The same seed gives the same character; each slideshow start is a new seed.
- **Proportions** (Head, Legs, Torso, Shoulders, Belly, Limbs): the skeleton and the part radii scale; hair, hats,
  ears, nose and glasses follow the head size.
- **Prints** (drawn in the shader, free): stripes, dots, plaid, gradient, zigzag, in the part's own coordinates
  (around and along it), antialiased; a dress takes the top's print.
- **Fabrics**: shiny (a tinted reflection of the visuals: the light texture sampled along the reflected view;
  a studio sky on a dark frame), sequins (facets of the print grid, each sparkling on its own beat), velvet (dark
  face-on, a sheen in its own colour at the edges), neon (unlit, saturated, pulsing on the beat: glow sticks too).
- **Sunglasses**: flat lens discs in front of the eyes cut to their shape in the shader (round, aviator, visor:
  one wide lens, heart, star, cat-eye: the outer corner swept up, square), a frame rim drawn on the edge, a bridge
  and arms to the ears; dark, tinted or mirrored lenses.
- **Faces**: eye shapes (oval, round, sleepy with a lid line, happy arcs, big with two highlights, dot), lashes,
  brows (normal, thick in the hair colour, thin, arched), mouths (smile, grin with teeth, small, smirk), lipstick,
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
- **Pompadour**: tight short sides and a big roll (a wide capsule) rising up and forward over the forehead.
- **Side part**: short hair under a second layer cut by a tilted plane, so the hairline runs from high over one
  brow down over the other brow and the outer corner of that eye. (Strands laid along the head looked beaded.)
- **Undercut**: the sides and back a thin shell in a fade colour (hair over skin), a thick cap on top (its cut edge
  shows as a solid slab) and a flick at the front.
- **High top**: a tall flat-topped block (a capsule clipped flat, its inside shaded as solid hair), faded sides.
- **Bowl**: a shell cut level all round just above the brows.
- **Mullet**: short on top, a flattened mass hanging down the back of the neck with two tapering side pieces.
- **Shag**: a full mass with pointed tufts flicking out round the back and sides at the jaw and wispy bangs.

Generated characters wear them by style (pompadour, shag, mullet, bowl for retro; high top, undercut for hip-hop;
side part for pop and rave; curly for disco and latin; ...). Curly, pompadour, high top and undercut count as tall
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
  360-wave rings, coils (a nubbly texture, no lines); a wavy flag. Very dark hair colours now read on the dark stage. A second
  clip plane (`clip2`) per hair part. Packed into slots only the head reads (face.w, eyes.w, the lips); ported to
  the WebGL preview.
- **Long hair** (bob, lob, long, layers, wavy, long curls, bangs, shag, half-up): a close top (parted) over a
  curtain: a tube round the head, open in front of the cheeks (its inside shows as the hair behind the neck), cut
  level at the hem by `clip2`, swinging with the dance. Layers: the hem slants up to the front. Wavy, shag and long
  curls flare out towards the ends (curls: coils and a ring of ringlet ends).
- **Lifts** (side part, quiff, pompadour, slick back, undercut, pixie's fringe, messy): a layer like the head's
  ball, off-centre, so it stands off the head where it should (the front, the top) and sinks into the hair under it
  elsewhere, cut by planes across the forehead (and along the parting).
- **Merged / reworked**: crew is short (`crew` still parses); side part is now a classic short side part (the
  emo sweep is gone); pompadour and the new quiff rise from the forehead; balding is a smooth band round the back and
  sides; curtains are short at the sides; high top is lower, a little wider at the top; ponytail is high; pigtails
  low, parted; braid a side braid over the shoulder; man bun at the crown; locs 22 ropes plus two each side in front
  of the ears.
- **New** (41 styles): quiff, messy, coils, waves (360), lob, long curls, layers, half-up, low bun. `quiff` and
  `waves` now name their own styles (they were aliases of pompadour and wavy).
- **Generated characters**: every dance style draws on everyday hair, each with a small flavour of its own; any
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
  pony, space buns. Short, side part, waves, spiky and mohawk lean masculine and long, ponytail and top knot
  feminine: on the other side's characters now and then (x0.15). Either way: curly, coils, afro, curtains, shag,
  anime.
- **Shaved** (new, masculine): a close shell barely darker than the skin, smooth (`kStrandsNone`: strands or the
  coil texture read as a cap). 36 styles.
- **Every style draws from the same everyday hair** (long, bob, ponytail, layers, half-up, bangs, bun, braid, pixie,
  side pony, pigtails, top knot; short, side part, quiff, buzz, undercut, shaved, slick back, waves, balding, man
  bun, bald, mullet, bowl; curly, coils, afro, curtains, shag, anime, spiky, mohawk, high top), plus a small flavour
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
then, an aviator rarely; black, tortoiseshell, gold, silver or the scheme's second colour; a little more often for
lock and chill), the same for feminine and masculine characters.

## 3D beards (Peter, 2026-11-21)

"Broadly make beards look better": hair material, 3D structure, more options, aligned with the mouth, ears and hair.
They were painted on the face; now all but stubble are built (DancerRig buildBody, after the hats), in the hair
colour, drawn with strands (Material::Strands), laid out in the face's own angles (drawFace: u around, v up):

- **Moustache**: a centre piece under the nose and a tapering, drooping piece each side to the mouth's corners,
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
- Pieces on the face (moustache, goatee, circle, handlebar) are centred on the face's surface (flat against it), so
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
  the head centre, a mass or lock towards its lowest point), or is dropped if it would have to shrink by over 55 %.
  Top hats keep the usual shapes (hair shows round and below the brim) through the same fitting.
- Test: every hairstyle under cap, beanie and top hat, at rest and through 5 s of dancing with swinging hair: no hair
  point outside the hat as built (its dome above its cut, or its crown above the brim). Without the fitting it fails
  84 checks.
- Cost: buildBody 1 us without a hat, about 12 us with a cap or beanie, 6.5 us with a top hat.

## Hair colours (Peter, 2026-11-20)

Natural hair: 12 colours, equally likely: black, soft black, dark brown, brown, chestnut, auburn, copper, dark blond,
blond, grey, silver, platinum (silver and platinum moved here from dyed). Dyed hair is a bright colour of any hue and
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
  time in groups than the least.
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
  Left out: floor work (hips more than 0.45 leg down: freezes, drops; most break routines), lying or upside down.
- **Tempo (Peter: "I generally DJ between 120 and 130")**: of each basic choreography's dozen performances, every
  usable one recorded at 120-130 BPM is kept (Peter, 2026-11-21: more performances per basic move, now that the
  dancers use only these; different dancers do the same step differently), at least 2 (the best of the rest by
  score, where a tempo outside 120-130 costs 0.03 per BPM, plus the loop closure, steady tracking and movement).
  Labels: "house basic 1 · dancer 19 · take 2" (takes numbered per dancer, slowest first). The choreographer weights
  mocap moves by tempo too (1 within 118-132, less further off, at least 1/3), since every usable routine is kept.
- Result: 543 moves: 376 basic (4 bars), 123 routines of 16 bars and 44 of 8. Basic / 16-bar / 8-bar per genre:
  house 60 / 13 / 4, pop 40 / 20 / 0, lock 40 / 16 / 4, break 20 / 1 / 6, middle hip-hop 39 / 17 / 3, LA hip-hop
  40 / 21 / 0, krump 37 / 9 / 8, waack 35 / 14 / 5, street jazz 36 / 3 / 12, ballet jazz 29 / 9 / 2. Recorded at
  120-130 BPM: 438. content/Moves/aist.moves, 11 MB: channels quantised to 8 bits over their range within a move
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
  read (the formations and sync modes are still in engine/Dancers and its tests).
- **Watching is AIST too** ("move to 100% AIST"): measured before, a mingling dancer spent 9-11 % of the time
  watching someone (cheering a soloist, hyping someone showing off, a partner's turn) with the hand-made claps,
  points and nods; now `watchPose` dances a calm basic mocap move of their own style (chill energy), a new one every
  4 bars (seeded per dancer), turned towards the one watched (the stage sets the facing and the head turn). The
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
  routines half the time; it measured 40 % with halves played once).
- **Panel**: the "New dancers in these styles" chips (always all on, Peter: "doesn't do me much good") became one
  **New dancer** button per style (`switch:dancer:Random <Style>`), and the Dancers card has no library list any more
  (only the Random entries were left in it; Past guests has its own list). `reforce.Dancer.RandomStyles` stays, in
  settings.json only, for Start next and the slideshow. The buttons sit three to a row; blocked guests (✕ unblocks)
  are listed at the end of the Past guests section (Peter: blocking won't be common).

## Styles are the dance genres (Peter, 2026-11-21)

"Choosing a dance move genre should be the top-level thing, and then choosing an appearance to go with it should be
secondary." The dancer styles are now the AIST++ genres, two pairs combined and two renamed (Peter: "I'm at least not
familiar with those terms"): **house**, **pop** (popping), **lock** (locking), **break** (breaking; toprock only, the
floor work is left out), **disco** (waack), **stomp** (krump), **hip-hop** (middle hip-hop, labelled "party hip-hop",
and LA hip-hop) and **jazz** (street and ballet jazz). `Style` in engine/DanceMoves: House, Pop, Lock, Break, Disco,
Stomp, HipHop, Jazz (+ Ballet, below).

- **Moves**: each AIST genre is exactly one style (DanceClips `clipGenreStyle`), so a dancer dances only its own
  genre(s); the old genre -> styles table is gone. Basic / 16-bar / 8-bar moves per style: house 60 / 13 / 4, pop
  40 / 20 / 0, lock 40 / 16 / 4, break 20 / 1 / 6, disco 35 / 14 / 5, stomp 37 / 9 / 8, hip-hop 79 / 38 / 3, jazz
  65 / 12 / 14 (tested: every style has at least 20 basic moves and 5 routines).
- **Looks**: `generateDancer` keeps the wardrobes written for the old styles and dresses each new one from the closest:
  house from house (30 % rave gear: neon, glow sticks, dyed hair), pop from robot (greys and one bright accent,
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

