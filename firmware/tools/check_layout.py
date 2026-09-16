"""Check every screen's controls for overlap, reach, and text that will not fit.

The device has no console and no way to assert against a rendered frame, so
layout faults get found by looking at it - and the worst ones were invisible
that way. A full-width row and two buttons once shared a y: they drew on top of
each other, and because rows are hit-tested first, tapping BATTERY locked the
device instead. Before that, a menu's rows and its actions drifted apart and
every entry ran its neighbour's job for days.

Both are geometry, and geometry can be checked without hardware.

What it reports:
  OVERLAP        two tappable controls sharing space. Whichever is tested first
                 wins and the other is unreachable.
  UNDER ...      a control hidden beneath the header or the tab bar.
  OFF THE PANEL  a control running past an edge.
  TEXT TOO WIDE  a literal label wider than the space it is drawn into.

What it will not do is guess. Anything placed inside a loop, or from a value it
cannot evaluate, is listed as unchecked rather than assumed correct - an
earlier version silently checked 11 controls out of 35 and printed "0
problems", which is the most dangerous output a checker can produce.

Overlaps between controls in opposite branches of an if/else are reported too.
Those are safe, and the message says to confirm it rather than assuming.
"""

import pathlib
import re
import sys

W, H = 480, 480
CONSTS = {
    "LCD_WIDTH": W, "LCD_HEIGHT": H,
    "UI_PAD": 16, "UI_HEADER_H": 72, "UI_TABBAR_H": 72,
    "CX": W // 2, "CY": H // 2,
    "TXT_SMALL": 2, "TXT_BODY": 3, "TXT_TITLE": 4, "TXT_BIG": 6, "TXT_HUGE": 8,
    "LOGO_W": 152, "LOGO_H": 54,
    "LOCK_MAX_LEN": 12,
    # How many rows each list shows. Runtime values, but fixed where they are
    # set, and lists are the controls most likely to run off the bottom.
    "notePager.perPage": 4,
    "taskPager.perPage": 5,
    "wifiPager.perPage": 3,
}

SRC = pathlib.Path(__file__).resolve().parent.parent / "src" / "main.cpp"

TABBED = {"screenRecord", "screenNotes", "screenNote", "screenTasks",
          "screenWifi", "screenMore"}


class Rect:
    def __init__(self, x, y, w, h, line, what):
        self.x, self.y, self.w, self.h = int(x), int(y), int(w), int(h)
        self.line, self.what = line, what

    def hits(self, o):
        return (self.x < o.x + o.w and o.x < self.x + self.w
                and self.y < o.y + o.h and o.y < self.y + self.h)

    def __repr__(self):
        return "%s(%d,%d %dx%d)" % (self.what, self.x, self.y, self.w, self.h)


def strip_comments(text):
    """Blank out comments, preserving line structure.

    A comment containing an unmatched bracket - and this file is full of prose
    that does - desynchronises the statement joiner for the rest of the
    function, after which every position looks unknowable. That is exactly how
    the More screen came back as "position not constant" for all nine rows."""
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text[i:j].count("\n"))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text[i] in "\"'":
            q = text[i]
            j = i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:min(j + 1, n)])
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def split_args(s):
    out, depth, cur, instr = [], 0, "", None
    for ch in s:
        if instr:
            cur += ch
            if ch == instr:
                instr = None
            continue
        if ch in "\"'":
            instr = ch
            cur += ch
        elif ch in "([":
            depth += 1
            cur += ch
        elif ch in ")]":
            depth -= 1
            cur += ch
        elif ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def evaluate(expr, env):
    e = expr.strip()
    if not e:
        return None
    m = re.match(r"^([^?()]*)\?(.*):(.*)$", e)
    if m:
        a, b = evaluate(m.group(2), env), evaluate(m.group(3), env)
        if a is not None and b is not None:
            return max(a, b)
        return None
    for name, val in list(env.items()) + list(CONSTS.items()):
        e = re.sub(r"\b%s\b" % re.escape(name), str(val), e)
    if not re.fullmatch(r"[-+*/%()\d\s]+", e):
        return None
    try:
        return int(eval(e, {"__builtins__": {}}, {}))
    except Exception:
        return None


def screens(text):
    for m in re.finditer(r"static void (screen[A-Za-z]+)\(const UiTap& t\) \{", text):
        start, depth, i = m.end(), 1, m.end()
        while i < len(text) and depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        yield m.group(1), text[start:i - 1], text[:m.start()].count("\n") + 1


def analyse(body, base_line):
    env, rects, texts, unchecked = {}, [], [], []
    depth = 0
    loop_depth = None
    loop_iters = None
    loop_reason = "inside a loop"

    joined, buf, paren = [], "", 0
    for raw in strip_comments(body).splitlines():
        buf = (buf + " " + raw.strip()).strip() if buf else raw.strip()
        paren += raw.count("(") - raw.count(")")
        if paren <= 0:
            joined.append(buf)
            buf, paren = "", 0
        else:
            joined.append("")

    for n, line in enumerate(joined):
        lineno = base_line + n + 1
        if not line:
            continue
        opened_loop = bool(re.match(r"(for|while)\s*\(", line))

        if loop_depth is None and not opened_loop:
            dm = re.search(r"(?:const\s+)?int\s+(.+?);", line)
            if dm and "(" not in dm.group(1).split("=")[0]:
                for part in dm.group(1).split(","):
                    pm = re.match(r"\s*(\w+)\s*=\s*(.+)", part)
                    if pm:
                        v = evaluate(pm.group(2), env)
                        if v is not None:
                            env[pm.group(1)] = v
            for am in re.finditer(r"\b(\w+)\s*\+=\s*([^;]+);", line):
                v = evaluate(am.group(2), env)
                if v is not None and am.group(1) in env:
                    env[am.group(1)] += v

        # A loop with a constant count is unrolled, so the rows and buttons it
        # places are checked like any other. Rows drawn in a loop are where the
        # lists live, and a list running under the tab bar is exactly the kind
        # of fault nobody sees until the last item is unreachable.
        if opened_loop:
            fm = re.match(r"for \(int (\w+) = (\d+); \1 < ([\w.()]+); \1\+\+\)", line)
            cnt = evaluate(fm.group(3), env) if fm else None
            loop_iters = ([{fm.group(1): k} for k in range(cnt)]
                          if fm and cnt is not None and cnt <= 64 else None)
            if loop_iters is None:
                loop_reason = "loop count not constant"
        iters = loop_iters if (loop_depth is not None or opened_loop) else [dict()]

        for call in ("uiRow", "uiButton"):
            cm = re.search(call + r"\(t,\s*(.+)\)", line)
            if not cm:
                continue
            if iters is None:
                unchecked.append((lineno, loop_reason, line[:58]))
                continue
            args = split_args(cm.group(1))
            failed = False
            for it in iters:
                e = dict(env)
                e.update(it)
                if call == "uiRow" and len(args) >= 3:
                    y, h = evaluate(args[0], e), evaluate(args[1], e)
                    if y is None or h is None:
                        failed = True
                        break
                    label = args[2].strip('" ')[:18]
                    rects.append(Rect(CONSTS["UI_PAD"], y, W - 2 * CONSTS["UI_PAD"], h,
                                      lineno, "row " + label))
                elif call == "uiButton" and len(args) >= 5:
                    v = [evaluate(a, e) for a in args[:4]]
                    if None in v:
                        failed = True
                        break
                    label = args[4].strip('" ')[:18]
                    rects.append(Rect(v[0], v[1], v[2], v[3], lineno,
                                      "button " + label))
                    if args[4].startswith('"'):
                        size = 3 if v[3] >= 56 else 2
                        texts.append((lineno, args[4].strip('"'), size, v[2] - 16,
                                      "button label"))
            if failed:
                unchecked.append((lineno, "position not constant", line[:58]))

        # uiPagerBar lives in pala_ui.cpp and places two 96x44 buttons at the y
        # it is given. Without modelling it, a pager sitting on top of the last
        # row of its own list goes unnoticed.
        pm2 = re.search(r"uiPagerBar\(t,\s*\w+,\s*([^)]+)\)", line)
        if pm2:
            py = evaluate(pm2.group(1), env)
            if py is None:
                unchecked.append((lineno, "pager position not constant", line[:58]))
            else:
                rects.append(Rect(CONSTS["UI_PAD"], py, 96, 44, lineno, "pager <"))
                rects.append(Rect(W - CONSTS["UI_PAD"] - 96, py, 96, 44, lineno, "pager >"))

        tm = re.search(r'dispTextCentered\(\s*[^,]+,\s*"([^"]*)"\s*,\s*(TXT_\w+)', line)
        if tm:
            texts.append((lineno, tm.group(1), CONSTS[tm.group(2)], W - 8, "centred"))
        d2 = re.search(r'dispText\(\s*([^,]+),\s*[^,]+,\s*"([^"]*)"\s*,\s*(TXT_\w+)', line)
        if d2:
            x = evaluate(d2.group(1), env)
            if x is not None:
                texts.append((lineno, d2.group(2), CONSTS[d2.group(3)],
                              W - x - 4, "at x=%d" % x))

        if opened_loop and loop_depth is None:
            loop_depth = depth
        depth += line.count("{") - line.count("}")
        if loop_depth is not None and depth <= loop_depth:
            loop_depth = None

    return rects, texts, unchecked


def main():
    text = SRC.read_text(encoding="utf-8")
    problems = controls = 0
    for name, body, line in screens(text):
        rects, texts, unchecked = analyse(body, line)
        controls += len(rects)
        issues = []

        for i, a in enumerate(rects):
            for b in rects[i + 1:]:
                if a.hits(b):
                    issues.append("  OVERLAP  %s (line %d) and %s (line %d)"
                                  " - confirm these are in opposite branches"
                                  % (a, a.line, b, b.line))
        limit = H - CONSTS["UI_TABBAR_H"] if name in TABBED else H
        for r in rects:
            if r.y + r.h > limit:
                issues.append("  UNDER THE %s  %s (line %d) reaches %d, limit %d"
                              % ("TAB BAR" if name in TABBED else "EDGE",
                                 r, r.line, r.y + r.h, limit))
            if r.x < 0 or r.x + r.w > W:
                issues.append("  OFF THE PANEL  %s (line %d)" % (r, r.line))
        for lineno, s, size, room, how in texts:
            if not size or room is None:
                continue
            need = 6 * size * len(s)
            if need > room:
                issues.append("  TEXT TOO WIDE  line %d (%s): %r needs %dpx, has %d"
                              % (lineno, how, s, need, room))

        if issues or unchecked:
            print("%s  (%d controls checked)" % (name, len(rects)))
            for i in issues:
                print(i)
                problems += 1
            for lineno, why, src in unchecked:
                print("  unchecked line %d (%s): %s" % (lineno, why, src))
    print()
    print("%d tappable controls checked, %d problem(s)" % (controls, problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
