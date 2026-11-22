"""Shared pieces of the Phase 5B.4 corpus sweep (stdlib only).

Corpus inventory, static classification (via `reforce --describe-config`), de-randomization of load-time rnd,
and the sweep timeline format (probes/timelines/sweep_*.py).

Classes (see docs/sweep.md):
  det       deterministic as shipped (no rnd at all; FlowFields: no t either). Compared exactly.
  derand    rnd() only in load-tier keys (A vars, ...): run as a DERIVED copy "zz sweep <name>" whose load-time
            rnd(x) calls are replaced by fixed fractions K*x (K per call, from a hash), on BOTH sides. That copy is
            deterministic, so it is compared exactly; it exercises everything except the random draw itself.
  noise     rnd() per row/pixel (C/D vars, src keys) or per step/frame: the field/palette is random by design.
            Derived copies are used when the load tier was random too. Compared loosely (structure only).
  rnd-load  load-tier randomness that could not be removed (noise()/choice() in A vars). Compared loosely.
  time      uses t (ColorMaps: B/C vars change with time). A ColorMap's t counts from its own start (measured
            by this sweep), so both sides are at t = 0.7 s at the shot; still judged loosely (+-2 frame shot jitter).
  png       PNG ColorMap strip (deterministic).
"""
import ast
import hashlib
import json
import os
import re
import subprocess

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SCENES = os.path.join(REPO, "scenes")
WORK = os.path.join(REPO, "build", "sweep")
DERIVED = os.path.join(WORK, "derived")  # build/sweep/derived/Packages/G-Force.<Type>.package/zz sweep/*.txt
DERIVED_PREFIX = "zz sweep "
DERIVED_SUBDIR = "zz sweep"  # also the folder under probes/<Type>/ that run_gforce_sweeps.sh installs
PKG = {"FlowField": "G-Force.FlowField.package", "ColorMap": "G-Force.ColorMap.package"}
DEFAULT_REFORCE = os.path.join(REPO, "build", "reforce.app", "Contents", "MacOS", "reforce")

# Probe configs used by the sweeps (probes/<Type>/<name>.txt, our own).
PROBE = {
    "gray": ("ColorMap", "zz reforce gray"),
    "void": ("FlowField", "zz reforce void"),
    "grid": ("WaveShape", "zz reforce sweep grid"),
    "ramp": ("WaveShape", "zz reforce sweep ramp"),
}


def probe_path(key):
    t, name = PROBE[key]
    return os.path.join(REPO, "probes", t, name + ".txt")


# ---------------------------------------------------------------------------------------------- inventory
def corpus(kind, scenes=SCENES):
    """-> sorted [(name, path)] for kind 'FlowField' or 'ColorMap' (txt + png, no -icon.png, no _*)."""
    root = os.path.join(scenes, "Packages", PKG[kind])
    out = []
    for d, _, files in os.walk(root):
        for f in files:
            stem, ext = os.path.splitext(f)
            ext = ext.lower()
            if f.startswith("_") or f.startswith("."):
                continue
            if ext == ".txt" or (kind == "ColorMap" and ext == ".png" and not stem.lower().endswith("-icon")):
                out.append((stem, os.path.join(d, f)))
    out.sort(key=lambda e: (e[0].lower(), e[1]))
    return out


def duplicates(entries):
    seen = {}
    for name, path in entries:
        seen.setdefault(name.lower(), []).append(path)
    return {k: v for k, v in seen.items() if len(v) > 1}


# ------------------------------------------------------------------------------------------------ describe
def describe(reforce, paths):
    """reforce --describe-config, batched. -> {path: dict}."""
    out = {}
    for i in range(0, len(paths), 100):
        chunk = paths[i:i + 100]
        r = subprocess.run([reforce, "--describe-config"] + chunk, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           text=True)
        for line in r.stdout.splitlines():
            line = line.strip()
            if line.startswith("{"):
                d = json.loads(line)
                out[d["path"]] = d
    missing = [p for p in paths if p not in out]
    if missing:
        raise RuntimeError("describe-config gave no result for %d file(s), e.g. %s" % (len(missing), missing[0]))
    return out


def tier(d, group, flag):
    return bool(d.get("tiers", {}).get(group, {}).get(flag))


def load_random(d):
    return tier(d, "load", "random")


def later_random(d):
    return any(tier(d, g, "random") for g in ("frame", "step", "pixel"))


def uses_time(d):
    return bool(d["all"]["time"])


def classify(kind, d, derived_d=None):
    """Class of a config from its describe result (and that of its de-randomized copy, if any)."""
    if d is None:
        return "png"
    eff = derived_d if derived_d is not None else d
    if kind == "ColorMap" and uses_time(eff):
        return "time"
    if load_random(eff):
        return "rnd-load"
    if later_random(eff):
        return "noise"
    if kind == "FlowField" and uses_time(eff):
        return "time"  # t at build time (none in the corpus as of 2026-11)
    return "derand" if derived_d is not None else "det"


# ---------------------------------------------------------------------------------------- de-randomization
_KEY = re.compile(r'(?im)^[ \t]*A\d+[ \t]*=[ \t]*')
_RND = re.compile(r'(?i)(?<![A-Za-z0-9_])rnd[ \t]*\(')


def _value_span(text, start):
    """Value extent of a key starting at text[start] (after '='): a quoted string (may span lines) or the rest
    of the line."""
    if start < len(text) and text[start] == '"':
        end = text.find('"', start + 1)
        return (start + 1, end if end >= 0 else len(text))
    end = text.find("\n", start)
    return (start, end if end >= 0 else len(text))


def _fraction(name, n):
    h = hashlib.sha1(("%s#%d" % (name, n)).encode("utf-8")).digest()
    return 0.2 + 0.6 * (int.from_bytes(h[:4], "big") / 2.0 ** 32)  # in [.2, .8): away from range ends


def _replace_rnd(value, name, counter, salt=""):
    out, i = "", 0
    while True:
        m = _RND.search(value, i)
        if not m:
            return out + value[i:]
        # balanced closing paren of the call
        depth, j = 1, m.end()
        while j < len(value) and depth:
            depth += {"(": 1, ")": -1}.get(value[j], 0)
            j += 1
        if depth:
            return out + value[i:]  # unbalanced: leave the rest alone (describe will still flag it)
        inner = _replace_rnd(value[m.end():j - 1], name, counter, salt)
        counter[0] += 1
        out += value[i:m.start()] + "(%.4f*(%s))" % (_fraction(salt + name, counter[0]), inner)
        i = j


def derandomize(text, name, salt=""):
    """Replaces rnd(x) inside A-var values with (K*(x)). -> (new text, number of replacements). `salt` changes
    the K values (validation: a perturbed copy must fail the comparison)."""
    counter = [0]
    out, pos = "", 0
    for m in _KEY.finditer(text):
        if m.start() < pos:
            continue
        a, b = _value_span(text, m.end())
        out += text[pos:a] + _replace_rnd(text[a:b], name, counter, salt)
        pos = b
    out += text[pos:]
    return out, counter[0]


def derived_path(kind, name, root=None):
    return os.path.join(root or DERIVED, "Packages", PKG[kind], DERIVED_SUBDIR, DERIVED_PREFIX + name + ".txt")


def write_derived(kind, name, src, root=None, salt=""):
    raw = open(src, "rb").read().decode("latin-1")
    new, n = derandomize(raw, name, salt)
    if n == 0:
        return None
    p = derived_path(kind, name, root)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    hdr = ("// reforce sweep (local, never committed): derived from %s/%s; %d rnd(x) call(s) in A vars replaced\n"
           "// by fixed K*x so G-Force and reforce load the same parameters.\n"
           % (PKG[kind], os.path.relpath(src, os.path.join(SCENES, "Packages", PKG[kind])), n))
    with open(p, "wb") as f:
        f.write((hdr + new).encode("latin-1"))
    return p


# --------------------------------------------------------------------------------------------- timelines
TIMELINE_DIR = os.path.join(REPO, "probes", "timelines")
_HDR = re.compile(r"^# reforce-sweep:(.*)$", re.M)
_TIMED = re.compile(r"^@_time\( '([0-9:.]+)' \)\ndef (\w+)\(\): (\w+)\( (.*?) \)[ \t]*(?:# (\S+) (.*))?$", re.M)


def fmt_time(t):
    m, s = divmod(round(t * 100) / 100.0, 60)
    txt = ("%.2f" % s).rstrip("0").rstrip(".")
    if s < 10:
        txt = "0" + txt
    return "%d:%s" % (int(m), txt)


def parse_time(text):
    s = 0.0
    for p in text.split(":"):
        s = s * 60 + float(p)
    return s


def parse_sweep(path):
    """Parses a generated sweep timeline (the exact calls G-Force runs).
    -> (header dict, setup, slots). setup: {colormap|flowfield|waveshape: name} at 0:00. slots: dicts with
    index, name (as G-Force addresses it), cls, source (path relative to the repo), t_clear, t_switch, t_shot,
    shot (1-based screenshot number = G-Force's 'G-Force Screen Shot NNNN.png')."""
    text = open(path, encoding="utf-8").read()
    m = _HDR.search(text)
    if not m:
        raise ValueError("%s: no '# reforce-sweep:' header" % path)
    hdr = dict(kv.split("=", 1) for kv in m.group(1).split())
    setup, slots, cur, shots = {}, [], None, 0
    for t_txt, fname, fn, args, cls, src in _TIMED.findall(text):
        t = parse_time(t_txt)
        if fn == "setup":  # setup( colormap, flowfield, waveshape ) at 0:00
            setup = dict(zip(("colormap", "flowfield", "waveshape"), ast.literal_eval("(%s)" % args)))
            continue
        arg = ast.literal_eval(args)
        if fname.startswith("c"):  # clear (void) before a FlowField
            cur = {"t_clear": t}
        elif fname.startswith("x"):  # switch to the config under test
            cur = dict(cur or {}, index=int(fname[1:]), name=arg, fn=fn, cls=cls, source=src.strip(), t_switch=t)
        elif fname.startswith("s"):
            shots += 1
            cur.update(t_shot=t, shot=shots, label=arg)
            slots.append(cur)
            cur = None
    if int(hdr.get("slots", len(slots))) != len(slots):
        raise ValueError("%s: header says %s slots, parsed %d" % (path, hdr.get("slots"), len(slots)))
    return hdr, setup, slots


def sweep_timelines(kind=None):
    names = sorted(f[:-3] for f in os.listdir(TIMELINE_DIR) if f.startswith("sweep_") and f.endswith(".py"))
    if kind:
        names = [n for n in names if n.startswith("sweep_%s_" % kind)]
    return names
