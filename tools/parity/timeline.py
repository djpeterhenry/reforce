"""Parse the G-Force probe timelines (probes/timelines/runN.py, Python 2.7 scripts that only G-Force runs)
into plain events, and translate them into a reforce offline run.

Only the small vocabulary the timelines use is understood:
  ws(name[, morph])  cm(name[, morph])  ff(name[, morph]) or ff(name[, union[, morph]])  shot(label)
  vis.StartParticle(name, seconds)
Everything else (SetSlideshowEnabled, KillRunning*, SetConsoleVisible, print) is setup that the reforce
command line covers (--slideshow off, nothing running at start).
"""
import re

_TIME = re.compile(r"@_time\(\s*'([0-9:.]+)'\s*\)\s*\ndef\s+\w+\s*\(\s*\)\s*:(.*?)(?=\n@_time|\Z)", re.S)
_CALL = re.compile(r"\b(ws|cm|ff|shot|vis\.StartParticle)\s*\(")
LAYER = {"ws": "waveshape", "cm": "colormap", "ff": "flowfield"}


def _seconds(text):
    parts = [float(p) for p in text.split(":")]
    s = 0.0
    for p in parts:
        s = s * 60 + p
    return s


def _args(text, start):
    """Parses a call's argument list beginning at text[start] (just after '('). Returns (args, end)."""
    args, i, cur = [], start, ""
    depth = 0
    while i < len(text):
        c = text[i]
        if c in "'\"":
            j = text.index(c, i + 1)
            args.append(text[i + 1:j])
            cur = None
            i = j + 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            if depth == 0:
                if cur is not None and cur.strip():
                    args.append(_num(cur.strip()))
                return args, i + 1
            depth -= 1
        elif c == "," and depth == 0:
            if cur is not None and cur.strip():
                args.append(_num(cur.strip()))
            cur = ""
            i += 1
            continue
        if cur is not None:
            cur += c
        i += 1
    raise ValueError("unterminated call")


def _num(s):
    try:
        return float(s)
    except ValueError:
        return s


class Event:
    def __init__(self, t, kind, layer=None, name=None, morph=0.0, union="", label=None):
        self.t, self.kind, self.layer, self.name = t, kind, layer, name
        self.morph, self.union, self.label = morph, union, label

    def __repr__(self):
        return "Event(%.2f %s %s %s %s %s)" % (self.t, self.kind, self.layer, self.name, self.morph, self.union)


def parse(path):
    text = open(path, encoding="utf-8").read()
    ff_has_union = re.search(r"def\s+ff\s*\(\s*name\s*,\s*union", text) is not None
    events = []
    for m in _TIME.finditer(text):
        t = _seconds(m.group(1))
        body = m.group(2)
        # Drop the helper definitions' own bodies (they come before the first @_time) and comments.
        body = "\n".join(line.split("#", 1)[0] for line in body.split("\n"))
        pos = 0
        while True:
            c = _CALL.search(body, pos)
            if not c:
                break
            args, pos = _args(body, c.end())
            fn = c.group(1)
            if fn == "shot":
                events.append(Event(t, "shot", label=args[0]))
            elif fn == "vis.StartParticle":
                events.append(Event(t, "particle", "particle", args[0], morph=float(args[1]) if len(args) > 1 else 0))
            elif fn == "ff":
                union, morph = "", 0.0
                if ff_has_union:
                    union = args[1] if len(args) > 1 else ""
                    morph = float(args[2]) if len(args) > 2 else 0.0
                else:
                    morph = float(args[1]) if len(args) > 1 else 0.0
                events.append(Event(t, "switch", "flowfield", args[0], morph, union))
            else:
                events.append(Event(t, "switch", LAYER[fn], args[0], float(args[1]) if len(args) > 1 else 0.0))
    return events


def reforce_args(events, fps):
    """-> (start {layer: name}, [--command values], {shot index (1-based): frame}, frames to render)."""
    start, commands, shots = {}, [], {}
    for e in events:
        if e.kind == "switch" and e.t == 0:
            start[e.layer] = e.name  # the 0:00 setup: starting configs (cuts)
        elif e.kind == "switch":
            if e.union:
                commands.append("%g=switch:union:%s" % (e.t, e.union))
            commands.append("%g=morph:%s:%g:%s" % (e.t, e.layer, e.morph, e.name))
        elif e.kind == "particle":
            commands.append("%g=start:particle:%s" % (e.t, e.name))
        elif e.kind == "shot":
            shots[len(shots) + 1] = int(round(e.t * fps))
    frames = max(shots.values()) + 1 if shots else 1
    return start, commands, shots, frames
