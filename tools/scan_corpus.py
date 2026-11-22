#!/usr/bin/env python3
"""Empirical inventory of G-Force scene (config) files.

Usage:
    python3 -I tools/scan_corpus.py [SCENES_DIR] [--out docs/corpus-stats.md] [--stdout]

SCENES_DIR defaults to ../scenes relative to this script.  Only the
G-Force.*.package trees under SCENES_DIR/Packages are scanned (all *.txt,
recursively).  Standard library only.

The generated markdown keeps any hand-written summary placed between the
HUMAN-SUMMARY markers of an existing output file, so the file can be
regenerated without losing it.

The scanner deliberately does NOT paste scene content: excerpts are limited to
short single-line snippets that illustrate anomalies (G-Force content is
SoundSpectrum-owned).
"""

import argparse
import collections
import os
import re
import sys

# --------------------------------------------------------------------------
# Scene-type classification
# --------------------------------------------------------------------------

TYPES = ["WaveShape", "Particle", "FlowField", "FlowField Union", "ColorMap", "Sprite"]


def classify(relpath):
    p = relpath.replace(os.sep, "/")
    if p.startswith("G-Force.WaveShape.package/Particles/"):
        return "Particle"
    if p.startswith("G-Force.WaveShape.package/"):
        return "WaveShape"
    if p.startswith("G-Force.FlowField Union.package/"):
        return "FlowField Union"
    if p.startswith("G-Force.FlowField.package/"):
        return "FlowField"
    if p.startswith("G-Force.ColorMap.package/"):
        return "ColorMap"
    if p.startswith("G-Force.Sprite.package/"):
        return "Sprite"
    return None


# --------------------------------------------------------------------------
# Lexical layer: file -> comments + KEY=VALUE entries
# --------------------------------------------------------------------------

USER_VAR_RE = re.compile(r"^([A-Da-d])(\d+)$")
FAMILY_RE = re.compile(r"^([A-Za-z_]+?)(\d+)$")
NUMERIC_VALUE_RE = re.compile(r"^[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?$")
DIRECTIVE_RE = re.compile(r"(?<![\w&])#([A-Za-z][\w-]*)")


class Entry:
    __slots__ = ("key", "value", "quoted", "line", "end_line", "issues")

    def __init__(self, key, value, quoted, line, end_line):
        self.key = key
        self.value = value
        self.quoted = quoted
        self.line = line
        self.end_line = end_line
        self.issues = []


class FileScan:
    def __init__(self, path, rel, stype):
        self.path = path
        self.rel = rel
        self.stype = stype
        self.entries = []
        self.comments = []  # (line, text, kind)
        self.anomalies = []  # (category, line, detail)
        self.line_endings = collections.Counter()
        self.non_ascii = []  # (line, context, bytes repr)
        self.bom = False

    def anomaly(self, cat, line, detail=""):
        self.anomalies.append((cat, line, detail))


def snippet(s, n=70):
    s = s.replace("\t", " ").strip()
    s = re.sub(r"\s+", " ", s)
    return s if len(s) <= n else s[: n - 3] + "..."


def lex_file(path, rel, stype):
    fs = FileScan(path, rel, stype)
    raw = open(path, "rb").read()
    if raw.startswith(b"\xef\xbb\xbf"):
        fs.bom = True
        raw = raw[3:]
    # line endings
    crlf = raw.count(b"\r\n")
    cr = raw.count(b"\r") - crlf
    lf = raw.count(b"\n") - crlf
    if crlf:
        fs.line_endings["CRLF"] = crlf
    if cr:
        fs.line_endings["CR"] = cr
    if lf:
        fs.line_endings["LF"] = lf
    if raw and not raw.endswith((b"\n", b"\r")):
        fs.line_endings["no-final-newline"] = 1
    # decoding: try utf-8, fall back to latin-1 (cp1252/MacRoman indistinguishable here)
    try:
        text = raw.decode("utf-8")
        enc = "utf-8" if any(b > 127 for b in raw) else "ascii"
    except UnicodeDecodeError:
        text = raw.decode("latin-1")
        enc = "8-bit (not UTF-8)"
    fs.encoding = enc
    if "\x00" in text:
        fs.anomaly("NUL bytes", 0, "")
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    lines = text.split("\n")

    # character-level state machine over the whole file
    i, n, line = 0, len(text), 1
    state_ctx = "code"  # for non-ascii context reporting

    def peek(k=0):
        j = i + k
        return text[j] if j < n else ""

    pending_key = None  # (key, line)
    stray_line_reported = set()

    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c in " \t\f\v":
            i += 1
            continue
        if ord(c) > 127:
            fs.non_ascii.append((line, "code", repr(c)))
        # comments
        if c == "/" and peek(1) == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            body = text[i + 2 : j]
            fs.comments.append((line, body, "//"))
            for ch in body:
                if ord(ch) > 127:
                    fs.non_ascii.append((line, "comment", repr(ch)))
                    break
            i = j
            continue
        if c == "/" and peek(1) == "*":
            j = text.find("*/", i + 2)
            start_line = line
            if j < 0:
                body = text[i + 2 :]
                fs.anomaly("unterminated /* comment", start_line, snippet(lines[start_line - 1]))
                j_end = n
            else:
                body = text[i + 2 : j]
                j_end = j + 2
            if "/*" in body:
                fs.anomaly("nested /* inside /* comment", start_line, "")
            fs.comments.append((start_line, body, "/*"))
            if any(ord(ch) > 127 for ch in body):
                fs.non_ascii.append((start_line, "comment", "(in block comment)"))
            line += body.count("\n")
            i = j_end
            continue
        if c in ",;":
            fs.anomaly("'%s' separator after a value (C-initializer style)" % c, line, snippet(lines[line - 1]))
            i += 1
            continue
        if c == "*" and peek(1) == "/":
            fs.anomaly("stray */ outside comment", line, snippet(lines[line - 1]))
            i += 2
            continue
        # KEY = VALUE
        m = re.compile(r"[A-Za-z_][A-Za-z0-9_]*").match(text, i)
        if m:
            key = m.group(0)
            j = m.end()
            # whitespace between key and '='
            k = j
            while k < n and text[k] in " \t":
                k += 1
            if k < n and text[k] == "=":
                if k != j:
                    fs.anomaly("space between key and '='", line, snippet(lines[line - 1]))
                key_line = line
                k += 1
                ws_start = k
                while k < n and text[k] in " \t":
                    k += 1
                if k > ws_start:
                    fs.anomaly("space after '='", line, snippet(lines[line - 1]))
                # value on following line?
                if k < n and text[k] == "\n":
                    kk = k
                    nl = 0
                    while kk < n and text[kk] in " \t\n":
                        if text[kk] == "\n":
                            nl += 1
                        kk += 1
                    if kk < n and text[kk] == '"':
                        fs.anomaly("value on line after '='", key_line, key)
                        line += nl
                        k = kk
                    else:
                        e = Entry(key, "", False, key_line, key_line)
                        e.issues.append("empty unquoted value")
                        fs.entries.append(e)
                        fs.anomaly("empty value (KEY= with nothing)", key_line, key)
                        i = k
                        continue
                if k < n and text[k] == '"':
                    # quoted string (may span lines -> anomaly)
                    end = text.find('"', k + 1)
                    if end < 0:
                        val = text[k + 1 :]
                        fs.anomaly("unterminated string (runs to EOF)", key_line, snippet(lines[key_line - 1]))
                        end_i = n
                    else:
                        val = text[k + 1 : end]
                        end_i = end + 1
                    nls = val.count("\n")
                    e = Entry(key, val, True, key_line, key_line + nls)
                    if nls:
                        fs.anomaly("string spans multiple lines", key_line,
                                   "%s spans %d lines" % (key, nls + 1))
                    if "//" in val or "/*" in val:
                        fs.anomaly("comment marker inside quoted value", key_line, snippet(lines[key_line - 1]))
                    for ch in val:
                        if ord(ch) > 127:
                            fs.non_ascii.append((key_line, "string", repr(ch)))
                            break
                    fs.entries.append(e)
                    line += nls
                    i = end_i
                    # trailing junk on same line after closing quote?
                    t = i
                    while t < n and text[t] in " \t":
                        t += 1
                    if t < n and text[t] not in "\n/,;":
                        rest = text[t : text.find("\n", t) if text.find("\n", t) >= 0 else n]
                        if not rest.startswith("*"):
                            fs.anomaly("trailing text after closing quote", line, snippet(lines[line - 1]))
                    continue
                # unquoted value: up to EOL or comment start
                t = k
                while t < n and text[t] not in "\n,;" and not (text[t] == "/" and t + 1 < n and text[t + 1] in "/*"):
                    t += 1
                val = text[k:t].rstrip(" \t")
                e = Entry(key, val, False, key_line, key_line)
                fs.entries.append(e)
                if '"' in val:
                    fs.anomaly("stray quote in unquoted value", key_line, snippet(lines[key_line - 1]))
                i = t
                continue
            # identifier not followed by '=' -> stray text
            pass
        # anything else: stray text not inside a comment / not a KEY=VALUE
        j = text.find("\n", i)
        j = n if j < 0 else j
        # stop at comment start so the comment is still processed
        cpos = [p for p in (text.find("//", i, j), text.find("/*", i, j)) if p >= 0]
        stop = min(cpos) if cpos else j
        if stop == i:
            stop = i + 1
        frag = text[i:stop]
        if line not in stray_line_reported:
            fs.anomaly("line not KEY=VALUE (stray text outside comment)", line, snippet(lines[line - 1]))
            stray_line_reported.add(line)
        i = stop
    return fs


# --------------------------------------------------------------------------
# Expression layer
# --------------------------------------------------------------------------

OPERATORS = ["==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "^^",
             "+", "-", "*", "/", "%", "^", "(", ")", ",", "<", ">", "!",
             "&", "|", "?", ":", "=", "~", "[", "]", "{", "}", ";"]
OPERATORS.sort(key=len, reverse=True)
NUM_RE = re.compile(r"0[xX][0-9A-Fa-f]+|(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?")
ID_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


class Tok:
    __slots__ = ("kind", "text", "pos")

    def __init__(self, kind, text, pos):
        self.kind, self.text, self.pos = kind, text, pos

    def __repr__(self):
        return "%s:%s" % (self.kind, self.text)


def tokenize_expr(s):
    toks, bad = [], []
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c.isspace():
            i += 1
            continue
        m = NUM_RE.match(s, i)
        if m and (c.isdigit() or (c == "." and i + 1 < n and s[i + 1].isdigit())):
            txt = m.group(0)
            # identifier glued to number, e.g. 2x
            i = m.end()
            if i < n and s[i] in "LlFf" and not (i + 1 < n and (s[i + 1].isalnum() or s[i + 1] == "_")):
                bad.append("C-style numeric suffix on literal: %r" % (txt + s[i]))
                txt += s[i]
                i += 1
            if i < n and s[i] == "." and i + 1 < n and s[i + 1].isdigit():
                bad.append("malformed number (two decimal points): %r" % s[m.start() : i + 4])
            toks.append(Tok("num", txt, i))
            if i < n and (s[i].isalpha() or s[i] == "_"):
                bad.append("identifier glued to number: %r" % s[max(0, i - 4) : i + 4])
            continue
        m = ID_RE.match(s, i)
        if m:
            toks.append(Tok("id", m.group(0), i))
            i = m.end()
            continue
        for op in OPERATORS:
            if s.startswith(op, i):
                toks.append(Tok("op", op, i))
                i += len(op)
                break
        else:
            bad.append("unexpected character: %r" % c)
            toks.append(Tok("bad", c, i))
            i += 1
    return toks, bad


def number_formats(txt):
    f = []
    if txt[:2] in ("0x", "0X"):
        return ["hex"]
    if txt[-1] in "LlFf":
        f.append("C suffix (1L)")
        txt = txt[:-1]
    if txt.startswith("."):
        f.append("leading-dot (.5)")
    if re.match(r"^\d+\.([eE]|$)", txt):
        f.append("trailing-dot (5.)")
    if "e" in txt or "E" in txt:
        f.append("exponent (1e3)")
    if re.match(r"^\d+\.\d+", txt):
        f.append("decimal (0.5)")
    if re.match(r"^\d+$", txt):
        f.append("integer")
        if len(txt) > 1 and txt.startswith("0"):
            f.append("leading-zero integer (05)")
    return f


# Pratt parser for the documented grammar -- used to verify every expression
# parses; failures are reported as anomalies.
BINARY_PREC = {
    "?": 1,
    "||": 2, "&&": 3,
    "|": 4, "^^": 5, "&": 6,
    "==": 7, "!=": 7,
    "<": 8, ">": 8, "<=": 8, ">=": 8,
    "<<": 9, ">>": 9,
    "+": 10, "-": 10,
    "*": 11, "/": 11, "%": 11,
    "^": 13,  # right assoc, binds tighter than unary
}
RIGHT_ASSOC = {"^", "?"}


class ParseError(Exception):
    pass


class Parser:
    def __init__(self, toks):
        self.t = toks
        self.i = 0
        self.features = collections.Counter()

    def peek(self, k=0):
        j = self.i + k
        return self.t[j] if j < len(self.t) else None

    def take(self):
        tok = self.peek()
        if tok is None:
            raise ParseError("unexpected end")
        self.i += 1
        return tok

    def expect(self, text):
        tok = self.take()
        if tok.text != text:
            raise ParseError("expected %r got %r" % (text, tok.text))

    def parse(self):
        if not self.t:
            raise ParseError("empty")
        self.expr(0)
        if self.peek() is not None:
            raise ParseError("trailing tokens starting at %r" % self.peek().text)

    def expr(self, minp):
        self.unary()
        while True:
            tok = self.peek()
            if tok is None or tok.kind != "op" or tok.text not in BINARY_PREC:
                return
            p = BINARY_PREC[tok.text]
            if p < minp:
                return
            self.take()
            if tok.text == "?":
                self.features["ternary ?:"] += 1
                self.expr(0)
                self.expect(":")
                self.expr(p)
                continue
            self.expr(p if tok.text in RIGHT_ASSOC else p + 1)

    def unary(self):
        tok = self.peek()
        if tok is not None and tok.kind == "op" and tok.text in ("-", "+", "!", "~"):
            self.take()
            self.features["unary " + tok.text] += 1
            # unary binds looser than ^ : -x^2 == -(x^2)
            self.unary()
            while self.peek() is not None and self.peek().text == "^":
                self.take()
                self.expr(BINARY_PREC["^"])
            return
        # casts: (int) / (float)
        if (tok is not None and tok.text == "(" and self.peek(1) is not None
                and self.peek(1).text in ("int", "float") and self.peek(2) is not None
                and self.peek(2).text == ")"):
            self.features["cast (%s)" % self.peek(1).text] += 1
            self.i += 3
            self.unary()
            return
        self.primary()

    def primary(self):
        tok = self.take()
        if tok.kind == "num":
            return
        if tok.kind == "id":
            nxt = self.peek()
            if nxt is not None and nxt.text == "(":
                self.take()
                if self.peek() is not None and self.peek().text == ")":
                    self.take()
                    return
                while True:
                    self.expr(0)
                    t2 = self.take()
                    if t2.text == ")":
                        return
                    if t2.text != ",":
                        raise ParseError("expected , or ) in call, got %r" % t2.text)
            return
        if tok.text == "(":
            self.expr(0)
            self.expect(")")
            return
        if tok.text == "{":
            self.features["array literal {...}"] += 1
            if self.peek() is not None and self.peek().text == "}":
                self.take()
                return
            while True:
                self.expr(0)
                t2 = self.take()
                if t2.text == "}":
                    return
                if t2.text != ",":
                    raise ParseError("expected , or } in array, got %r" % t2.text)
        raise ParseError("unexpected token %r" % tok.text)


def call_arg_counts(toks):
    """Yield (name, argc) for every id immediately followed by '('."""
    out = []
    for idx, tok in enumerate(toks):
        if tok.kind == "id" and idx + 1 < len(toks) and toks[idx + 1].text == "(":
            depth, argc, any_tok = 0, 0, False
            j = idx + 1
            while j < len(toks):
                tt = toks[j].text
                if tt in ("{", "["):
                    depth += 1
                    any_tok = True
                elif tt in ("}", "]"):
                    depth -= 1
                    any_tok = True
                elif tt == "(":
                    depth += 1
                    if depth > 1:
                        any_tok = True
                elif tt == ")":
                    depth -= 1
                    if depth == 0:
                        break
                    any_tok = True
                elif tt == "," and depth == 1:
                    argc += 1
                else:
                    any_tok = True
                j += 1
            if depth != 0:
                argc = None  # unbalanced
            elif any_tok or argc:
                argc += 1
            out.append((tok.text, argc, idx))
    return out


# --------------------------------------------------------------------------
# Aggregation
# --------------------------------------------------------------------------


def family_of(key):
    m = FAMILY_RE.match(key)
    if m:
        return m.group(1) + "#", int(m.group(2))
    return key, None


class TypeStats:
    def __init__(self):
        self.files = 0
        self.key_files = collections.Counter()  # case-preserved -> #files
        self.key_ci_variants = collections.defaultdict(collections.Counter)  # lower -> {variant: files}
        self.family_files = collections.Counter()  # family (ci, upper) -> files
        self.family_max = {}
        self.family_maxfile = {}
        self.calls = collections.Counter()
        self.call_variants = collections.defaultdict(collections.Counter)
        self.call_argc = collections.defaultdict(collections.Counter)
        self.call_files = collections.Counter()
        self.builtins = collections.Counter()
        self.builtin_variants = collections.defaultdict(collections.Counter)
        self.builtin_files = collections.Counter()
        self.builtin_by_key = collections.defaultdict(collections.Counter)  # name -> key family
        self.uservars = collections.Counter()  # family -> refs
        self.keyrefs = collections.Counter()  # refs to non-user-var keys defined in file (X0, Pen...)
        self.ops = collections.Counter()
        self.numfmt = collections.Counter()
        self.numfmt_example = {}
        self.vers = collections.Counter()
        self.parse_features = collections.Counter()
        self.complexity = []  # (tokens, nexpr, longest, rel)
        self.value_kinds = collections.Counter()
        self.stage_refs = collections.Counter()  # (defining family, referenced family)
        self.forward_refs = []  # (rel, key, ref)
        self.unquoted_expr = []
        self.meta = collections.Counter()


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("scenes", nargs="?", default=os.path.join(here, "..", "scenes"))
    ap.add_argument("--out", default=os.path.join(here, "..", "docs", "corpus-stats.md"))
    ap.add_argument("--stdout", action="store_true", help="print markdown instead of writing --out")
    args = ap.parse_args()

    pkg_root = os.path.join(args.scenes, "Packages")
    if not os.path.isdir(pkg_root):
        pkg_root = args.scenes
    files = []
    for dirpath, dirnames, filenames in os.walk(pkg_root):
        dirnames.sort()
        for fn in sorted(filenames):
            if fn.lower().endswith(".txt"):
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, pkg_root)
                st = classify(rel)
                if st:
                    files.append((full, rel, st))

    stats = {t: TypeStats() for t in TYPES}
    all_anoms = collections.defaultdict(list)  # category -> [(type, rel, line, detail)]
    directives = collections.defaultdict(list)  # tag -> [(type, rel)]
    encodings = collections.Counter()
    line_ending_files = collections.defaultdict(list)
    global_ops = collections.Counter()

    for full, rel, st in files:
        S = stats[st]
        S.files += 1
        fs = lex_file(full, rel, st)
        encodings[fs.encoding] += 1
        le = "+".join(sorted(k for k in fs.line_endings if k != "no-final-newline")) or "(none)"
        line_ending_files[le].append(rel)
        if "no-final-newline" in fs.line_endings:
            line_ending_files["no final newline"].append(rel)
        if fs.bom:
            fs.anomaly("UTF-8 BOM", 1, "")
        for (ln, ctx, ch) in fs.non_ascii:
            fs.anomaly("non-ASCII char in " + ctx, ln, "%s (file decodes as %s)" % (ch, fs.encoding))
        for (ln, body, kind) in fs.comments:
            for m in DIRECTIVE_RE.finditer(body):
                directives["#" + m.group(1)].append((st, rel))

        # keys / dups
        seen_exact, seen_ci = {}, {}
        defined_ci = set()
        for e in fs.entries:
            defined_ci.add(e.key.upper())
        file_keys = set()
        file_families = {}
        for e in fs.entries:
            if e.key in seen_exact:
                fs.anomaly("duplicate key (exact)", e.line, "%s (first at line %d)" % (e.key, seen_exact[e.key]))
            elif e.key.upper() in seen_ci:
                fs.anomaly("duplicate key (differs only in case)", e.line,
                           "%s vs %s" % (e.key, seen_ci[e.key.upper()][0]))
            seen_exact.setdefault(e.key, e.line)
            seen_ci.setdefault(e.key.upper(), (e.key, e.line))
            file_keys.add(e.key)
            fam, idx = family_of(e.key.upper())
            if idx is not None:
                file_families[fam] = max(file_families.get(fam, -1), idx)
        for k in file_keys:
            S.key_files[k] += 1
        for k in set(x.upper() for x in file_keys):
            pass
        ci_seen = collections.defaultdict(set)
        for k in file_keys:
            ci_seen[k.upper()].add(k)
        for ku, vs in ci_seen.items():
            for v in vs:
                S.key_ci_variants[ku][v] += 1
        for fam, mx in file_families.items():
            S.family_files[fam] += 1
            if mx > S.family_max.get(fam, -1):
                S.family_max[fam] = mx
                S.family_maxfile[fam] = rel
        # gaps in user var numbering
        for fam in ("A#", "B#", "C#", "D#"):
            if fam in file_families:
                idxs = sorted(int(family_of(k.upper())[1]) for k in file_keys
                              if family_of(k.upper())[0] == fam)
                missing = sorted(set(range(idxs[-1] + 1)) - set(idxs))
                if missing:
                    fs.anomaly("gap in user-var numbering", 0,
                               "%s missing %s" % (fam[0], ",".join(fam[0] + str(x) for x in missing[:6])
                                                  + ("..." if len(missing) > 6 else "")))

        # values
        n_expr, total_tok, longest = 0, 0, 0
        file_calls, file_builtins = set(), set()
        for e in fs.entries:
            val = e.value
            v = val.strip()
            if e.key.upper() == "VERS":
                S.vers[v if not e.quoted else '"%s"' % v] += 1
            if e.quoted:
                if v == "":
                    kind = "quoted empty"
                    fs.anomaly("empty expression (\"\")", e.line, e.key)
                elif NUMERIC_VALUE_RE.match(v):
                    kind = "quoted number"
                else:
                    kind = "quoted expression"
                if v != val:
                    fs.anomaly("leading/trailing whitespace inside quotes", e.line, e.key)
            else:
                if v == "":
                    kind = "unquoted empty"
                elif NUMERIC_VALUE_RE.match(v):
                    kind = "unquoted number"
                    if v.startswith("-"):
                        S.numfmt["negative (unquoted -N)"] += 1
                        S.numfmt_example.setdefault("negative (unquoted -N)", "%s=%s" % (e.key, v))
                    if v.startswith("+"):
                        fs.anomaly("unquoted number with leading +", e.line, "%s=%s" % (e.key, v))
                else:
                    kind = "unquoted expression"
                    fs.anomaly("unquoted non-numeric value", e.line, "%s=%s" % (e.key, snippet(v, 50)))
            if e.key.upper() == "META" and e.quoted:
                kind = "Meta metadata string"
                S.value_kinds[kind] += 1
                for mm in re.finditer(r"(\w+)\s*=\s*([^\s,]+)", v):
                    S.meta[mm.group(1)] += 1
                continue
            S.value_kinds[kind] += 1
            if kind in ("quoted empty", "unquoted empty"):
                continue
            toks, bad = tokenize_expr(v)
            for b in bad:
                fs.anomaly("tokenizer: " + b.split(": ")[0], e.line, "%s: %s" % (e.key, snippet(b.split(": ", 1)[1], 50)))
            n_expr += 1
            total_tok += len(toks)
            longest = max(longest, len(v))
            # parse check
            P = Parser(toks)
            try:
                P.parse()
            except ParseError as ex:
                fs.anomaly("expression does not parse", e.line, "%s: %s | %s" % (e.key, ex, snippet(v, 60)))
            S.parse_features.update(P.features)
            # paren balance
            depth, minimum = 0, 0
            for t in toks:
                if t.text == "(":
                    depth += 1
                elif t.text == ")":
                    depth -= 1
                    minimum = min(minimum, depth)
            if depth != 0 or minimum < 0:
                fs.anomaly("unbalanced parentheses", e.line, "%s: %s" % (e.key, snippet(v, 60)))
            # operators / numbers / identifiers
            for idx, t in enumerate(toks):
                if t.kind == "op":
                    S.ops[t.text] += 1
                    global_ops[t.text] += 1
                    if t.text == "=":
                        fs.anomaly("single '=' inside expression", e.line, "%s: %s" % (e.key, snippet(v, 60)))
                elif t.kind == "num":
                    for f in number_formats(t.text):
                        S.numfmt[f] += 1
                        S.numfmt_example.setdefault(f, t.text)
                    prev = toks[idx - 1] if idx else None
                    if prev is not None and prev.text == "-" and (idx == 1 or toks[idx - 2].kind == "op"
                                                                  and toks[idx - 2].text != ")"):
                        S.numfmt["negative (unary - before literal)"] += 1
                        S.numfmt_example.setdefault("negative (unary - before literal)", "-" + t.text)
            calls = call_arg_counts(toks)
            call_idx = set()
            for name, argc, idx in calls:
                call_idx.add(idx)
                lname = name.lower()
                S.calls[lname] += 1
                S.call_variants[lname][name] += 1
                S.call_argc[lname][argc if argc is not None else "unbalanced"] += 1
                file_calls.add(lname)
            def_fam = family_of(e.key.upper())
            for idx, t in enumerate(toks):
                if t.kind != "id" or idx in call_idx:
                    continue
                # casts (int)/(float)
                if t.text in ("int", "float") and idx and toks[idx - 1].text == "(" \
                        and idx + 1 < len(toks) and toks[idx + 1].text == ")":
                    continue
                up = t.text.upper()
                m = USER_VAR_RE.match(t.text)
                if m:
                    fam = m.group(1).upper() + "#"
                    S.uservars[fam] += 1
                    if up not in defined_ci:
                        fs.anomaly("reference to undefined user var", e.line, "%s uses %s" % (e.key, t.text))
                    if def_fam[1] is not None and def_fam[0] in ("A#", "B#", "C#", "D#"):
                        S.stage_refs[(def_fam[0][0], fam[0])] += 1
                        if fam == def_fam[0] and int(m.group(2)) >= def_fam[1]:
                            S.forward_refs.append((rel, e.key, t.text))
                    elif def_fam[1] is None or def_fam[0] not in ("A#", "B#", "C#", "D#"):
                        S.stage_refs[(e.key if def_fam[1] is None else def_fam[0], fam[0])] += 1
                elif up in defined_ci:
                    S.keyrefs[up] += 1
                    fs.anomaly("expression references a non-var key", e.line, "%s uses %s" % (e.key, t.text))
                else:
                    S.builtins[up] += 1
                    S.builtin_variants[up][t.text] += 1
                    S.builtin_by_key[up][def_fam[0]] += 1
                    file_builtins.add(up)
            if not e.quoted and kind == "unquoted expression":
                S.unquoted_expr.append((rel, e.key))
        for c in file_calls:
            S.call_files[c] += 1
        for b in file_builtins:
            S.builtin_files[b] += 1
        S.complexity.append((total_tok, n_expr, longest, rel))
        if not fs.entries:
            fs.anomaly("file has no KEY=VALUE entries", 0, "")
        if not any(e.key.upper() == "VERS" for e in fs.entries):
            fs.anomaly("no Vers key", 0, "")
        for cat, ln, det in fs.anomalies:
            all_anoms[cat].append((st, rel, ln, det))

    md = render(files, stats, all_anoms, directives, encodings, line_ending_files, global_ops)

    marker_b, marker_e = "<!-- BEGIN HUMAN SUMMARY -->", "<!-- END HUMAN SUMMARY -->"
    summary = marker_b + "\n_(human summary goes here)_\n" + marker_e
    if not args.stdout and os.path.exists(args.out):
        old = open(args.out, encoding="utf-8").read()
        if marker_b in old and marker_e in old:
            summary = old[old.index(marker_b): old.index(marker_e) + len(marker_e)]
    md = md.replace("@@SUMMARY@@", summary)
    if args.stdout:
        sys.stdout.write(md)
    else:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(md)
        print("wrote %s (%d files scanned)" % (args.out, len(files)))


# --------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------


def fmt_counter(c, limit=None, sep=", "):
    items = sorted(c.items(), key=lambda kv: (-kv[1], str(kv[0])))
    if limit:
        items = items[:limit]
    return sep.join("`%s`×%d" % (k, v) for k, v in items)


def md_escape(s):
    return str(s).replace("|", "\\|")


COSMETIC = {"leading/trailing whitespace inside quotes", "space after '='", "space between key and '='",
            "expression references a non-var key", "empty expression (\"\")"}


def render(files, stats, all_anoms, directives, encodings, line_ending_files, global_ops):
    out = []
    w = out.append
    w("# G-Force scene corpus statistics")
    w("")
    w("Generated by `python3 -I tools/scan_corpus.py` — do not hand-edit outside the summary block "
      "(it is preserved on regeneration). Counts come from a real tokenizer (comments stripped, "
      "`/* */` across lines, quoted vs unquoted values) plus a Pratt parser for the documented "
      "expression grammar.")
    w("")
    w("@@SUMMARY@@")
    w("")
    w("## Corpus")
    w("")
    w("| Type | Files | Expressions | Value kinds |")
    w("|---|---|---|---|")
    for t in TYPES:
        S = stats[t]
        w("| %s | %d | %d | %s |" % (t, S.files, sum(x[1] for x in S.complexity), fmt_counter(S.value_kinds)))
    w("")
    w("Encodings: %s" % fmt_counter(encodings))
    w("")
    w("Line endings (files): %s" % ", ".join("%s: %d" % (k, len(v)) for k, v in sorted(line_ending_files.items())))
    w("")

    # ---------------- global summaries
    all_calls = collections.Counter()
    all_call_argc = collections.defaultdict(collections.Counter)
    all_call_variants = collections.defaultdict(collections.Counter)
    all_builtins = collections.Counter()
    all_builtin_variants = collections.defaultdict(collections.Counter)
    builtin_types = collections.defaultdict(set)
    call_types = collections.defaultdict(set)
    feats = collections.Counter()
    for t in TYPES:
        S = stats[t]
        all_calls.update(S.calls)
        for k, v in S.call_argc.items():
            all_call_argc[k].update(v)
            call_types[k].add(t)
        for k, v in S.call_variants.items():
            all_call_variants[k].update(v)
        all_builtins.update(S.builtins)
        for k, v in S.builtin_variants.items():
            all_builtin_variants[k].update(v)
            builtin_types[k].add(t)
        feats.update(S.parse_features)

    w("## All functions (all types)")
    w("")
    w("| Function | Calls | Arg counts | Spellings | Types |")
    w("|---|---|---|---|---|")
    for k, v in sorted(all_calls.items(), key=lambda kv: -kv[1]):
        w("| `%s` | %d | %s | %s | %s |" % (k, v, fmt_counter(all_call_argc[k]),
                                          fmt_counter(all_call_variants[k]),
                                          ", ".join(x for x in TYPES if x in call_types[k])))
    w("")
    w("## All built-in (non user-var) identifiers (all types)")
    w("")
    w("Identifiers that are neither function calls, `A#`/`B#`/`C#`/`D#` user vars, nor keys defined in the "
      "same file. Grouped case-insensitively (G-Force identifiers are case-insensitive: `a0` == `A0`).")
    w("")
    w("| Identifier | Refs | Spellings | Types |")
    w("|---|---|---|---|")
    for k, v in sorted(all_builtins.items(), key=lambda kv: -kv[1]):
        w("| `%s` | %d | %s | %s |" % (k, v, fmt_counter(all_builtin_variants[k]),
                                     ", ".join(x for x in TYPES if x in builtin_types[k])))
    w("")
    w("## Operators / punctuation (all types)")
    w("")
    w(fmt_counter(global_ops))
    w("")
    w("Parser-observed constructs: %s" % (fmt_counter(feats) or "none"))
    w("")

    # ---------------- per-type
    for t in TYPES:
        S = stats[t]
        w("## %s (%d files)" % (t, S.files))
        w("")
        w("### Keys")
        w("")
        fam_rows = {}
        plain_rows = []
        for ku, variants in S.key_ci_variants.items():
            fam, idx = family_of(ku)
            if idx is not None:
                continue
            plain_rows.append((sum(variants.values()), ku, variants))
        plain_rows.sort(key=lambda r: (-r[0], r[1]))
        w("Non-numbered keys (files containing; spellings):")
        w("")
        w("| Key (ci) | Files | Spellings |")
        w("|---|---|---|")
        for cnt, ku, variants in plain_rows:
            w("| `%s` | %d | %s |" % (md_escape(ku), cnt, fmt_counter(variants)))
        w("")
        w("Numbered families (files containing ≥1 member; max index; spellings of members):")
        w("")
        w("| Family | Files | Max index | Max in | Lower-case spellings |")
        w("|---|---|---|---|---|")
        for fam in sorted(S.family_files, key=lambda f: (-S.family_files[f], f)):
            lowers = sorted({v for ku, vs in S.key_ci_variants.items() if family_of(ku)[0] == fam
                             for v in vs if v != ku})
            w("| `%s` | %d | %d | %s | %s |" % (fam, S.family_files[fam], S.family_max[fam],
                                              md_escape(S.family_maxfile[fam]),
                                              ", ".join("`%s`" % x for x in lowers[:8]) or "—"))
        w("")
        w("### Functions")
        w("")
        w("| Function | Calls | Files | Arg counts |")
        w("|---|---|---|---|")
        for k, v in sorted(S.calls.items(), key=lambda kv: -kv[1]):
            w("| `%s` | %d | %d | %s |" % (k, v, S.call_files[k], fmt_counter(S.call_argc[k])))
        w("")
        w("### Built-in identifiers")
        w("")
        w("| Identifier | Refs | Files | Used in keys |")
        w("|---|---|---|---|")
        for k, v in sorted(S.builtins.items(), key=lambda kv: -kv[1]):
            w("| `%s` | %d | %d | %s |" % (k, v, S.builtin_files[k], fmt_counter(S.builtin_by_key[k], 6)))
        w("")
        w("### User variable references")
        w("")
        w("By family: %s" % (fmt_counter(S.uservars) or "none"))
        w("")
        w("References to other keys defined in the file (not A–D vars): %s" % (fmt_counter(S.keyrefs) or "none"))
        w("")
        w("Stage references (defining key family → referenced var family): %s" %
          (", ".join("%s→%s×%d" % (a, b, c) for (a, b), c in sorted(S.stage_refs.items(), key=lambda kv: -kv[1]))
           or "none"))
        w("")
        if S.forward_refs:
            w("Same-family self/forward references (e.g. `A1` uses `A3`): %d occurrences in %d files; first few: %s" % (
                len(S.forward_refs), len({r[0] for r in S.forward_refs}),
                "; ".join("%s: %s→%s" % (md_escape(r[0]), r[1], r[2]) for r in S.forward_refs[:6])))
            w("")
        if S.meta:
            w("`Meta` string fields (not expressions): %s" % fmt_counter(S.meta))
            w("")
        w("### Operators")
        w("")
        w(fmt_counter(S.ops) or "none")
        w("")
        w("### Number literal formats")
        w("")
        w(", ".join("%s ×%d (e.g. `%s`)" % (k, v, S.numfmt_example[k])
                    for k, v in sorted(S.numfmt.items(), key=lambda kv: -kv[1])) or "none")
        w("")
        w("### Vers")
        w("")
        w(fmt_counter(S.vers) or "none")
        w("")
        w("### Complexity (token count across all expressions)")
        w("")
        cx = sorted(S.complexity, key=lambda r: -r[0])
        w("| | File | Expressions | Tokens | Longest expr (chars) |")
        w("|---|---|---|---|---|")
        for r in cx[:5]:
            w("| top | %s | %d | %d | %d |" % (md_escape(r[3]), r[1], r[0], r[2]))
        for r in sorted(S.complexity, key=lambda r: (r[0], r[3]))[:3]:
            w("| simplest | %s | %d | %d | %d |" % (md_escape(r[3]), r[1], r[0], r[2]))
        if cx:
            toks = sorted(r[0] for r in cx)
            w("")
            w("Median tokens/file: %d; median expressions/file: %d; longest single expression: %d chars." % (
                toks[len(toks) // 2], sorted(r[1] for r in cx)[len(cx) // 2], max(r[2] for r in cx)))
        w("")

    # ---------------- anomalies
    w("## Anomalies (all types)")
    w("")
    w("| Category | Occurrences | Files | By type |")
    w("|---|---|---|---|")
    for cat in sorted(all_anoms, key=lambda c: -len(all_anoms[c])):
        lst = all_anoms[cat]
        bt = collections.Counter(x[0] for x in lst)
        w("| %s | %d | %d | %s |" % (md_escape(cat), len(lst), len({x[1] for x in lst}),
                                    ", ".join("%s %d" % (k, v) for k, v in bt.most_common())))
    w("")
    for cat in sorted(all_anoms, key=lambda c: -len(all_anoms[c])):
        lst = all_anoms[cat]
        w("### %s (%d)" % (cat, len(lst)))
        w("")
        limit = 3 if cat in COSMETIC else 12
        for st, rel, ln, det in lst[:limit]:
            w("- `%s`%s%s" % (rel, (":%d" % ln) if ln else "", (" — `%s`" % det.replace("`", "'")) if det else ""))
        if len(lst) > limit:
            w("- … %d more" % (len(lst) - limit))
        w("")
    w("## `#` tags in comments")
    w("")
    if directives:
        for tag, lst in sorted(directives.items(), key=lambda kv: -len(kv[1])):
            bt = collections.Counter(x[0] for x in lst)
            w("- `%s` ×%d (%s)" % (tag, len(lst), ", ".join("%s %d" % kv for kv in bt.most_common())))
    else:
        w("none")
    w("")
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    main()
