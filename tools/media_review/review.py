#!/usr/bin/env python3
"""Movie review contact sheet for picking reforce movies.

    python3 tools/media_review/review.py [FOLDER] [--port N] [--no-open]   # default $REFORCE_MOVIE_SOURCE
    python3 tools/media_review/review.py collect [FOLDER]           # dry run: what --copy/--prune would do
    python3 tools/media_review/review.py collect --copy [FOLDER]    # copy new Keep movies into ~/reforce/movies
    python3 tools/media_review/review.py collect --prune [FOLDER]   # delete copies whose original is no longer Keep
    python3 tools/media_review/review.py collect --move [FOLDER]    # move instead of copy (old behavior)

Copies are APFS clones when possible (instant, no extra space until a side changes). The Finder tags in FOLDER
are the switch: tag Keep + --copy adds, re-tag Maybe/Drop + --prune removes (or just delete the copy by hand,
and re-tag the original so the next --copy does not bring it back). --prune only deletes files whose name
matches a movie in FOLDER; anything else in ~/reforce/movies is never touched.

Start / End (fields, or "here" buttons / keys [ ] from the preview's position) write "Start = s" / "End = s".
Black / White / Gamma (small fields) write the movie sprite's luminance levels (PLAN 5K; Black 0..1, White 0..1 above
Black, Gamma > 0, below 1 brightens; empty clears). They go to the original's .reforce.txt and to its copy in
~/reforce/movies, like the beats.
Loop (button or key l) writes "Loop = 1" into the movie's "<stem>.reforce.txt" (reforce's per-movie options,
engine/MediaOptions.hpp; a plain "<stem>.txt" would be G-Force sprite params). --copy carries these files along
and keeps them in sync for movies already copied.

Beats (button or key b) opens a beat panel for dance clips (PLAN 5J.2): the preview flashes and clicks on the beat
grid (red = the "1"), over a strip of the dancers' motion. BPM field, x2 / /2, nudge, "This beat is 1", tap tempo
(key t), Analyze (tools/dance_sync/dance_beats.py; uses the interpolated "<stem>.rife4x.mov" in ~/reforce/movies when
there is one) and Save, which writes BPM / BeatStart / BeatsPerBar to the movie's .reforce.txt and to its copy in
~/reforce/movies, so reforce plays it beat-synced right away.

Serves http://127.0.0.1:8420 (this Mac only). One row per movie: a filmstrip of frames, duration,
size, an inline preview player, "Open in QuickTime", "Show in Finder", and Keep / Maybe / Drop
buttons that set real Finder tags (Green / Yellow / Red) on the file, so Finder shows the same
decisions. Keys: j/k or arrows move, 1 Keep, 2 Maybe, 3 Drop, 0 clear, space play/pause, o QuickTime.

The originals are never modified except for their Finder tags. Thumbnails and small H.264
previews (made on first play) are cached in ~/Library/Caches/reforce/review/.
"""
from __future__ import annotations
import contextlib, hashlib, html, io, json, os, plistlib, re, shutil, subprocess, sys, threading
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

MOVIE_EXT = {".mov", ".mp4", ".m4v", ".mpg", ".mpeg", ".avi", ".qt", ".3gp"}
CACHE = Path.home() / "Library/Caches/reforce/review"
DEST = Path.home() / "reforce/movies"
PORT = 8420
FRAMES = 8
TAGS = {"keep": ("Green", 2), "maybe": ("Yellow", 5), "drop": ("Red", 6)}
TAG_KEY = "com.apple.metadata:_kMDItemUserTags"


# ---------- Finder tags (xattr com.apple.metadata:_kMDItemUserTags, a binary plist of "Name\nColor") ----------
def read_tags(p: Path) -> list[str]:
    r = subprocess.run(["xattr", "-px", TAG_KEY, str(p)], capture_output=True, text=True)
    if r.returncode != 0:
        return []
    try:
        return list(plistlib.loads(bytes.fromhex(r.stdout.replace("\n", "").replace(" ", ""))))
    except Exception:
        return []


def write_tags(p: Path, tags: list[str]) -> None:
    if not tags:
        subprocess.run(["xattr", "-d", TAG_KEY, str(p)], capture_output=True)
        return
    data = plistlib.dumps(tags, fmt=plistlib.FMT_BINARY).hex()
    subprocess.run(["xattr", "-wx", TAG_KEY, data, str(p)], check=True, capture_output=True)


def verdict(tags: list[str]) -> str:
    names = {t.split("\n")[0] for t in tags}
    for v, (name, _) in TAGS.items():
        if name in names:
            return v
    return ""


def set_verdict(p: Path, v: str) -> str:
    ours = {name for name, _ in TAGS.values()}
    kept = [t for t in read_tags(p) if t.split("\n")[0] not in ours]  # keep any other tags Peter set
    if v in TAGS:
        name, color = TAGS[v]
        kept.append(f"{name}\n{color}")
    write_tags(p, kept)
    return verdict(read_tags(p))


# ---------- media ----------
# ---------- per-movie reforce options: "<stem>.reforce.txt" (engine/MediaOptions.hpp) ----------
def options_path(p: Path) -> Path:
    return p.with_name(p.stem + ".reforce.txt")


OPTION_NOTES = {"loop": "play on repeat (reforce)", "start": "seconds into the movie", "end": "stop / loop back here",
                "bpm": "dancers' tempo (tools/dance_sync)", "beatstart": "seconds of a downbeat",
                "beatsperbar": "beats per bar", "black": "sprite levels: luminance shown as black",
                "white": "sprite levels: luminance shown as white", "gamma": "sprite levels: < 1 brightens"}
OPTION_NAMES = {"bpm": "BPM", "beatstart": "BeatStart", "beatsperbar": "BeatsPerBar", "black": "Black",
                "white": "White", "gamma": "Gamma"}
LEVEL_KEYS = ("black", "white", "gamma")


def _opt_key(line: str) -> str | None:
    code = line.split("//", 1)[0]
    return code.split("=", 1)[0].strip().lower() if "=" in code else None


def read_options(p: Path) -> dict:
    f = options_path(p)
    out = {}
    if f.exists():
        for line in f.read_text(errors="replace").splitlines():
            k = _opt_key(line)
            if k:
                out[k] = line.split("//", 1)[0].split("=", 1)[1].strip().strip('"')
    return out


def read_loop(p: Path) -> bool:
    return read_options(p).get("loop", "").lower() in ("1", "on", "yes", "true")


def parse_time(v: str) -> float | None:
    """Seconds, or m:ss(.s) / h:mm:ss."""
    v = v.strip()
    if not v:
        return None
    try:
        total = 0.0
        for part in v.split(":"):
            x = float(part)
            if x < 0:
                return None
            total = total * 60 + x
        return total
    except ValueError:
        return None


def set_option(p: Path, key: str, value: str | None) -> dict:
    """Sets (or with None removes) one option, keeping the others; deletes the file when nothing is left."""
    f = options_path(p)
    lines = f.read_text(errors="replace").splitlines() if f.exists() else []
    rest = [l for l in lines if _opt_key(l) != key.lower()]
    if value is not None:
        name = OPTION_NAMES.get(key.lower(), key[:1].upper() + key[1:].lower())
        rest.append(f"{name} = {value}  // {OPTION_NOTES.get(key.lower(), 'reforce')}")
        order = ["loop", "start", "end"]
        rest.sort(key=lambda l: order.index(_opt_key(l)) if _opt_key(l) in order else len(order))
    if any(l.split("//", 1)[0].strip() for l in rest):
        f.write_text("\n".join(rest) + "\n")
    elif f.exists():
        f.unlink()
    return read_options(p)


def set_loop(p: Path, on: bool) -> bool:
    set_option(p, "loop", "1" if on else None)
    return read_loop(p)


def set_time(p: Path, key: str, text: str) -> tuple[dict, str]:
    """key start|end; text seconds or m:ss, "" clears. Returns (options, error)."""
    if not text.strip():
        return set_option(p, key, None), ""
    t = parse_time(text)
    if t is None:
        return read_options(p), f"not a time: {text!r} (use seconds or m:ss)"
    opts = read_options(p)
    other = parse_time(opts.get("end" if key == "start" else "start", "") or "")
    if key == "end" and t <= (other or 0):
        return opts, "End must be after Start"
    if key == "start" and other is not None and t >= other:
        return opts, "Start must be before End"
    return set_option(p, key, f"{t:.2f}".rstrip("0").rstrip(".")), ""


def level_options(p: Path) -> dict:
    o = read_options(p)
    out = {}
    for k in LEVEL_KEYS:
        try:
            out[k] = float(o[k]) if o.get(k) else None
        except ValueError:
            out[k] = None
    return out


def set_level(p: Path, key: str, text: str) -> tuple[dict, str]:
    """key black|white|gamma; "" clears. Written beside the original and its copy in ~/reforce/movies (like the
    beats), keeping every other key (Loop, Start/End, BPM/BeatStart/BeatsPerBar). Returns (levels, error)."""
    text = text.strip()
    value = None
    if text:
        try:
            value = float(text)
        except ValueError:
            return level_options(p), f"not a number: {text!r}"
        cur = level_options(p)
        black = value if key == "black" else (cur["black"] if cur["black"] is not None else 0.0)
        white = value if key == "white" else (cur["white"] if cur["white"] is not None else 1.0)
        if key == "gamma" and not 0 < value <= 10:
            return cur, "Gamma must be above 0 (up to 10; 1 = unchanged, below 1 brightens)"
        if key in ("black", "white") and not 0 <= value <= 1:
            return cur, f"{key.title()} must be 0..1"
        if white <= black:
            return cur, "White must be above Black"
    targets = [p]
    copy = DEST / p.name
    if copy.exists() and copy.resolve() != p.resolve():
        targets.append(copy)
    for t in targets:
        set_option(t, key, None if value is None else f"{value:.3f}".rstrip("0").rstrip("."))
    return level_options(p), ""


# ---------- beats (tools/dance_sync) ----------
DANCE_SYNC = Path(__file__).resolve().parents[1] / "dance_sync"
BEAT_CACHE = Path.home() / "Library/Caches/reforce/dance_sync"
BEAT_KEYS = ("bpm", "beatstart", "beatsperbar")


def interpolated_copy(p: Path) -> Path | None:
    q = DEST / f"{p.stem}.rife4x{p.suffix}"
    return q if q.exists() else None


def beat_options(p: Path) -> dict:
    o = read_options(p)
    try:
        return {"bpm": float(o["bpm"]) if o.get("bpm") else None,
                "beatStart": parse_time(o.get("beatstart", "")) if o.get("beatstart") else None,
                "beatsPerBar": int(float(o["beatsperbar"])) if o.get("beatsperbar") else 4}
    except ValueError:
        return {"bpm": None, "beatStart": None, "beatsPerBar": 4}


def beat_analysis(p: Path) -> dict | None:
    """The latest dance_beats.py result for this movie (its interpolated copy's first), trimmed for the page."""
    for stem in (f"{p.stem}.rife4x", p.stem):
        f = BEAT_CACHE / f"{stem.strip()}.beats.json"
        if f.exists():
            r = json.loads(f.read_text())
            keep = ("bpm", "beat_start", "beats_per_bar", "confidence", "candidates", "alternatives", "signal",
                    "envelope", "bounce", "hits", "envelope_rate", "envelope_start", "fps", "duration", "file")
            return {k: r.get(k) for k in keep}
    return None


def analyze_beats(p: Path) -> tuple[dict | None, str]:
    src = interpolated_copy(p) or p
    r = subprocess.run([str(DANCE_SYNC / "run"), "dance_beats.py", str(src)], capture_output=True, text=True)
    if r.returncode != 0:
        return None, (r.stderr or r.stdout).strip()[-400:] or "dance_beats.py failed"
    return beat_analysis(p), ""


def save_beats(p: Path, bpm: float | None, start: float | None, bpb: int) -> dict:
    """Writes (or with bpm None removes) the beat keys beside the original and its copy in ~/reforce/movies."""
    targets = [p]
    copy = DEST / p.name
    if copy.exists() and copy.resolve() != p.resolve():
        targets.append(copy)
    for t in targets:
        if bpm and bpm > 0:
            set_option(t, "bpm", f"{bpm:.3f}")
            set_option(t, "beatstart", f"{max(0.0, start or 0.0):.3f}")
            set_option(t, "beatsperbar", str(int(bpb)))
        else:
            for k in BEAT_KEYS:
                set_option(t, k, None)
    return beat_options(p)


def scan(folder: Path) -> list[dict]:
    items = []
    for p in sorted(folder.rglob("*"), key=lambda q: str(q.relative_to(folder)).strip().lower()):
        if p.is_file() and p.suffix.lower() in MOVIE_EXT and not p.name.startswith("._"):
            st = p.stat()
            key = hashlib.sha1(f"{p}|{st.st_size}|{st.st_mtime}".encode()).hexdigest()[:16]
            items.append({"id": len(items), "path": p, "rel": str(p.relative_to(folder)), "key": key,
                          "size": st.st_size, "sidecar": p.with_suffix(".txt").exists()})
    return items


def probe(it: dict) -> None:
    meta = CACHE / f"{it['key']}.json"
    if meta.exists():
        it.update(json.loads(meta.read_text()))
        return
    r = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                        "stream=width,height,codec_name,avg_frame_rate:format=duration", "-of", "json",
                        str(it["path"])], capture_output=True, text=True)
    info = {"dur": 0.0, "w": 0, "h": 0, "codec": "?", "fps": 0.0}
    try:
        j = json.loads(r.stdout)
        s = j["streams"][0]
        info.update(w=s.get("width", 0), h=s.get("height", 0), codec=s.get("codec_name", "?"),
                    dur=float(j["format"].get("duration", 0)))
        n, _, d = s.get("avg_frame_rate", "0/1").partition("/")
        info["fps"] = round(float(n) / float(d or 1), 2) if float(d or 1) else 0.0
    except Exception:
        info["codec"] = "unreadable"
    meta.write_text(json.dumps(info))
    it.update(info)


def make_strip(it: dict) -> None:
    out = CACHE / f"{it['key']}.jpg"
    if out.exists() or not it.get("dur"):
        return
    tmp = CACHE / f"{it['key']}.tmp"
    tmp.mkdir(exist_ok=True)
    frames = []
    for i in range(FRAMES):
        t = it["dur"] * (i + 0.5) / FRAMES
        f = tmp / f"{i}.jpg"
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-ss", f"{t:.2f}", "-i", str(it["path"]), "-frames:v", "1",
                        "-vf", "scale=200:112:force_original_aspect_ratio=decrease,pad=200:112:(ow-iw)/2:(oh-ih)/2",
                        str(f)], capture_output=True)
        if f.exists():
            frames.append(f)
    if frames:
        args = ["ffmpeg", "-v", "error", "-y"]
        for f in frames:
            args += ["-i", str(f)]
        args += ["-filter_complex", "".join(f"[{i}]" for i in range(len(frames))) + f"hstack=inputs={len(frames)}",
                 "-q:v", "4", str(out)] if len(frames) > 1 else ["-q:v", "4", str(out)]
        subprocess.run(args, capture_output=True)
    shutil.rmtree(tmp, ignore_errors=True)


PREVIEW_LOCKS: dict[str, threading.Lock] = {}


def preview(it: dict) -> Path | None:
    out = CACHE / f"{it['key']}.mp4"
    lock = PREVIEW_LOCKS.setdefault(it["key"], threading.Lock())
    with lock:
        if not out.exists():
            tmp = out.with_suffix(".part.mp4")
            enc = ["-c:v", "h264_videotoolbox", "-b:v", "1500k"]
            cmd = ["ffmpeg", "-v", "error", "-y", "-i", str(it["path"]), "-vf", "scale='min(960,iw)':-2",
                   "-pix_fmt", "yuv420p", *enc, "-c:a", "aac", "-b:a", "128k", "-movflags", "+faststart", str(tmp)]
            if subprocess.run(cmd, capture_output=True).returncode != 0:
                cmd[cmd.index("h264_videotoolbox")] = "libx264"
                subprocess.run(cmd, capture_output=True)
            if tmp.exists():
                tmp.rename(out)
    return out if out.exists() else None


# ---------- web ----------
PAGE = r"""<!doctype html><html><head><meta charset="utf-8"><title>Movie review</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{--bg:#141416;--fg:#e8e8ea;--mut:#9a9aa2;--row:#1d1d21;--sel:#2b2d36;--keep:#3fb950;--maybe:#d29922;--drop:#f85149}
body{margin:0;background:var(--bg);color:var(--fg);font:14px -apple-system,system-ui,sans-serif}
header{position:sticky;top:0;z-index:2;background:var(--bg);padding:10px 16px;border-bottom:1px solid #2a2a30;display:flex;gap:14px;align-items:center;flex-wrap:wrap}
header b{font-size:16px} .mut{color:var(--mut)} button{background:#2a2a31;color:var(--fg);border:1px solid #3a3a44;border-radius:6px;padding:4px 10px;cursor:pointer;font:inherit}
button:hover{background:#34343d} .f.on{outline:2px solid #6e7bff}
.row{display:grid;grid-template-columns:minmax(0,1fr);gap:6px;padding:10px 16px;border-bottom:1px solid #222;background:var(--row)}
.row.sel{background:var(--sel)} .top{display:flex;gap:10px;align-items:baseline;flex-wrap:wrap}
.name{font-weight:600;word-break:break-all} .strip{width:100%;max-width:1600px;aspect-ratio:1600/112;background:#000;border-radius:4px;cursor:pointer;object-fit:contain}
.btns{display:flex;gap:6px;flex-wrap:wrap;align-items:center} .lp.on{background:#6e7bff;color:#000}
.tm{width:6.5em;background:#101013;color:var(--fg);border:1px solid #3a3a44;border-radius:6px;padding:4px 6px;font:inherit}
.tm.lv{width:3.8em} .tm.set{border-color:#6e7bff} .err{color:var(--drop)} #q{width:16em;background:#101013;color:var(--fg);border:1px solid #3a3a44;border-radius:6px;padding:4px 8px;font:inherit} .k.on{background:var(--keep);color:#000} .m.on{background:var(--maybe);color:#000} .d.on{background:var(--drop);color:#000}
.row.v-keep{border-left:5px solid var(--keep)} .row.v-maybe{border-left:5px solid var(--maybe)} .row.v-drop{border-left:5px solid var(--drop);opacity:.55}
video{width:100%;max-width:960px;background:#000;border-radius:4px;outline-offset:-6px} .hide{display:none}
.beats{display:grid;gap:6px;padding:6px 0} .bcv{width:100%;max-width:960px;height:76px;background:#0b0b0e;border-radius:4px}
.bdots{display:inline-flex;gap:5px;margin-right:6px} .bdots i{width:12px;height:12px;border-radius:50%;background:#444;display:inline-block}
.bdots i.on{background:#5ac8fa} .bdots i.on.one{background:#ff453a} .chip{padding:2px 8px;border-radius:10px;font-size:12px}
.bt.on{background:#5ac8fa;color:#000} .bpm{width:5.5em}
</style></head><body>
<header><b>Movie review</b><span class="mut" id="folder"></span><span id="counts"></span>
<span>Show: <button class="f on" data-f="all">All</button> <button class="f" data-f="">Untagged</button>
<button class="f" data-f="keep">Keep</button> <button class="f" data-f="maybe">Maybe</button> <button class="f" data-f="drop">Drop</button></span>
<button id="collect" title="Copy Keep movies (and their Loop/Start/End options) into ~/reforce/movies = review.py collect --copy">Collect → ~/reforce/movies</button>
<input id="q" type="search" placeholder="Search names (/)" autocomplete="off">
<span class="mut">j/k move · 1 Keep · 2 Maybe · 3 Drop · 0 clear · l loop · [ ] start/end here · space play · o QuickTime</span></header>
<pre id="collectOut" class="hide" style="margin:0;padding:10px 16px;background:#0d0d10;border-bottom:1px solid #2a2a30;white-space:pre-wrap;max-height:40vh;overflow:auto"></pre>
<main id="list"></main>
<script>
let items=[],sel=0,filter='all';
const $=s=>document.querySelector(s);
const fmt=s=>{s=Math.round(s);return Math.floor(s/60)+':'+String(s%60).padStart(2,'0')};
const fmt2=s=>{const m=Math.floor(s/60),r=s-60*m;let t=r.toFixed(2).replace(/\.?0+$/,'');if(r<10)t='0'+t;return m+':'+t};
const gb=b=>b>=1e9?(b/1e9).toFixed(2)+' GB':(b/1e6).toFixed(0)+' MB';
async function load(){const r=await fetch('/api/list');const j=await r.json();items=j.items;$('#folder').textContent=j.folder+' · '+j.progress;render();
 if(j.pending)setTimeout(refresh,3000);
 const deep=new URLSearchParams(location.search).get('beats');if(deep!=null&&items[+deep])beats(+deep)}  // ?beats=ID opens its beat panel
async function refresh(){const r=await fetch('/api/list');const j=await r.json();$('#folder').textContent=j.folder+' · '+j.progress;
 for(const n of j.items){const o=items[n.id];if(!o.strip&&n.strip){o.strip=n.strip;o.dur=n.dur;o.w=n.w;o.h=n.h;o.codec=n.codec;const el=document.getElementById('r'+n.id);if(el)el.replaceWith(rowEl(o))}}
 if(j.pending)setTimeout(refresh,3000)}
let query='';
function visible(){const words=query.toLowerCase().split(/\s+/).filter(Boolean);
 return items.filter(i=>(filter==='all'||i.v===filter)&&words.every(w=>i.rel.toLowerCase().includes(w)))}
function counts(){const c={keep:0,maybe:0,drop:0,'':0};items.forEach(i=>c[i.v]++);const n=visible().length;
 $('#counts').textContent=`${items.length} movies · ${c.keep} keep · ${c.maybe} maybe · ${c.drop} drop · ${c['']} untagged${n<items.length?' · showing '+n:''}`}
function rowEl(i){const d=document.createElement('div');d.className='row v-'+(i.v||'none');d.id='r'+i.id;
 d.innerHTML=`<div class="top"><span class="name"></span><span class="mut">${i.dur?fmt(i.dur):'…'} · ${i.w||'?'}×${i.h||'?'} · ${i.codec||''} · ${gb(i.size)}${i.sidecar?' · has .txt':''}${i.loop?' · loops':''}${i.bpm?' · ♪ '+i.bpm.toFixed(1)+' BPM':''}${i.start!=null||i.end!=null?' · plays '+fmt2(i.start||0)+'–'+(i.end!=null?fmt2(i.end):'end'):''}</span></div>
 ${i.strip?`<img class="strip" loading="lazy" src="/thumb/${i.id}">`:'<div class="mut">making thumbnails…</div>'}
 <video class="hide" controls preload="none"></video>
 <div class="btns"><button class="play">▶ Play here</button><button class="qt">Open in QuickTime</button><button class="rev">Show in Finder</button>
 <span style="width:12px"></span><button class="k ${i.v==='keep'?'on':''}">Keep</button><button class="m ${i.v==='maybe'?'on':''}">Maybe</button><button class="d ${i.v==='drop'?'on':''}">Drop</button><button class="c">Clear</button>
 <span style="width:12px"></span><button class="bt" title="Beat grid: check, fix and save the dancers' tempo (key b)">♪ Beats</button>
 <button class="lp ${i.loop?'on':''}" title="Loop in reforce: writes Loop = 1 to the movie's .reforce.txt (key l)">⟳ Loop</button>
 <span style="width:12px"></span>Start <input class="tm st ${i.start!=null?'set':''}" placeholder="0:00" value="${i.start!=null?fmt2(i.start):''}"><button class="stn" title="Start = the preview's current position (key [)">⇤ here</button>
 End <input class="tm en ${i.end!=null?'set':''}" placeholder="${i.dur?fmt2(i.dur):'end'}" value="${i.end!=null?fmt2(i.end):''}"><button class="enn" title="End = the preview's current position (key ])">here ⇥</button>
 <span style="width:12px"></span><span title="Movie sprite levels in reforce (.reforce.txt): l = ((l - Black) / (White - Black))^Gamma. Empty = default">Levels</span>
 ${[['bk','black','0'],['wt','white','1'],['gm','gamma','1']].map(([c,k,ph])=>`<input class="tm lv ${c} ${i[k]!=null?'set':''}" title="${k[0].toUpperCase()+k.slice(1)} (empty = ${ph})" placeholder="${k[0].toUpperCase()} ${ph}" value="${i[k]!=null?i[k]:''}">`).join('')}
 <span class="err"></span></div>`;
 d.querySelector('.name').textContent=i.rel;
 const v=d.querySelector('video');
 const play=()=>{select(i.id);if(v.classList.contains('hide')){v.src='/preview/'+i.id;v.classList.remove('hide');v.play()}else if(v.paused)v.play();else v.pause()};
 d.querySelector('.play').onclick=play; const st=d.querySelector('.strip'); if(st)st.onclick=e=>{play();const f=e.offsetX/st.clientWidth;v.addEventListener('loadedmetadata',()=>{v.currentTime=f*v.duration},{once:true});if(v.readyState>0)v.currentTime=f*v.duration};
 d.querySelector('.qt').onclick=()=>post('/api/open',{id:i.id});
 d.querySelector('.rev').onclick=()=>post('/api/reveal',{id:i.id});
 for(const [c,t] of [['k','keep'],['m','maybe'],['d','drop'],['c','']]) d.querySelector('.'+c).onclick=()=>tag(i.id,t);
 d.querySelector('.lp').onclick=()=>loop(i.id);
 d.querySelector('.bt').onclick=()=>beats(i.id);
 for(const [cls,key] of [['st','start'],['en','end']]){const inp=d.querySelector('.'+cls);
  inp.onchange=()=>setTime(i.id,key,inp.value);inp.onkeydown=e=>{if(e.key==='Enter')inp.blur();e.stopPropagation()}}
 d.querySelector('.stn').onclick=()=>here(i.id,'start');d.querySelector('.enn').onclick=()=>here(i.id,'end');
 for(const [cls,key] of [['bk','black'],['wt','white'],['gm','gamma']]){const inp=d.querySelector('.'+cls);
  inp.onchange=()=>setLevel(i.id,key,inp.value);inp.onkeydown=e=>{if(e.key==='Enter')inp.blur();e.stopPropagation()}}
 d.onclick=e=>{if(e.target===d||e.target.classList.contains('top'))select(i.id)};
 return d}
function render(){const l=$('#list');l.innerHTML='';for(const i of visible())l.appendChild(rowEl(i));counts();mark()}
function mark(){document.querySelectorAll('.row.sel').forEach(e=>e.classList.remove('sel'));const e=document.getElementById('r'+sel);if(e)e.classList.add('sel')}
function select(id){sel=id;mark()}
async function post(u,b){const r=await fetch(u,{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(b)});return r.json()}
async function tag(id,t){const j=await post('/api/tag',{id,tag:t});items[id].v=j.v;const el=document.getElementById('r'+id);
 const vid=el&&el.querySelector('video');const playing=vid&&!vid.classList.contains('hide');
 if(el&&!playing)el.replaceWith(rowEl(items[id]));else if(el){el.className='row sel v-'+(j.v||'none');for(const [c,tt] of [['k','keep'],['m','maybe'],['d','drop']])el.querySelector('.'+c).classList.toggle('on',j.v===tt)}
 counts();mark()}
async function loop(id){const j=await post('/api/loop',{id,loop:!items[id].loop});items[id].loop=j.loop;
 const el=document.getElementById('r'+id);if(el)el.querySelector('.lp').classList.toggle('on',j.loop)}
async function setTime(id,key,value){const j=await post('/api/time',{id,key,value});const it=items[id];it.start=j.start;it.end=j.end;
 const el=document.getElementById('r'+id);if(!el)return;el.querySelector('.err').textContent=j.error||'';
 const st=el.querySelector('.st'),en=el.querySelector('.en');st.value=j.start!=null?fmt2(j.start):'';en.value=j.end!=null?fmt2(j.end):'';
 st.classList.toggle('set',j.start!=null);en.classList.toggle('set',j.end!=null)}
async function setLevel(id,key,value){const j=await post('/api/level',{id,key,value});const it=items[id];
 const el=document.getElementById('r'+id);
 for(const [c,k] of [['bk','black'],['wt','white'],['gm','gamma']]){it[k]=j[k];if(!el)continue;const inp=el.querySelector('.'+c);inp.value=j[k]!=null?j[k]:'';inp.classList.toggle('set',j[k]!=null)}
 if(el)el.querySelector('.err').textContent=j.error||''}
function here(id,key){const el=document.getElementById('r'+id);const v=el&&el.querySelector('video');
 if(!v||v.classList.contains('hide')||!v.readyState){if(el)el.querySelector('.err').textContent='Play the preview first, then pause where you want it';return}
 setTime(id,key,v.currentTime.toFixed(2))}
function move(dir){const v=visible();let k=v.findIndex(i=>i.id===sel);k=Math.max(0,Math.min(v.length-1,(k<0?0:k+dir)));if(!v[k])return;
 document.querySelectorAll('video').forEach(x=>x.pause());select(v[k].id);document.getElementById('r'+sel).scrollIntoView({block:'center',behavior:'smooth'})}
document.addEventListener('keydown',e=>{if(e.metaKey||e.ctrlKey||e.altKey)return;const k=e.key;
 if(e.target.tagName==='INPUT')return;
 if(k==='/'){$('#q').focus();e.preventDefault();return}
 if(k==='['){here(sel,'start');return} if(k===']'){here(sel,'end');return}
 if(k==='j'||k==='ArrowDown'){move(1);e.preventDefault()}else if(k==='k'||k==='ArrowUp'){move(-1);e.preventDefault()}
 else if(k==='1')tag(sel,'keep');else if(k==='2')tag(sel,'maybe');else if(k==='3')tag(sel,'drop');else if(k==='0')tag(sel,'');
 else if(k===' '){const el=document.getElementById('r'+sel);if(el){el.querySelector('.play').click();e.preventDefault()}}
 else if(k==='o')post('/api/open',{id:sel});else if(k==='l')loop(sel)
 else if(k==='b')beats(sel);else if(k==='t'&&BP[sel])BP[sel].tap();else if(k==='d'&&BP[sel])BP[sel].one()});
document.querySelectorAll('.f').forEach(b=>b.onclick=()=>{filter=b.dataset.f;document.querySelectorAll('.f').forEach(x=>x.classList.toggle('on',x===b));render()});
$('#collect').onclick=async()=>{const b=$('#collect'),o=$('#collectOut');b.disabled=true;b.textContent='Collecting…';
 try{const j=await post('/api/collect',{});o.textContent=j.text+'\n(click to hide)';o.classList.remove('hide')}
 finally{b.disabled=false;b.textContent='Collect → ~/reforce/movies'}};
$('#collectOut').onclick=()=>$('#collectOut').classList.add('hide');
$('#q').addEventListener('input',e=>{query=e.target.value;render();const v=visible();if(v.length&&!v.some(i=>i.id===sel)){sel=v[0].id;mark()}});
$('#q').addEventListener('keydown',e=>{if(e.key==='Escape'){e.target.value='';query='';render();e.target.blur()}else if(e.key==='Enter')e.target.blur()});

// ---------- beat panel (dance clips: tools/dance_sync) ----------
const BP={};let AC=null;
function click(when,one){if(!AC)return;const o=AC.createOscillator(),g=AC.createGain();o.frequency.value=one?2000:1200;
 g.gain.setValueAtTime(one?0.5:0.3,when);g.gain.exponentialRampToValueAtTime(0.001,when+0.05);o.connect(g).connect(AC.destination);o.start(when);o.stop(when+0.06)}
async function beats(id){const row=document.getElementById('r'+id);if(!row)return;select(id);
 if(BP[id]){BP[id].close();return}
 if(!AC)AC=new (window.AudioContext||window.webkitAudioContext)();AC.resume();
 const it=items[id],v=row.querySelector('video');
 if(v.classList.contains('hide')){v.src='/preview/'+id;v.classList.remove('hide')}
 const p=document.createElement('div');p.className='beats';
 p.innerHTML=`<canvas class="bcv"></canvas><div class="btns"><span class="bdots"></span>
  BPM <input class="tm bpm" placeholder="–"> <button class="x2" title="Double the tempo">×2</button><button class="h2" title="Halve the tempo">÷2</button>
  <button class="nl" title="Move the grid 10 ms earlier (shift: one frame)">◀ 10 ms</button><button class="nr" title="Move the grid 10 ms later (shift: one frame)">10 ms ▶</button>
  <button class="one" title="The beat nearest the playhead is the 1 of the bar (key d)">This beat is 1</button>
  <button class="tap" title="Tap along on the beat while it plays (key t); taps set the BPM and the 1">Tap</button>
  <label class="mut"><input type="checkbox" class="clk" checked> click</label>
  <select class="spd" title="Playback speed"><option value="1">1×</option><option value="0.5">½×</option><option value="0.75">¾×</option></select>
  <button class="ana" title="Find the BPM from the dancers' motion (tools/dance_sync)">Analyze</button>
  <button class="sv" title="Write BPM / BeatStart / BeatsPerBar to the movie's .reforce.txt (and its copy in ~/reforce/movies)">Save</button>
  <button class="clr" title="Remove the beat keys: reforce plays it unsynced">Clear</button>
  <span class="bmsg mut"></span></div><div class="balt mut"></div>`;
 v.after(p);row.querySelector('.bt').classList.add('on');
 const q=s=>p.querySelector(s),cv=q('.bcv'),ctx=cv.getContext('2d');
 const st={bpm:null,start:0,bpb:4,an:null,saved:null,taps:[],lastSched:-1,alive:true,dirty:false};
 const msg=t=>q('.bmsg').textContent=t;
 const period=()=>60/st.bpm, bar=()=>period()*st.bpb;
 function norm(){if(!st.bpm)return;const b=bar();st.start=((st.start%b)+b)%b}
 function show(){q('.bpm').value=st.bpm?st.bpm.toFixed(3).replace(/\.?0+$/,''):'';
  const sv=st.saved;const same=sv&&sv.bpm&&st.bpm&&Math.abs(sv.bpm-st.bpm)<5e-4&&Math.abs((sv.beatStart||0)-st.start)<5e-4;
  q('.sv').textContent=st.bpm&&!same?'Save *':'Save';
  q('.bdots').innerHTML=Array.from({length:st.bpb},(_,k)=>`<i class="${k?'':'one'}"></i>`).join('')}
 function setFrom(o){if(o&&o.bpm){st.bpm=o.bpm;st.start=o.beatStart??o.beat_start??0;st.bpb=o.beatsPerBar||o.beats_per_bar||4;norm()}show()}
 function alts(a){const box=q('.balt');if(!a){box.textContent=st.saved&&st.saved.bpm?'':'Not analyzed yet: Analyze, or tap along.';return}
  const c=(a.candidates||[]).slice(1,5).map(x=>x.bpm),o=Object.values(a.alternatives||{}).map(x=>x.bpm);
  box.innerHTML=`analysis: <b>${a.bpm.toFixed(2)}</b> BPM, confidence ${a.confidence.toFixed(2)} (${a.signal||''}) · try: `;
  for(const b of [...new Set([a.bpm,...o,...c].map(x=>+x.toFixed(2)))]){const bt=document.createElement('button');bt.className='chip';bt.textContent=b;
   bt.onclick=()=>{st.bpm=b;if(Math.abs(b-a.bpm)<0.01)st.start=a.beat_start;norm();show()};box.append(bt,' ')}}
 const r=await post('/api/beats',{id});st.saved=r.options;st.an=r.analysis;
 if(r.options&&r.options.bpm)setFrom(r.options);else if(r.analysis)setFrom(r.analysis);else show();
 alts(st.an);msg(r.interpolated?'(analysis uses the interpolated copy)':'');
 q('.bpm').onchange=e=>{const x=parseFloat(e.target.value);if(x>20&&x<400){st.bpm=x;norm()}show()};
 q('.bpm').onkeydown=e=>{if(e.key==='Enter')e.target.blur();e.stopPropagation()};
 q('.x2').onclick=()=>{if(st.bpm){st.bpm*=2;show()}};q('.h2').onclick=()=>{if(st.bpm){st.bpm/=2;norm();show()}};
 const nudge=(e,dir)=>{if(!st.bpm)return;st.start+=dir*(e.shiftKey?1/(it.fps||25):0.01);norm();show()};
 q('.nl').onclick=e=>nudge(e,-1);q('.nr').onclick=e=>nudge(e,1);
 const one=()=>{if(!st.bpm)return;const k=Math.round((v.currentTime-st.start)/period());st.start+=k*period();norm();show();msg('the 1 moved')};
 q('.one').onclick=one;
 const tap=()=>{const t=v.currentTime;if(st.taps.length&&(t<st.taps[st.taps.length-1]||t-st.taps[st.taps.length-1]>2.5))st.taps=[];
  st.taps.push(t);const n=st.taps.length;if(n<4){msg(`tap ${n}…`);return}
  const xs=st.taps.map((_,i)=>i),mx=(n-1)/2,my=st.taps.reduce((a,b)=>a+b)/n;let sxy=0,sxx=0;
  for(let i=0;i<n;i++){sxy+=(i-mx)*(st.taps[i]-my);sxx+=(i-mx)**2}
  const per=sxy/sxx;st.bpm=60/per;st.start=my-per*mx;norm();show();msg(`tap ${n}: ${st.bpm.toFixed(1)} BPM (first tap = the 1)`);
  st.start=st.taps[0];norm();show()};
 q('.tap').onclick=tap;
 q('.spd').onchange=e=>{v.playbackRate=+e.target.value};
 q('.ana').onclick=async()=>{msg('analyzing… (about 10–30 s)');q('.ana').disabled=true;
  try{const j=await post('/api/beats/analyze',{id});if(j.error){msg(j.error);return}st.an=j.analysis;setFrom(st.an);alts(st.an);msg('analyzed (not saved yet)')}
  finally{q('.ana').disabled=false}};
 q('.sv').onclick=async()=>{if(!st.bpm){msg('no BPM');return}const j=await post('/api/beats/save',{id,bpm:st.bpm,beatStart:st.start,beatsPerBar:st.bpb});
  if(j.error){msg(j.error);return}st.saved=j.options;items[id].bpm=j.options.bpm;show();msg('saved to .reforce.txt')};
 q('.clr').onclick=async()=>{const j=await post('/api/beats/save',{id,bpm:null});st.saved=j.options;items[id].bpm=null;msg('beat keys removed');show()};
 v.addEventListener('seeking',()=>{st.lastSched=-1});
 function frame(){if(!st.alive)return;requestAnimationFrame(frame);
  const W=cv.width=cv.clientWidth*devicePixelRatio,H=cv.height=cv.clientHeight*devicePixelRatio,t=v.currentTime,span=3;
  ctx.fillStyle='#0b0b0e';ctx.fillRect(0,0,W,H);const X=tt=>(tt-t+span)/(2*span)*W;
  const a=st.an;if(a&&a.envelope){const rate=a.envelope_rate,e0=a.envelope_start||0;
   for(const [arr,col] of [[a.hits,'#78e678'],[a.bounce,'#ffa03c']]){if(!arr)continue;ctx.strokeStyle=col;ctx.lineWidth=devicePixelRatio;ctx.beginPath();
    const i0=Math.max(0,Math.floor((t-span-e0)*rate)),i1=Math.min(arr.length,Math.ceil((t+span-e0)*rate));
    for(let i=i0;i<i1;i++){const y=H-6-Math.min(1.2,arr[i])/1.2*(H-14);i===i0?ctx.moveTo(X(e0+i/rate),y):ctx.lineTo(X(e0+i/rate),y)}ctx.stroke()}}
  let k=-1;
  if(st.bpm){const P=period();const k0=Math.ceil((t-span-st.start)/P),k1=Math.floor((t+span-st.start)/P);
   for(let j=k0;j<=k1;j++){const bt=st.start+j*P,dn=((j%st.bpb)+st.bpb)%st.bpb===0;ctx.fillStyle=dn?'#ff453a':'#5ac8fa';ctx.fillRect(X(bt)-(dn?1.5:0.5)*devicePixelRatio,0,(dn?3:1)*devicePixelRatio,H)}
   k=Math.floor((t-st.start)/P+1e-6);const since=t-(st.start+k*P),n=((k%st.bpb)+st.bpb)%st.bpb;
   p.querySelectorAll('.bdots i').forEach((el,j)=>el.classList.toggle('on',j===n));
   v.style.outline=since<Math.min(0.15,0.4*P)?`6px solid ${n?'#5ac8fa':'#ff453a'}`:'none';
   if(!v.paused&&q('.clk').checked&&AC){const rate=v.playbackRate||1,ahead=0.15*rate;let from=Math.max(st.lastSched,t);
    for(let j=Math.ceil((from-st.start)/P+1e-9);st.start+j*P<=t+ahead;j++){const bt=st.start+j*P;if(bt<=st.lastSched)continue;
     click(AC.currentTime+(bt-t)/rate,((j%st.bpb)+st.bpb)%st.bpb===0);st.lastSched=bt}}}
  ctx.fillStyle='#fff';ctx.fillRect(W/2-devicePixelRatio/2,0,devicePixelRatio,H)}
 requestAnimationFrame(frame);
 BP[id]={tap,one,close(){st.alive=false;p.remove();v.style.outline='none';row.querySelector('.bt').classList.remove('on');delete BP[id]}};
 if(v.paused)v.play().catch(()=>{})}
load();
</script></body></html>"""


class Review:
    def __init__(self, folder: Path):
        self.folder = folder
        self.items = scan(folder)
        self.done = 0
        CACHE.mkdir(parents=True, exist_ok=True)
        threading.Thread(target=self._prepare, daemon=True).start()

    def _prepare(self):
        def one(it):
            probe(it)
            make_strip(it)
            self.done += 1
        with ThreadPoolExecutor(max_workers=4) as ex:
            list(ex.map(one, self.items))

    def listing(self):
        out = []
        for it in self.items:
            out.append({"id": it["id"], "rel": it["rel"], "size": it["size"], "sidecar": it["sidecar"],
                        "dur": it.get("dur", 0), "w": it.get("w", 0), "h": it.get("h", 0), "codec": it.get("codec", ""),
                        "strip": (CACHE / f"{it['key']}.jpg").exists(), "v": verdict(read_tags(it["path"])),
                        "loop": read_loop(it["path"]), "start": parse_time(read_options(it["path"]).get("start", "")),
                        "end": parse_time(read_options(it["path"]).get("end", "")),
                        "bpm": beat_options(it["path"])["bpm"], "fps": it.get("fps", 0), **level_options(it["path"])})
        n = len(self.items)
        return {"folder": str(self.folder), "items": out, "pending": self.done < n,
                "progress": f"thumbnails {self.done}/{n}" if self.done < n else "thumbnails ready"}


collect_lock = threading.Lock()


def serve(rv: Review, open_browser: bool = True):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, body: bytes, ctype="application/json", extra=None):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def _item(self, s):
            try:
                return rv.items[int(s)]
            except (ValueError, IndexError):
                return None

        def _file(self, p: Path, ctype):
            size = p.stat().st_size
            rng = self.headers.get("Range")
            start, end = 0, size - 1
            m = re.match(r"bytes=(\d*)-(\d*)", rng or "")
            if m:
                if m.group(1):
                    start = int(m.group(1))
                    if m.group(2):
                        end = min(int(m.group(2)), size - 1)
                else:
                    start = max(0, size - int(m.group(2)))
            length = end - start + 1
            self.send_response(206 if m else 200)
            self.send_header("Content-Type", ctype)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(length))
            if m:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            with open(p, "rb") as f:
                f.seek(start)
                left = length
                try:
                    while left > 0:
                        chunk = f.read(min(1 << 20, left))
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        left -= len(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        def do_GET(self):
            path = self.path.split("?")[0]
            if path == "/":
                return self._send(200, PAGE.encode(), "text/html; charset=utf-8")
            if path == "/api/list":
                return self._send(200, json.dumps(rv.listing()).encode())
            m = re.match(r"/(thumb|preview)/(\d+)$", path)
            it = self._item(m.group(2)) if m else None
            if not it:
                return self._send(404, b"{}")
            if m.group(1) == "thumb":
                p = CACHE / f"{it['key']}.jpg"
                return self._file(p, "image/jpeg") if p.exists() else self._send(404, b"{}")
            p = preview(it)
            return self._file(p, "video/mp4") if p else self._send(500, b'{"error":"preview failed"}')

        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            try:
                body = json.loads(self.rfile.read(n) or b"{}")
            except json.JSONDecodeError:
                return self._send(400, b"{}")
            if self.path == "/api/collect":
                if rv.folder.resolve() == DEST.resolve():
                    return self._send(200, json.dumps({"text": f"This sheet shows {DEST} itself: nothing to collect."}).encode())
                buf = io.StringIO()
                with collect_lock, contextlib.redirect_stdout(buf):
                    collect(rv.folder, "dry" if body.get("dry") else "copy")
                return self._send(200, json.dumps({"text": buf.getvalue()}).encode())
            it = self._item(body.get("id"))
            if not it:
                return self._send(404, b"{}")
            if self.path == "/api/tag":
                v = body.get("tag", "")
                if v not in ("", *TAGS):
                    return self._send(400, b"{}")
                return self._send(200, json.dumps({"v": set_verdict(it["path"], v)}).encode())
            if self.path == "/api/time":
                key = body.get("key")
                if key not in ("start", "end"):
                    return self._send(400, b"{}")
                opts, err = set_time(it["path"], key, str(body.get("value", "")))
                return self._send(200, json.dumps({"start": parse_time(opts.get("start", "")),
                                                   "end": parse_time(opts.get("end", "")), "error": err}).encode())
            if self.path == "/api/level":
                key = body.get("key")
                if key not in LEVEL_KEYS:
                    return self._send(400, b"{}")
                levels, err = set_level(it["path"], key, str(body.get("value", "")))
                return self._send(200, json.dumps({**levels, "error": err}).encode())
            if self.path == "/api/beats":
                return self._send(200, json.dumps({"options": beat_options(it["path"]),
                                                   "analysis": beat_analysis(it["path"]),
                                                   "interpolated": bool(interpolated_copy(it["path"]))}).encode())
            if self.path == "/api/beats/analyze":
                a, err = analyze_beats(it["path"])
                return self._send(200, json.dumps({"analysis": a, "error": err}).encode())
            if self.path == "/api/beats/save":
                try:
                    bpm = float(body["bpm"]) if body.get("bpm") not in (None, "") else None
                    start = float(body.get("beatStart") or 0.0)
                    bpb = int(body.get("beatsPerBar") or 4)
                except (TypeError, ValueError):
                    return self._send(400, b"{}")
                if bpm is not None and not (20 <= bpm <= 400) or not (1 <= bpb <= 32):
                    return self._send(400, b'{"error":"BPM 20-400, beats per bar 1-32"}')
                return self._send(200, json.dumps({"options": save_beats(it["path"], bpm, start, bpb)}).encode())
            if self.path == "/api/loop":
                return self._send(200, json.dumps({"loop": set_loop(it["path"], bool(body.get("loop")))}).encode())
            if self.path == "/api/open":
                subprocess.Popen(["open", "-a", "QuickTime Player", str(it["path"])])
                return self._send(200, b"{}")
            if self.path == "/api/reveal":
                subprocess.Popen(["open", "-R", str(it["path"])])
                return self._send(200, b"{}")
            return self._send(404, b"{}")

    srv = ThreadingHTTPServer(("127.0.0.1", PORT), H)
    url = f"http://127.0.0.1:{PORT}/"
    print(f"Movie review: {len(rv.items)} movies in {rv.folder}\n  {url}   (Ctrl+C to stop)", flush=True)
    if open_browser:
        subprocess.Popen(["open", url])
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


def copy_clone(src: Path, dst: Path) -> None:
    """APFS clone (cp -c) when possible, else a normal copy."""
    if subprocess.run(["cp", "-c", str(src), str(dst)], capture_output=True).returncode != 0:
        if dst.exists():
            dst.unlink()
        shutil.copy2(str(src), str(dst))


def collect(folder: Path, mode: str):
    """mode: "dry", "copy", "move" or "prune"."""
    items = scan(folder)
    keep = [it for it in items if verdict(read_tags(it["path"])) == "keep"]
    DEST.mkdir(parents=True, exist_ok=True)
    verb = {"dry": "would copy", "copy": "copying", "move": "moving"}.get(mode, "would copy")
    new = [it for it in keep if not (DEST / it["path"].name).exists()]
    if mode != "prune":
        print(f"{len(keep)} Keep movie(s); {len(new)} not yet in {DEST} ({verb}):")
        for it in new:
            src = it["path"]
            side = src.with_suffix(".txt")
            print(f"  {it['rel']}{'  (+ .txt)' if side.exists() else ''}")
            if mode in ("copy", "move"):
                opts = options_path(src)
                for a, b in ((src, DEST / src.name), (side, DEST / side.name), (opts, DEST / opts.name)):
                    if a.exists() and not b.exists():
                        if mode == "move":
                            shutil.move(str(a), str(b))
                        else:
                            copy_clone(a, b)
    # Options files (.reforce.txt, e.g. Loop) of movies already copied follow the originals.
    if mode in ("dry", "copy"):
        for it in keep:
            if it in new:
                continue
            src, dst = options_path(it["path"]), DEST / options_path(it["path"]).name
            want = src.read_bytes() if src.exists() else None
            have = dst.read_bytes() if dst.exists() else None
            if want == have:
                continue
            print(f"  options {'update' if want else 'remove'}: {dst.name}{'' if mode == 'copy' else ' (--copy does it)'}")
            if mode == "copy":
                if want is None:
                    dst.unlink()
                else:
                    dst.write_bytes(want)
    # Copies whose original is no longer tagged Keep.
    keep_names = {it["path"].name for it in keep}
    stale = [it for it in items if it["path"].name not in keep_names and (DEST / it["path"].name).exists()]
    if stale:
        print(f"{len(stale)} cop{'y' if len(stale) == 1 else 'ies'} in {DEST} whose original is no longer Keep"
              f"{' (deleting)' if mode == 'prune' else ' (--prune deletes)'}:")
        for it in stale:
            print(f"  {it['path'].name}")
            if mode == "prune":
                for f in (DEST / it["path"].name, DEST / it["path"].with_suffix(".txt").name,
                          DEST / options_path(it["path"]).name):
                    if f.exists():
                        f.unlink()
    elif mode == "prune":
        print("Nothing to prune.")
    if mode == "dry":
        print("Dry run. --copy copies, --prune deletes stale copies.")


def source_folder(args):
    """The folder of movies to review: the argument, else $REFORCE_MOVIE_SOURCE."""
    folder = args[0] if args else os.environ.get("REFORCE_MOVIE_SOURCE", "")
    if not folder:
        sys.exit("Which folder? Pass FOLDER or set REFORCE_MOVIE_SOURCE.")
    return Path(folder).expanduser()

def main(argv):
    args = list(argv)
    if args and args[0] == "collect":
        args.pop(0)
        mode = "dry"
        for flag in ("--copy", "--move", "--prune"):
            if flag in args:
                mode = flag[2:]
        args = [a for a in args if not a.startswith("--")]
        folder = source_folder(args)
        return collect(folder, mode)
    global PORT
    open_browser = "--no-open" not in args
    if "--port" in args:
        i = args.index("--port")
        PORT = int(args[i + 1])
        del args[i:i + 2]
    args = [a for a in args if a != "--no-open"]
    folder = source_folder(args)
    if not folder.is_dir():
        sys.exit(f"Not a folder: {folder}")
    for tool in ("ffmpeg", "ffprobe"):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found (brew install ffmpeg)")
    serve(Review(folder), open_browser)


if __name__ == "__main__":
    main(sys.argv[1:])
