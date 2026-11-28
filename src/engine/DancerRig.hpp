#pragma once
// reforce extension (PLAN 5P, docs/dancers-design.md): the dancer's body. Pure C++ (no Metal), unit-tested.
//
// Character space: +y up, the floor at y = 0, the character faces +z (towards the camera). The character's own LEFT
// is +x (screen right when facing the camera). A standing character is about 1 unit tall (Tall = 1).
//
// A pose (DancePose) gives the whole-body placement, the pelvis, chest and head rotations, and IK targets for the
// hands (chest frame) and feet (body frame). buildBody() solves the two-bone IK chains (elbows bend back and out,
// knees forward) and emits the body as a list of rounded parts (BodyPart): every part is one instance of the
// "capsule" primitive the GPU draws (render/DancerPipeline): two hemispheres of radii r0 (at the part's origin) and
// r1 (at `len` along its local +y), joined by a tapered band, then scaled by `scale` across the axis (x, z). A
// sphere is len 0, an ellipsoid adds scale, a cone has r1 ~ 0.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace reforce::engine::dance {

struct V3 {
    float x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline V3 operator*(float s, V3 a) { return a * s; }
inline V3 operator-(V3 a) { return {-a.x, -a.y, -a.z}; }
float dot(V3 a, V3 b);
V3 cross(V3 a, V3 b);
float length(V3 a);
V3 normalize(V3 a, V3 fallback = {0, 1, 0});
V3 lerp(V3 a, V3 b, float t);

// An orthonormal frame (rigid transform): world = o + x * p.x + y * p.y + z * p.z.
struct Frame {
    V3 o{0, 0, 0};
    V3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
    V3 point(V3 p) const { return o + x * p.x + y * p.y + z * p.z; }
    V3 dir(V3 v) const { return x * v.x + y * v.y + z * v.z; }
    // This frame followed by a local translation / rotation (child = this * local).
    Frame translated(V3 local) const;
    Frame rotatedX(float a) const;  // about the local x axis (+a tips +y towards +z: a forward lean)
    Frame rotatedY(float a) const;  // about local y (+a turns +z towards +x: the character turns to its left)
    Frame rotatedZ(float a) const;  // about local z (+a tips +y towards -x: a lean to the character's right)
};
// A frame at `o` whose y axis points along `axis`, x as close as possible to `xHint`.
Frame frameAlong(V3 o, V3 axis, V3 xHint);

struct Rgb {
    float r = 1, g = 1, b = 1;
};
std::optional<Rgb> parseRgb(std::string_view text);  // "#rrggbb" / "rrggbb" / "#rgb"

// ---- character description (scene keys, engine/Dancers.hpp)
enum class HairStyle {
    Short, Bob, Long, Spiky, Afro, Ponytail, Bun, Bald, Mohawk, Pigtails,
    // 2026-11-20 (Peter: more hairstyles)
    Curly, Pompadour, SidePart, Undercut, Bowl, Mullet, Shag,  // (high top cut 2026-11-21)
    // 2026-11-20 (Peter, for stream guests: more hairstyles, women's styles especially)
    Pixie, Bangs, Braid, SidePony, SpaceBuns, TopKnot,
    // 2026-11-20 (Peter: more standard men's haircuts; crew merged into short 2026-11-21)
    Buzz, SlickBack, Curtains, Balding, ManBun,
    // 2026-11-21 (Peter: better looking "normal" hair; wavy, locs, crop, lob, long curls, low bun and messy cut the
    // same day, their names read as the closest left)
    Quiff, Coils, Waves, Layers, HalfUp, Anime, Shaved,
    // 2026-11-27 (Peter: "a couple of nice looking long curly feminine hair options"): a long curly mane widening to
    // the shoulders, in three sizes (spiral ringlets tried the same day and dropped; the name reads as Curls)
    Curls, BigCurls, HugeCurls
};
inline constexpr int kHairStyleCount = int(HairStyle::HugeCurls) + 1;
// Skirt: a top over a short skirt (the bottom color); CropTop: a short top over the bottom, a bare midriff;
// Jumpsuit: top and trousers in one (the top color) with a belt (accent); LongDress: a dress to the ankles.
enum class Outfit { Tee, Tank, Hoodie, Dress, Suit, Skirt, CropTop, Jumpsuit, LongDress };
inline constexpr int kOutfitCount = int(Outfit::LongDress) + 1;
enum class Hat { None, Cap, Beanie, Top, Headphones };
// Cloth (Peter, 2026-11-19: more variety): a print on the fabric and how the fabric takes the light.
enum class Pattern : std::uint8_t { None, Stripes, Dots, Plaid, Gradient, Zigzag, Count };
enum class Fabric : std::uint8_t {
    Matte,    // the plain toon plastic
    Shiny,    // lame / metallic: reflects the visuals
    Sequins,  // metallic facets that sparkle on the beat
    Velvet,   // dark in the middle, a soft sheen at the edges
    Neon,     // glows by itself (blacklight / glow sticks): bright even on a black frame, pulses on the beat
    Count
};
enum class Glasses : std::uint8_t { None, Round, Aviator, Visor, Heart, Star, CatEye, Square, Count };
enum class EyeShape : std::uint8_t { Oval, Round, Sleepy, Happy, Big, Dot, Count };
enum class Brows : std::uint8_t { Normal, Thick, Thin, Arched, Count };
enum class Mouth : std::uint8_t { Smile, Grin, Small, Smirk, Count };
// Stubble is drawn on the face; the others are 3D (2026-11-21), in the hair color, drawn with strands.
// (Short took chinstrap's place 2026-11-21; "chinstrap" reads as short.)
enum class Beard : std::uint8_t { None, Stubble, Moustache, Goatee, Full, Handlebar, Circle, Short, Long, Count };
// Whether a beard has a moustache (the face then draws the mouth a little lower, clear of it).
inline bool beardHasMoustache(Beard b) {
    return b == Beard::Moustache || b == Beard::Full || b == Beard::Handlebar || b == Beard::Circle || b == Beard::Short ||
           b == Beard::Long;
}
std::optional<HairStyle> parseHairStyle(std::string_view s);
std::optional<Outfit> parseOutfit(std::string_view s);
std::optional<Hat> parseHat(std::string_view s);
std::optional<Pattern> parsePattern(std::string_view s);
std::optional<Fabric> parseFabric(std::string_view s);
std::optional<Glasses> parseGlasses(std::string_view s);
std::optional<EyeShape> parseEyeShape(std::string_view s);
std::optional<Brows> parseBrows(std::string_view s);
std::optional<Mouth> parseMouth(std::string_view s);
std::optional<Beard> parseBeard(std::string_view s);

struct Look {
    Rgb skin{0.96f, 0.80f, 0.66f};
    Rgb hair{0.25f, 0.16f, 0.10f};
    Rgb top{0.90f, 0.30f, 0.35f};
    Rgb bottom{0.20f, 0.26f, 0.42f};
    Rgb shoes{0.95f, 0.95f, 0.95f};
    Rgb eyes{0.20f, 0.14f, 0.10f};
    Rgb accent{0.15f, 0.15f, 0.18f};  // hat / headphones / suit tie
    HairStyle hairStyle = HairStyle::Short;
    Outfit outfit = Outfit::Tee;
    Hat hat = Hat::None;
    float build = 1.0f;  // body width, 0.8 .. 1.25
    float tall = 1.0f;   // overall height, 0.85 .. 1.15
    // Proportions (1 = the standard cartoon body).
    float head = 1.0f;       // head size, 0.8 (grown-up) .. 1.3 (big cartoon head)
    float legs = 1.0f;       // leg length, 0.8 .. 1.2
    float torso = 1.0f;      // torso length, 0.85 .. 1.15
    float shoulders = 1.0f;  // shoulder width, 0.85 .. 1.2
    float belly = 0.0f;      // 0 .. 1: a rounder middle
    float limbs = 1.0f;      // arm and leg thickness, 0.8 .. 1.3
    // Cloth: prints (in `ink`) and fabrics for the top and the bottom (a dress is the top).
    Pattern topPattern = Pattern::None, bottomPattern = Pattern::None;
    Fabric topFabric = Fabric::Matte, bottomFabric = Fabric::Matte;
    Rgb ink{1.0f, 1.0f, 1.0f};
    // Glasses (lens and frame colors; a Shiny lens is mirrored; a clear lens is just the frame, the eyes showing
    // through) and glow sticks (one in each hand).
    Glasses glasses = Glasses::None;
    Rgb lens{0.08f, 0.08f, 0.1f};
    bool clearLens = false;
    Rgb frame{0.1f, 0.1f, 0.1f};
    bool mirrored = false;
    bool glowSticks = false;
    Rgb glow{0.3f, 1.0f, 0.4f};
    // Face.
    EyeShape eyeShape = EyeShape::Oval;
    Brows brows = Brows::Normal;
    Mouth mouth = Mouth::Smile;
    Beard beard = Beard::None;
    bool lashes = false;
    bool freckles = false;
    float blush = 0.35f;       // 0 .. 1
    std::optional<Rgb> lips;   // lipstick
};

// ---- skeleton dimensions (character units) for a Look
struct Skeleton {
    float ankleY;        // ankle height above the floor when the sole is flat
    float hipY;          // pelvis origin height at rest
    float hipHalf;       // half the distance between the hip joints
    float thigh, shin;   // leg bone lengths
    float spine;         // pelvis origin -> chest origin
    float shoulderHalf;  // half the shoulder width
    float shoulderY;     // shoulder joint height in the chest frame
    float upperArm, forearm;
    float neck;          // chest origin -> head base
    float headR;         // head radius
    float footHalf;      // rest stance: half the distance between the ankles
};
Skeleton makeSkeleton(const Look& look);

// ---- pose
struct DancePose {
    // Whole body including the feet: offset on the floor and turn (radians, + turns to the character's left).
    float bodyX = 0, bodyZ = 0, bodyYaw = 0;
    // Pelvis offset from its rest position (body frame; y < 0 bends the knees) and rotation (applied yaw, roll,
    // pitch). Positive pitch leans forward (towards the camera), positive roll leans to the character's right.
    V3 hip{0, 0, 0};
    float hipYaw = 0, hipRoll = 0, hipPitch = 0;
    float chestYaw = 0, chestRoll = 0, chestPitch = 0;  // chest relative to the pelvis
    float headYaw = 0, headRoll = 0, headPitch = 0;     // head relative to the chest (+pitch nods down)
    // Hand targets in the chest frame (origin at the chest, between the shoulders; +x the character's left).
    V3 handL{0.20f, -0.22f, 0.03f}, handR{-0.20f, -0.22f, 0.03f};
    // Ankle targets in the body frame (y = 0: the sole on the floor, i.e. the ankle at its rest height).
    V3 footL{0.085f, 0, 0}, footR{-0.085f, 0, 0};
    float footYawL = 0, footYawR = 0;      // toes out (+) / in (-), radians
    float footPitchL = 0, footPitchR = 0;  // toes up (+) / down (-)
    // Face.
    float mouth = 0;  // 0 = closed smile .. 1 = wide open
    float blink = 0;  // 0 = open .. 1 = shut
    float brow = 0;   // -1 = frown .. 1 = raised
    float fist = 0;   // 0 = open mitten .. 1 = fist (slightly smaller hands)
    // Bend hints (mocap moves): the direction an elbow (chest frame) or knee (body frame) bends, its length how much
    // it counts (0 = the rig's own choice: elbows back and out, knees forward; 1 = this direction). Mixed and
    // mirrored like the targets, so a move without hints fades smoothly into one with them.
    V3 elbowL{}, elbowR{}, kneeL{}, kneeR{};
    // Floor work (docs/dancers-design.md "Break floor work"): the pelvis's whole turn in the body frame as a unit
    // quaternion (x, y, z, w), applied before hipYaw / roll / pitch (Euler angles lock where a body lies on its side;
    // identity for every standing move), and how groundPose holds the upper body to the floor: `ground` 0..1 how much
    // (floor moves 1, mixed like the rest), `clear` how far above the floor its lowest point should be (character
    // units; the recorded dancer's).
    float hipQx = 0, hipQy = 0, hipQz = 0, hipQw = 1;
    float ground = 0, clear = 0;
    // How much each hand (0 left, 1 right) is planted on the floor (0..1: the source dancer's hand was down; groundPose
    // pulls the hand ball onto the floor by this much; 2026-11-27).
    float plantL = 0, plantR = 0;
    // Planted contacts (2026-11-27, "Floor moves held still"): how much the source dancer had each hand, foot, knee and
    // the head planted (down on the floor and hardly moving across it; 0..1, from the floor moves' file). DancerRun
    // holds a planted contact where it came down (pinContacts). Mixed and mirrored like the rest; 0 standing.
    float pinHandL = 0, pinHandR = 0, pinFootL = 0, pinFootR = 0, pinKneeL = 0, pinKneeR = 0, pinHead = 0;
    // Hair in motion (set by the dancer's hair springs after the pose is made; not part of the pose floats, not
    // mixed or mirrored): how far the hanging hair on the character's left (0) and right (1) swings from where it
    // hangs at rest, in the head's frame, per unit length of the hair (about radians). Zero: still.
    V3 hairSwing[2] = {};
};
// The rest pose for a skeleton: hands hanging, feet at the stance width.
DancePose restPose(const Skeleton& s);
// Per-field interpolation (vectors and angles linearly).
DancePose mix(const DancePose& a, const DancePose& b, float t);
// A pose as a flat list of numbers (for filters: inertial blending). kPoseFloats values, in a fixed order.
inline constexpr int kPoseFloats = 62;  // the pelvis quaternion at 47..50, the hands' plant at 53, 54, pins 55..61
void poseToFloats(const DancePose& p, float* out);
DancePose poseFromFloats(const float* in);
// The same pose mirrored left <-> right (x negated, yaw and roll negated, L and R swapped).
DancePose mirrored(const DancePose& p);
// The look's hair and hat round the head as groundPose holds them off the floor (2026-11-27): points on the outside
// of every part that moves with the head (hats, big hair, buns, spikes, the shells; not the hair hanging below it,
// which the hair sim keeps above the floor), in the head frame (character units, origin at the head center), the
// farthest out each of a set of directions. n 0: nothing past the head ball (bald, short hair).
struct HeadExtent {
    static constexpr int kMax = 64;
    int n = 0;
    V3 pts[kMax];
};
HeadExtent headExtent(const Look& look, const Skeleton& sk);
// Floor work: the upper body (pelvis, chest, shoulders, elbows, hands, knees, head, and `ext`, the look's hair and
// hat) moved up or down so its lowest point is `clear` above the floor where the move has it near the floor, and
// never below the floor; feet in the air go with it. Then a planted hand (plantL / R) is pulled down onto the floor
// by as much (its target moved; where the arm is too short the body is lowered, at most 0.12 leg and never putting
// anything else through the floor). Moves the pelvis (hip.y), lifted feet and planted hands only. Nothing for standing
// poses (ground 0).
void groundPose(const Skeleton& sk, DancePose& p, const HeadExtent* ext = nullptr);
// The hand ball's lowest point (height in character space, as buildBody makes it, the arm not kept out of the head)
// for side 0 left / 1 right.
float handBallBottom(const Skeleton& sk, const DancePose& p, int side);

// ---- two-bone IK
struct IkResult {
    V3 mid;     // elbow / knee
    V3 end;     // wrist / ankle (the target, pulled in when out of reach)
    bool reached = true;
};
// Root at `root`, bones a then b; the mid joint bends towards `pole` (a direction). The chain never fully
// straightens (at most 99.5 % of a + b) so the bend direction stays defined.
IkResult solveTwoBone(V3 root, V3 target, float a, float b, V3 pole);

// ---- body parts
enum class Material : std::uint8_t {
    Plain = 0,  // toon-shaded color (color0 below `split`, color1 above)
    Head = 1,   // skin with the drawn face (DancePose mouth / blink / brow, Look eyes)
    Hair = 2,   // drawn on both sides (shells cut open at the face show their inside, shaded as solid hair)
    Lens = 3,   // a sunglasses lens: a flat disc cut to `shape` (pattern), color0 lens, color1 frame rim
    Strands = 4,  // hair: as Hair, plus strands, a sheen band and a parting drawn in the shader (`strands`), and
                  // a second clip plane (`clip2`)
};

// How the shader draws a hair part's strands (BodyPart::strands).
enum Strands : std::uint8_t {
    kStrandsAlong = 0,     // along the part's axis: from the crown down a shell, down a lock or a tail
    kStrandsParted = 1,    // combed sideways from a front-to-back parting at local x = strandPart (x r0)
    kStrandsBack = 2,      // combed straight back, front to back over the top
    kStrandsWaveCut = 3,   // 360 waves: ripples in rings round the crown
    kStrandsCoils = 4,     // coils and curls: a nubbly texture, no lines
    kStrandsNone = 5,      // smooth (a shaved head's shadow)
};

struct BodyPart {
    Frame frame;               // origin at end 0, +y along the part
    float len = 0;             // distance between the two ends
    float r0 = 0.05f, r1 = 0.05f;
    float scaleX = 1, scaleZ = 1;  // across the axis (ellipsoids, flattened torso)
    Rgb color0, color1;      // color1 above `split` (local y, before scale), color0 below
    float split = 1e9f;
    std::array<float, 4> clip{0, 0, 0, 1e9f};  // local plane: discard where dot(p, xyz) > w (p before scale)
    Material material = Material::Plain;
    bool outline = true;       // drawn in the outline pass
    // Cloth: print and fabric of the color0 region [0] and the color1 region [1]; the print in `ink`, `period`
    // its repeat (world units). For a Lens, pattern[0] is the Glasses shape.
    std::uint8_t pattern[2] = {0, 0}, fabric[2] = {0, 0};
    Rgb ink;
    float period = 0.05f;
    // Hair (Material::Strands): the strand look and the parting (x r0, local x; > 5: none), and a second plane cut
    // (as `clip`: what either plane cuts away is gone).
    std::uint8_t strands = kStrandsAlong;
    float strandPart = 9;
    std::array<float, 4> clip2{0, 0, 0, 1e9f};
    // Long hair that bends (BodyOut::bend): drawn bent along the hanging hair's columns (DancerPipeline).
    bool bent = false;
    // Hanging hair (a chain's links, the long hair's tube, the curls riding it): moves with the hair, not the head
    // (headExtent leaves it to the hair sim).
    bool hangs = false;
};

// Long hair that bends (reforce.Dancer.LongHair 1, 2026-11-27; docs/dancers-design.md "Hair physics"): the long
// styles' tube round the head, one part, is drawn bent. In the part's own frame with x and z scaled (world units:
// p = (x * scaleX, y, z * scaleZ)): rigid down to y0 along its axis (local y); below that a point follows the curve of
// a column (the back, the left (+x) and the right side), blended across the tube by its local x / radius (-1 right,
// 0 back, 1 left). A column's curve is a cubic Bezier from (0, y0, 0) leaving along the axis (its first control
// point (0, y0 + (len - y0) / 3, 0)) to p3 (p2 the other control point), t = (y - y0) / (len - y0); a point on the
// tube is its column's anchor (where the column runs on the wall, x and z; the right side mirrors the left) moved
// along the curve, its offset from the anchor turned as the curve turns (so the wall keeps its width). Normals turn
// with the curve. The shader and bendHairPoint do the same.
struct HairBend {
    float y0 = 0;
    V3 p2[3], p3[3];         // back, left, right
    float backZ = 0;         // the back column's anchor (0, backZ)
    float sideX = 0, sideZ = 0;  // the left column's anchor (sideX, sideZ); the right one's (-sideX, sideZ)
};
// A point of a bent part (world, as drawn unbent) where the bend puts it; `normal` (optional) turned with it.
V3 bendHairPoint(const BodyPart& p, const HairBend& b, V3 world, V3* normal = nullptr);

// Face parameters for the head part (from the pose).
struct FaceState {
    float mouth = 0, blink = 0, brow = 0;
    Rgb eyes;
    EyeShape eyeShape = EyeShape::Oval;
    Brows brows = Brows::Normal;
    Mouth mouthStyle = Mouth::Smile;
    Beard beard = Beard::None;
    bool lashes = false, freckles = false;
    float blush = 0.35f;
    Rgb hair;                  // beard color
    Rgb lips{0.3f, 0.07f, 0.09f};
    bool lipstick = false;
};

// ---- hair physics (Peter, 2026-11-27; docs/dancers-design.md "Hair physics")
// Hanging hair (tails, braids, the hanging part of long hair, mullets, locks under a hat) is a short chain of points
// (DancerRun's hair sim): pinned at the root to the head, pulled towards its rest shape (softer towards the tip),
// pulled down by gravity, and pushed out of the body. buildBody draws each chain bent along its points. Looser
// volumes (afro lobes, curls, buns, spikes, lifts, the shells' strands) lag the head through one small spring.
inline constexpr int kHairChains = 4, kHairChainPts = 5;  // up to 4 segments a chain
struct HairChainDef {
    V3 a, b;               // root and rest tip in the head frame (character units, origin at the head center)
    int segs = 4;          // 1 .. kHairChainPts - 1
    float radius = 0.03f;  // collision radius (character units)
    float floor = 0;       // how far above the floor it is kept (0: `radius`; the curls riding long hair: more)
    float droop = 0.5f;    // how much gravity bends its rest shape (0: holds its shape, 1: hangs)
    float stiff = 1.0f;    // shape stiffness (1: a tail)
    bool head = true;      // kept outside the head
    // How far (radians) the chain's end may turn from its rest direction in the head frame (0: no limit), link i
    // i / segs of it (a bend that grows down the chain), upright; upside down up to about twice that (at most 2.9),
    // and nearer the root more of it, so the hair can fall. A link past it turns back softly (a hard limit fought the collisions and popped). Long
    // hair that bends: its tube leaves the head along the rest direction and can't fold back on itself. (Also marks
    // a long column: its first link, by the shoulder, doesn't collide with the arms.)
    float cone = 0;
    float damp = 1;  // the damping of a link's motion relative to the one above it, x this (more: settles sooner)
};
struct HairState {
    int chains = 0;
    V3 pts[kHairChains][kHairChainPts] = {};  // character space (as the pose), the root first
    // Loose volumes: their lag behind the head, in the head frame: a shift (character units) and a turn (radians,
    // an axis times the angle).
    V3 lagT{}, lagR{};
    // reforce.Dancer.LongHair 1: the long styles hang as three columns (back, left, right) the tube bends along
    // (HairBend); 0 the one chain the rigid tube turns towards.
    bool bendLong = false;
};
// The body as the hair collides with it (character space): rounded capsules (two ends, radii), the torso flattened
// (its own frame: across it scaled by sx, sz), the head a ball.
struct HairCollider {
    V3 a, b;
    float r0 = 0, r1 = 0;
    V3 x{1, 0, 0}, z{0, 0, 1};  // the torso's across axes (sx, sz != 1 only)
    float sx = 1, sz = 1;
};
inline constexpr int kHairColliders = 8;  // torso, head, upper arms, forearms, hands
enum HairColliderIndex { kColTorso = 0, kColHead = 1, kColArms = 2 };
void hairColliders(const Look& look, const Skeleton& sk, const DancePose& pose, HairCollider out[kHairColliders]);
// How far a ball (center p, radius r) reaches into a collider (> 0: inside), and the way out (unit).
float colliderDepth(const HairCollider& c, V3 p, float r, V3* out = nullptr);

struct BodyOut {
    std::vector<BodyPart> parts;
    // The hanging hair chains this look has (as buildBody laid them out; in order), for the hair sim.
    HairChainDef hairChains[kHairChains];
    int hairChainCount = 0;
    // The bend of the part marked `bent` (long hair that bends; one a body at most).
    HairBend bend;
    FaceState face;
    V3 headCenter;  // world
    float headTop = 0;  // world height of the highest point of the head, hair and hat (name tags)
    V3 feet[2];     // world ankle positions (L, R), for shadows
    V3 rootFloor;   // world floor point under the pelvis
    float ground = 0;  // floor work: the pose's `ground` (0..1; shadows)
    float size = 1;    // the placement's scale (shadows)
    // How far an arm (upper arm, forearm or hand) reaches into the head, in character units: as posed, and as built
    // (arms are kept out of the head). Tests.
    float armInHeadPosed = 0, armInHead = 0;
    V3 hands[2], handsPosed[2];  // wrists (L, R) as built / as posed (before keeping out of the head); tests
    std::size_t headBegin = 0;   // parts from here on: the head, face, glasses, hair and hat
};

// The head frame (character space, before placement): origin at the head center, y up, z out of the face.
Frame headFrame(const Skeleton& sk, const DancePose& pose);
// The chest frame (character space): origin between the shoulders' roots; the hand targets live in it.
Frame chestFrame(const Skeleton& sk, const DancePose& pose);
// A knee as the rig places it (character space; side 0 left, 1 right), and the pelvis's origin.
V3 kneeAt(const Skeleton& sk, const DancePose& pose, int side);
V3 pelvisAt(const Skeleton& sk, const DancePose& pose);

// Places a posed character: `placement` is its spot on the stage (origin on the floor, y up, z towards the
// camera), `scale` multiplies everything (entrance pop, stage size). Appends to out.parts. `hair`: the hair sim's
// chains and volume lag (hair physics); null: the classic swing (pose.hairSwing, one rigid piece each).
void buildBody(const Look& look, const Skeleton& sk, const DancePose& pose, const Frame& placement, float scale,
               BodyOut& out, const HairState* hair = nullptr);

}  // namespace reforce::engine::dance
