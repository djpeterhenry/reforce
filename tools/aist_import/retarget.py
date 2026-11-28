"""AIST++ 3D keypoints -> reforce dancer pose channels (tools/aist_import).

Frames (reforce's, docs/dancers-design.md "Animation"): +x the character's left, +y up, +z forward (towards the
audience). Rotations are applied yaw (about y), then roll (about z), then pitch (about x), as DancerRig's Frame does.

Every channel is independent of body size: lengths are divided by the source dancer's own arm or leg length, and the
engine multiplies them back by the character's (engine/DanceClips). Channels (CHANNELS below):
  body x, z (/ leg), yaw              where the whole body is on the floor and which way it faces
  hip x, y, z (/ leg), yaw roll pitch the pelvis relative to the body (y: below standing height, knees bent)
  chest yaw roll pitch                relative to the pelvis
  head yaw roll pitch                 relative to the chest
  hand L / R (u, v, w)                the wrist in the chest frame from its own shoulder, / arm: u across (+ the
                                      character's left), v up, w forward. (The engine pulls hands towards the middle
                                      by however much wider its characters' shoulders are, so claps still meet.)
  foot L / R (u, y, z)                the ankle in the body frame: u as for the hands with the stance half-width,
                                      y above its standing height / leg, z / leg
  foot yaw L R, foot pitch L R        toes out (+), toes up (+)
  elbow L / R, knee L / R (x, y, z)   bend hints: the direction the joint bends (chest frame for elbows, body frame
                                      for knees), as long as the bend is clear (0 = straight: the rig's own default)
"""
import numpy as np
from scipy.ndimage import gaussian_filter1d
from aistlib import *

CHANNELS = ['bodyX', 'bodyZ', 'bodyYaw', 'hipX', 'hipY', 'hipZ', 'hipYaw', 'hipRoll', 'hipPitch',
            'chestYaw', 'chestRoll', 'chestPitch', 'headYaw', 'headRoll', 'headPitch',
            'handLu', 'handLv', 'handLw', 'handRu', 'handRv', 'handRw',
            'footLu', 'footLy', 'footLz', 'footRu', 'footRy', 'footRz',
            'footYawL', 'footYawR', 'footPitchL', 'footPitchR',
            'elbowLx', 'elbowLy', 'elbowLz', 'elbowRx', 'elbowRy', 'elbowRz',
            'kneeLx', 'kneeLy', 'kneeLz', 'kneeRx', 'kneeRy', 'kneeRz']
# Floor work (docs/dancers-design.md "Break floor work"): the pelvis's whole turn as a quaternion in the body frame
# (hipYaw / roll / pitch stay 0: Euler angles lock where a body lies on its side), and how far the upper body's lowest
# point (hands, elbows, knees, shoulders, pelvis, the top of the head) is above the floor, / leg; then (2026-11-27)
# each hand's, knee's and the head's own clearance (/ leg, 0 .. CONTACT_MAX): the engine plants a hand the source
# dancer had on the floor (DancerRig groundPose).
CONTACT_CHANNELS = ['floorHandL', 'floorHandR', 'floorKneeL', 'floorKneeR', 'floorHead']
# Planted contacts (2026-11-27, "Floor moves held still"): 0 .. 1 how much each hand, foot, knee and the head is
# planted on the floor in the source: near it and hardly moving across it (pins() below). The engine holds a planted
# contact where it came down (DancerRun pinContacts): our retargeted hands and knees otherwise skate (proportions).
PIN_CHANNELS = ['pinHandL', 'pinHandR', 'pinFootL', 'pinFootR', 'pinKneeL', 'pinKneeR', 'pinHead']
FLOOR_CHANNELS = CHANNELS + ['hipQx', 'hipQy', 'hipQz', 'hipQw', 'clear'] + CONTACT_CHANNELS + PIN_CHANNELS
CONTACT_MAX = 0.5  # (above it: off the floor; held there so the 8 bits are fine near the floor)
CH = {n: i for i, n in enumerate(FLOOR_CHANNELS)}
ANGLES = {'bodyYaw', 'hipYaw', 'hipRoll', 'hipPitch', 'chestYaw', 'chestRoll', 'chestPitch', 'headYaw', 'headRoll',
          'headPitch', 'footYawL', 'footYawR', 'footPitchL', 'footPitchR'}
UP = np.array([0.0, 1.0, 0.0])
# (channel, min, max) radians: well past what dancers do, well short of half a turn.
JOINT_LIMITS = [('hipYaw', -1.6, 1.6), ('hipRoll', -1.0, 1.0), ('hipPitch', -1.0, 1.2),
                ('chestYaw', -1.3, 1.3), ('chestRoll', -1.0, 1.0), ('chestPitch', -1.0, 1.4),
                ('headYaw', -1.5, 1.5), ('headRoll', -0.9, 0.9), ('headPitch', -1.2, 1.2)]
HEAD_NEUTRAL_PITCH = 0.0  # set by calibrate_head()
FLOOR_SMOOTH = 1.5     # floor work: frames (60 fps) the joints are smoothed over
FLOOR_LR_SMOOTH = 4.0  # and the hips' and shoulders' left-right lines


def nrm(v, eps=1e-9):
    return v / (np.linalg.norm(v, axis=-1, keepdims=True) + eps)


def orth(v, axis):
    """v without its component along unit `axis` (row-wise)."""
    return v - axis * (v * axis).sum(-1, keepdims=True)


def frames_from(x, yhint):
    """Rotation matrices (N, 3, 3), columns x, y, z (right-handed, z = x cross y) from an x axis and a y hint."""
    x = nrm(x)
    y = nrm(orth(yhint, x))
    z = np.cross(x, y)
    return np.stack([x, y, z], axis=-1)


def wrap(a):
    return (a + np.pi) % (2 * np.pi) - np.pi


def euler_yzx(R):
    """R = Ry(yaw) Rz(roll) Rx(pitch) -> (yaw, roll, pitch). Of the two solutions ((y, r, p) and (y + pi, pi - r,
    p + pi)) the one with the smaller yaw and pitch, as a joint (a head on a chest, a chest on hips) turns: so the
    angles don't flip by half a turn where the roll passes 90 degrees."""
    roll = np.arcsin(np.clip(R[..., 1, 0], -1, 1))
    yaw = np.arctan2(-R[..., 2, 0], R[..., 0, 0])
    pitch = np.arctan2(-R[..., 1, 2], R[..., 1, 1])
    y2, r2, p2 = wrap(yaw + np.pi), wrap(np.pi - roll), wrap(pitch + np.pi)
    alt = np.maximum(np.abs(y2), np.abs(p2)) < np.maximum(np.abs(yaw), np.abs(pitch))
    return np.where(alt, y2, yaw), np.where(alt, r2, roll), np.where(alt, p2, pitch)


def rot_y(a):
    c, s = np.cos(a), np.sin(a)
    o, z = np.ones_like(a), np.zeros_like(a)
    return np.stack([np.stack([c, z, s], -1), np.stack([z, o, z], -1), np.stack([-s, z, c], -1)], -2)


def apply(R, v):
    """R^T v: a world vector in the frame R (row-wise)."""
    return np.einsum('nji,nj->ni', R, v)


def across(x, half, reach):
    """Across coordinate: inside +-half (shoulders / stance) scaled to +-1, beyond it 1 + distance / reach."""
    a = np.abs(x)
    return np.sign(x) * np.where(a <= half, a / half, 1 + (a - half) / reach)


def bend_hint(root, mid, end, R, bone):
    """Direction the middle joint bends (in frame R), length = how clear the bend is (0 straight .. 1)."""
    d = nrm(end - root)
    e = mid - root
    perp = orth(e, d)
    amount = np.linalg.norm(perp, axis=-1) / bone
    w = np.clip((amount - 0.08) / 0.25, 0, 1)
    w = w * w * (3 - 2 * w)
    return apply(R, nrm(perp)) * w[:, None]


class Body:
    """Per-clip measurements of the source dancer (cm)."""

    def __init__(self, k):
        med = lambda a: float(np.median(a))
        L = np.linalg.norm
        self.thigh = med(L(k[:, LHIP] - k[:, LKNE], axis=1) + L(k[:, RHIP] - k[:, RKNE], axis=1)) / 2
        self.shin = med(L(k[:, LKNE] - k[:, LANK], axis=1) + L(k[:, RKNE] - k[:, RANK], axis=1)) / 2
        self.upper = med(L(k[:, LSHO] - k[:, LELB], axis=1) + L(k[:, RSHO] - k[:, RELB], axis=1)) / 2
        self.fore = med(L(k[:, LELB] - k[:, LWRI], axis=1) + L(k[:, RELB] - k[:, RWRI], axis=1)) / 2
        self.leg, self.arm = self.thigh + self.shin, self.upper + self.fore
        self.shoHalf = med(L(k[:, LSHO] - k[:, RSHO], axis=1)) / 2
        self.hipHalf = med(L(k[:, LHIP] - k[:, RHIP], axis=1)) / 2
        lowAnkle = np.minimum(k[:, LANK, 1], k[:, RANK, 1])
        self.ankleRest = float(np.percentile(lowAnkle, 5))  # ankle height with the sole on the floor
        self.hipRest = self.ankleRest + 0.965 * self.leg     # as DancerRig's makeSkeleton
        # Tracking quality: how much the bone lengths wobble (cm), relative to the bones.
        bones = [(LHIP, LKNE), (RHIP, RKNE), (LKNE, LANK), (RKNE, RANK), (LSHO, LELB), (RSHO, RELB), (LELB, LWRI),
                 (RELB, RWRI)]
        self.wobble = float(np.mean([np.std(L(k[:, a] - k[:, b], axis=1)) / np.median(L(k[:, a] - k[:, b], axis=1))
                                     for a, b in bones]))


def quat_from(R):
    """Rotation matrices (N, 3, 3) -> unit quaternions (N, 4) x y z w, signs kept continuous from frame to frame."""
    from scipy.spatial.transform import Rotation
    q = Rotation.from_matrix(R).as_quat()
    for i in range(1, len(q)):
        if np.dot(q[i], q[i - 1]) < 0: q[i] = -q[i]
    return q


def retarget(k, beat_frames, floor=False):
    """Keypoints (N, 17, 3) cm -> channels (N, C), plus the Body. `beat_frames`: frames per beat (smoothing).
    `floor`: floor work (FLOOR_CHANNELS): the pelvis follows the spine whichever way it points and its turn is a
    quaternion; the body's heading is where the pelvis faces or, lying or bent over, where the head is."""
    if floor:
        # The tracking jitters on the floor (the hips' width wobbles by half when upside down): the joints smoothed
        # over a few frames (about a 64th note), the hips' and shoulders' left-right lines over more below.
        k = gaussian_filter1d(k, FLOOR_SMOOTH, axis=0, mode='nearest')
    b = Body(k)
    n = len(k)
    out = np.zeros((n, len(FLOOR_CHANNELS if floor else CHANNELS)))
    hipMid = (k[:, LHIP] + k[:, RHIP]) / 2
    shoMid = (k[:, LSHO] + k[:, RSHO]) / 2
    spine = nrm(shoMid - hipMid)
    # Pelvis: across the hip joints; leans with the spine a little (its tilt forward isn't measured). Floor work: with
    # the spine (a body upside down or lying has no "up" to lean from).
    if floor:
        lr = lambda a, c: gaussian_filter1d(nrm(k[:, a] - k[:, c]), FLOOR_LR_SMOOTH, axis=0, mode='nearest')
        Rp = frames_from(lr(LHIP, RHIP), spine)
        Rc = frames_from(lr(LSHO, RSHO), spine)
    else:
        Rp = frames_from(k[:, LHIP] - k[:, RHIP], nrm(0.65 * UP + 0.35 * spine))
        Rc = frames_from(k[:, LSHO] - k[:, RSHO], spine)
    earMid = (k[:, LEAR] + k[:, REAR]) / 2
    ex = nrm(k[:, LEAR] - k[:, REAR])
    fwd = orth(k[:, NOSE] - earMid, ex)
    Rh = frames_from(ex, np.cross(nrm(fwd), ex))  # y = z cross x
    # Body: the pelvis's heading and ground position, smoothed over about a beat (twists stay in the hips).
    pf = Rp[:, :, 2].copy()
    if floor:
        # Facing, plus the spine's lie: either is horizontal when the other isn't (lying on the back the pelvis faces
        # up and the spine points along the floor; upside down the spine points down). Its angle smoothed (unwrapped),
        # weighted by the vector's length, so a moment where both are short doesn't turn the body. (Smoothed as a
        # vector it shrank in a fast turn and its angle whipped round: up to 3.8 turns/s where the dancer turned 1.4.
        # Spin glitches 2026-11-27.)
        v = pf + spine
        a = np.unwrap(np.arctan2(v[:, 0], v[:, 2]))
        w = np.hypot(v[:, 0], v[:, 2])
        sw = gaussian_filter1d(w, 0.5 * beat_frames, mode='nearest')
        yawB = gaussian_filter1d(a * w, 0.5 * beat_frames, mode='nearest') / np.maximum(sw, 1e-9)
    else:
        yawP = np.unwrap(np.arctan2(pf[:, 0], pf[:, 2]))
        yawB = gaussian_filter1d(yawP, 0.5 * beat_frames, mode='nearest')
    posB = gaussian_filter1d(hipMid[:, [0, 2]], 0.5 * beat_frames, axis=0, mode='nearest')
    Rb = rot_y(yawB)
    bodyO = np.stack([posB[:, 0], np.full(n, b.ankleRest), posB[:, 1]], -1)  # ankle-rest height: foot y 0 on floor
    out[:, CH['bodyYaw']] = yawB
    out[:, CH['bodyX']] = posB[:, 0] / b.leg  # world x / z for now: made relative to the loop by loop_channels
    out[:, CH['bodyZ']] = posB[:, 1] / b.leg
    # Pelvis in the body frame.
    hp = apply(Rb, hipMid - bodyO)
    out[:, CH['hipX']] = hp[:, 0] / b.leg
    out[:, CH['hipY']] = (hipMid[:, 1] - b.hipRest) / b.leg
    out[:, CH['hipZ']] = hp[:, 2] / b.leg
    if floor:
        out[:, CH['hipQx']:CH['hipQw'] + 1] = quat_from(np.einsum('nji,njk->nik', Rb, Rp))
    else:
        y, r, p = euler_yzx(np.einsum('nji,njk->nik', Rb, Rp))
        out[:, CH['hipYaw']], out[:, CH['hipRoll']], out[:, CH['hipPitch']] = y, r, p
    y, r, p = euler_yzx(np.einsum('nji,njk->nik', Rp, Rc))
    out[:, CH['chestYaw']], out[:, CH['chestRoll']], out[:, CH['chestPitch']] = y, r, p
    y, r, p = euler_yzx(np.einsum('nji,njk->nik', Rc, Rh))
    out[:, CH['headYaw']], out[:, CH['headRoll']] = y, r
    out[:, CH['headPitch']] = p - HEAD_NEUTRAL_PITCH
    # Hands: wrists in the chest frame (origin between the shoulders).
    for side, (sho, elb, wri) in (('L', (LSHO, LELB, LWRI)), ('R', (RSHO, RELB, RWRI))):
        sx = 1 if side == 'L' else -1
        v = apply(Rc, k[:, wri] - shoMid)
        # The tracking sometimes stretches an arm past its length: a straight arm at most a little long.
        rel = v - np.array([sx * b.shoHalf, 0, 0])
        ln = np.linalg.norm(rel, axis=1, keepdims=True)
        v = np.array([sx * b.shoHalf, 0, 0]) + rel * np.minimum(1, 1.1 * b.arm / np.maximum(ln, 1e-9))
        out[:, CH['hand' + side + 'u']] = (v[:, 0] - sx * b.shoHalf) / b.arm  # from its own shoulder
        out[:, CH['hand' + side + 'v']] = v[:, 1] / b.arm
        out[:, CH['hand' + side + 'w']] = v[:, 2] / b.arm
        h = bend_hint(k[:, sho], k[:, elb], k[:, wri], Rc, b.upper)
        out[:, CH['elbow' + side + 'x']:CH['elbow' + side + 'x'] + 3] = h
    # Feet: ankles in the body frame.
    for side, (hip, kne, ank) in (('L', (LHIP, LKNE, LANK)), ('R', (RHIP, RKNE, RANK))):
        sx = 1 if side == 'L' else -1
        a = apply(Rb, k[:, ank] - bodyO)
        h = np.maximum(0, k[:, ank, 1] - b.ankleRest)
        out[:, CH['foot' + side + 'u']] = across(a[:, 0], b.hipHalf, b.leg)
        out[:, CH['foot' + side + 'y']] = h / b.leg
        out[:, CH['foot' + side + 'z']] = a[:, 2] / b.leg
        kh = bend_hint(k[:, hip], k[:, kne], k[:, ank], Rb, b.thigh)
        out[:, CH['knee' + side + 'x']:CH['knee' + side + 'x'] + 3] = kh
        # Toes follow the knee: its direction on the floor (body frame), while the knee is clearly bent.
        w = np.linalg.norm(kh, axis=1)
        toe = np.arctan2(sx * kh[:, 0], np.maximum(kh[:, 2], 1e-3))
        out[:, CH['footYaw' + side]] = np.clip(toe, -0.6, 0.6) * w
        # Heel lifts: the ankle a little up while the foot hardly moves across the floor (toes down).
        sp = np.linalg.norm(np.gradient(k[:, ank, [0, 2]], axis=0), axis=1) * FPS  # cm/s
        still = np.clip(1 - sp / 40, 0, 1)
        lift = np.clip(h / 7.0, 0, 1) * np.clip((12 - h) / 6, 0, 1)
        out[:, CH['footPitch' + side]] = -0.6 * lift * still
    # Bend hints smoothed over a few frames: where a limb passes through straight the direction is noise, and
    # averaging the vectors shrinks it there instead of letting it flip.
    for n in CHANNELS:
        if n.startswith(('elbow', 'knee')):
            out[:, CH[n]] = gaussian_filter1d(out[:, CH[n]], 3, mode='nearest')
    # The body's heading without wraps (spins add up). Joint angles within what a body can do: beyond that it is the
    # tracking flipping (a head read as turned backwards); clamped, such a flip shows as a jump, which build_moves
    # bridges. Never near +-pi, so no wrap-around inside a move.
    out[:, CH['bodyYaw']] = np.unwrap(out[:, CH['bodyYaw']])
    if floor:  # the face tracking flips upside down, the toes guess from the knee: smoothed (a 16th of a beat)
        for n in ('headYaw', 'headRoll', 'headPitch', 'footYawL', 'footYawR'):
            out[:, CH[n]] = gaussian_filter1d(np.clip(out[:, CH[n]], -1.5, 1.5), beat_frames / 8, mode='nearest')
    for n, lo, hi in JOINT_LIMITS:
        out[:, CH[n]] = np.clip(out[:, CH[n]], lo, hi)
    if floor:
        out[:, CH['clear']] = clearance(k, b, Rh)
        for n, h in zip(CONTACT_CHANNELS, contacts(k, b)):
            out[:, CH[n]] = np.clip(h, 0, CONTACT_MAX)
        for n, w in zip(PIN_CHANNELS, pins(k, b, beat_frames)):
            out[:, CH[n]] = w
    return out, b


# Floor contact: how far a part's center is above the floor when it touches (cm, about a person's), and the floor
# under the source: the ankle's center sits about 0.09 leg above it.
CONTACT = [(LWRI, 4), (RWRI, 4), (LELB, 4), (RELB, 4), (LKNE, 5), (RKNE, 5), (LSHO, 7), (RSHO, 7)]


def clearance(k, b, Rh):
    """Per frame, how far the upper body's lowest point is above the floor (/ leg): hands, elbows, knees,
    shoulders, the pelvis and the top of the head (above the ears along the head's up)."""
    floor = b.ankleRest - 0.09 * b.leg
    hs = [k[:, j, 1] - r for j, r in CONTACT]
    hs.append((k[:, LHIP, 1] + k[:, RHIP, 1]) / 2 - 10)
    hs.append((k[:, LEAR, 1] + k[:, REAR, 1]) / 2 + 0.13 * b.leg * Rh[:, 1, 1])
    return np.maximum(0, (np.min(hs, axis=0) - floor) / b.leg)


def contacts(k, b):
    """Per frame, the wrists', knees' and the head's own clearance above the floor (/ leg, as `clearance`: the
    part's contact surface; the head a ball of 0.12 leg round the ears' middle)."""
    floor = b.ankleRest - 0.09 * b.leg
    out = [(k[:, j, 1] - r - floor) / b.leg for j, r in CONTACT[:2] + CONTACT[4:6]]
    out.append(((k[:, LEAR, 1] + k[:, REAR, 1]) / 2 - 0.12 * b.leg - floor) / b.leg)
    return out


# Planted (pins): down within PIN_NEAR leg and moving under PIN_SLOW leg lengths a beat across the floor to plant;
# stays planted while within PIN_NEAR_OUT and under PIN_SLOW_OUT (hysteresis); a plant shorter than PIN_MIN beats is
# none. The weight eases in and out over PIN_EASE beats inside the plant (the contact has landed when it is held).
PIN_NEAR, PIN_NEAR_OUT = 0.03, 0.06
PIN_SLOW, PIN_SLOW_OUT = 0.12, 0.3
PIN_MIN, PIN_EASE = 1 / 8, 1 / 16


def pins(k, b, beat_frames):
    """Per frame, 0 .. 1: how planted each hand, foot, knee and the head is (PIN_CHANNELS order): near the floor and
    hardly moving across it, with hysteresis; eased in and out inside each plant."""
    floor = b.ankleRest - 0.09 * b.leg
    hands = [(k[:, j, 1] - r - floor) / b.leg for j, r in CONTACT[:2]]
    feet = [(k[:, j, 1] - b.ankleRest) / b.leg for j in (LANK, RANK)]
    knees = [(k[:, j, 1] - r - floor) / b.leg for j, r in CONTACT[4:6]]
    earMid = (k[:, LEAR] + k[:, REAR]) / 2
    head = (earMid[:, 1] - 0.12 * b.leg - floor) / b.leg
    pts = [k[:, LWRI], k[:, RWRI], k[:, LANK], k[:, RANK], k[:, LKNE], k[:, RKNE], earMid]
    out = []
    for h, p in zip(hands + feet + knees + [head], pts):
        xz = gaussian_filter1d(p[:, [0, 2]], 2, axis=0, mode='nearest')
        speed = np.linalg.norm(np.gradient(xz, axis=0), axis=1) * beat_frames / b.leg  # leg lengths a beat
        on = np.zeros(len(h), bool)
        state = False
        for i in range(len(h)):
            state = (h[i] < PIN_NEAR_OUT and speed[i] < PIN_SLOW_OUT) if state else (h[i] < PIN_NEAR and speed[i] < PIN_SLOW)
            on[i] = state
        w = np.zeros(len(h))
        i = 0
        while i < len(h):
            if not on[i]:
                i += 1
                continue
            j = i
            while j < len(h) and on[j]: j += 1
            n = j - i
            if n >= PIN_MIN * beat_frames:
                d = np.minimum(np.arange(n) + 0.5, n - np.arange(n) - 0.5) / max(1.0, PIN_EASE * beat_frames)
                x = np.clip(d, 0, 1)
                w[i:j] = x * x * (3 - 2 * x)
            i = j
        out.append(w)
    return out


def calibrate_head(sample_clips):
    """Neutral head pitch: the median over a set of clips (the nose sits below the ears' line)."""
    global HEAD_NEUTRAL_PITCH
    vals = []
    for k in sample_clips:
        ex = nrm(k[:, LEAR] - k[:, REAR])
        shoMid = (k[:, LSHO] + k[:, RSHO]) / 2
        hipMid = (k[:, LHIP] + k[:, RHIP]) / 2
        Rc = frames_from(k[:, LSHO] - k[:, RSHO], nrm(shoMid - hipMid))
        fwd = orth(k[:, NOSE] - (k[:, LEAR] + k[:, REAR]) / 2, ex)
        Rh = frames_from(ex, np.cross(nrm(fwd), ex))
        vals.append(euler_yzx(np.einsum('nji,njk->nik', Rc, Rh))[2])
    HEAD_NEUTRAL_PITCH = float(np.median(np.concatenate(vals)))
    return HEAD_NEUTRAL_PITCH


def check_handedness(k):
    """Mean of (nose - ears) . (left x up): > 0 when the coordinates are right-handed as assumed."""
    left = nrm(k[:, LHIP] - k[:, RHIP])
    fwd = np.cross(left, UP)
    return float(np.mean(((k[:, NOSE] - (k[:, LEAR] + k[:, REAR]) / 2) * fwd).sum(1)))
