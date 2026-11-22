#!/usr/bin/env python3
"""Unit test of the text-object matching (src/objects.h findRec / findObjects, src/select.h rules), ported 1:1 to
Python so it runs on the PC without the tablet or Qt. Stroke rects are made the way xochitl reports them: the
integer bounds of a stroke's points, widened by half the pen width.

    python tests/match_test.py            (exit code 0 = all passed)

Keep this in step with src/objects.h when the matching changes.
"""
import json
import math
import random
import sys
from pathlib import Path

GLYPHS = json.loads((Path(__file__).resolve().parent.parent / "data" / "glyphs.json").read_text(encoding="utf-8"))["glyphs"]


# ---- making strokes and their rects ----
def layout(text, x0, y0, em):
    """Strokes (page units) of one line, as src/ink.h layoutLine + appendLine."""
    out, x = [], 0.0
    for ch in text:
        g = GLYPHS.get(ch)
        if not g:
            x += 0.5
            continue
        for s in g["strokes"]:
            out.append([(x0 + (s[i] + x) * em, y0 + s[i + 1] * em) for i in range(0, len(s) - 1, 2)])
        x += g["adv"] + 0.04
    return out


def stroke_rect(pts, width):
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    pad = width / 2
    l, t = math.floor(min(xs) - pad), math.floor(min(ys) - pad)
    r, b = math.ceil(max(xs) + pad), math.ceil(max(ys) + pad)
    return (l, t, r - l, b - t)


def rects_of(strokes, em, scale=1.0, dx=0.0, dy=0.0, width=None):
    w = width if width is not None else max(1.5, min(em / 20, 9.0))
    return [stroke_rect([(p[0] * scale + dx, p[1] * scale + dy) for p in s], w) for s in strokes]


# ---- port of src/objects.h ----
class Rec:
    em = 40

    def __init__(self, page, text, rects, n=None):
        bx = min(r[0] for r in rects); by = min(r[1] for r in rects)
        bx2 = max(r[0] + r[2] for r in rects); by2 = max(r[1] + r[3] for r in rects)
        self.page, self.text = page, text
        self.rects = [(r[0] - bx, r[1] - by, r[2], r[3]) for r in rects]
        self.n = len(rects)
        self.w, self.h = bx2 - bx, by2 - by


def center(r):
    return (r[0] + r[2] / 2, r[1] + r[3] / 2)


class Index:
    """Stroke rects by a coarse grid of their centres."""
    def __init__(self, rects):
        self.rects, self.grid = rects, {}
        for i, r in enumerate(rects):
            c = center(r)
            self.grid.setdefault((math.floor(c[0] / 16), math.floor(c[1] / 16)), []).append(i)

    def candidates(self, c, size, tc, ts):
        """(error, index) of the rects whose centre is within tc of c and whose size is within ts of size."""
        out = []
        for gx in range(math.floor((c[0] - tc) / 16), math.floor((c[0] + tc) / 16) + 1):
            for gy in range(math.floor((c[1] - tc) / 16), math.floor((c[1] + tc) / 16) + 1):
                for i in self.grid.get((gx, gy), []):
                    r = self.rects[i]
                    rc = center(r)
                    err = max(abs(rc[0] - c[0]), abs(rc[1] - c[1]))
                    serr = max(abs(r[2] - size[0]), abs(r[3] - size[1]))
                    if err <= tc and serr <= ts:
                        out.append((err + 0.5 * serr, i))
        return out


def scale_allowed(rec, page):
    return rec.page == page and rec.n >= 4


POS_TOL = 2.5   # page units: whole-unit rects of moved strokes round differently (anchor and stroke: 1 each)


def match_at(rec, idx, k, d, size_off, ca, loose):
    """Members of rec placed at scale k and offset d (page centre = d + k * record centre); sizes compared as
    k * size + size_off (the pen width does not scale with the strokes). Each page stroke goes to one record stroke:
    the closest pairs first."""
    pairs = []
    for ri, rr in enumerate(rec.rects):
        cr = center(rr)
        c = (d[0] + k * cr[0], d[1] + k * cr[1])
        dist = math.hypot(cr[0] - ca[0], cr[1] - ca[1])
        tiny = max(rr[2], rr[3]) <= 5   # tracing leaves clusters of tiny fragments that trade places when rounded
        tc = POS_TOL + (2.0 if tiny else 0.0) + (0.04 * k * dist if loose else 0.004 * k * max(rr[2], rr[3]))
        ts = POS_TOL + 0.06 * k * max(rr[2], rr[3]) + (4 if loose else 0)
        for err, pi in idx.candidates(c, (k * rr[2] + size_off, k * rr[3] + size_off), tc, ts):
            pairs.append((err, ri, pi))
    pairs.sort()
    members, taken = [-1] * rec.n, set()
    for err, ri, pi in pairs:
        if members[ri] < 0 and pi not in taken:
            members[ri] = pi
            taken.add(pi)
    return members, len(taken)


def fit(rec, page_rects, members):
    """Scale k and offset d of page centre = d + k * record centre, least squares over the matched strokes."""
    pr = [(center(rec.rects[i]), center(page_rects[m])) for i, m in enumerate(members) if m >= 0]
    if len(pr) < 2:
        return None
    mrx = sum(a[0][0] for a in pr) / len(pr); mry = sum(a[0][1] for a in pr) / len(pr)
    mpx = sum(a[1][0] for a in pr) / len(pr); mpy = sum(a[1][1] for a in pr) / len(pr)
    num = sum((a[0][0] - mrx) * (a[1][0] - mpx) + (a[0][1] - mry) * (a[1][1] - mpy) for a in pr)
    den = sum((a[0][0] - mrx) ** 2 + (a[0][1] - mry) ** 2 for a in pr)
    if den <= 0:
        return None
    k = num / den
    return k, (mpx - k * mrx, mpy - k * mry)


def find_rec(rec, ri, page_rects, idx, allow_scale, want_partial=True):
    full, partial = [], None
    # anchors: the three biggest strokes (an object with its biggest stroke erased is still found in part)
    anchors = sorted(range(rec.n), key=lambda i: -rec.rects[i][2] * rec.rects[i][3])[:3]
    for o, a in ((o, a) for a in anchors for o in page_rects):
        ar = rec.rects[a]
        ca = center(ar)
        if allow_scale:
            k = (o[2] + o[3]) / (ar[2] + ar[3])
            if k < 0.3 or k > 4:
                continue
            if min(ar[2], ar[3], o[2], o[3]) > 6 and abs((o[2] / o[3]) / (ar[2] / ar[3]) - 1) > 0.3:
                continue
        else:
            if abs(o[2] - ar[2]) > POS_TOL or abs(o[3] - ar[3]) > POS_TOL:
                continue
            k = 1.0
        co = center(o)
        d = (co[0] - k * ca[0], co[1] - k * ca[1])
        if any(p["rec"] == ri and abs(p["d"][0] - d[0]) + abs(p["d"][1] - d[1]) < 6 for p in full):
            continue
        scaled = abs(k - 1) > 0.02
        # quick check before matching every stroke: one of the other anchor strokes must be there too
        others = [b for b in anchors if b != a]
        present = (any if want_partial else all)
        if others and not present(idx.candidates(
                (d[0] + k * center(rec.rects[b])[0], d[1] + k * center(rec.rects[b])[1]),
                (k * rec.rects[b][2], k * rec.rects[b][3]),
                POS_TOL + (0.04 * k * math.hypot(center(rec.rects[b])[0] - ca[0], center(rec.rects[b])[1] - ca[1]) if scaled else 0),
                POS_TOL + 0.06 * k * max(rec.rects[b][2], rec.rects[b][3]) + (4 if scaled else 0)) for b in others):
            continue
        members, matched = match_at(rec, idx, k, d, 0.0, ca, scaled)
        if scaled and matched * 2 >= rec.n:
            # fit k and d to the matched centres, drop the strokes far off the fit, fit again; then the pen-width
            # size offset, and match again strictly
            for _ in range(2):
                f = fit(rec, page_rects, members)
                if not f:
                    break
                k, d = f
                for i, m in enumerate(members):
                    if m >= 0:
                        cr, cp = center(rec.rects[i]), center(page_rects[m])
                        if max(abs(d[0] + k * cr[0] - cp[0]), abs(d[1] + k * cr[1] - cp[1])) > POS_TOL + 1:
                            members[i] = -1
            offs = sorted(page_rects[m][2] - k * rec.rects[i][2] for i, m in enumerate(members) if m >= 0)
            if offs:
                members, matched = match_at(rec, idx, k, d, offs[len(offs) // 2], ca, False)
        p = {"rec": ri, "k": k, "d": d, "members": members, "matched": matched,
             "box": (d[0], d[1], rec.w * k, rec.h * k)}
        if matched == rec.n:
            full.append(p)
        elif partial is None or matched > partial["matched"]:
            partial = p
    return full, partial


def find_objects(recs, page_rects, page, other_pages=True):
    idx = Index(page_rects)
    found = []
    for i, r in enumerate(recs):
        if r.n > len(page_rects):
            continue
        if r.page != page and (not other_pages or r.n < 3):
            continue
        found += find_rec(r, i, page_rects, idx, scale_allowed(r, page), want_partial=False)[0]
    found.sort(key=lambda p: (-p["matched"], -p["rec"]))
    used, keep = set(), []
    for p in found:
        # sharing a few strokes (tiny fragments of overlapping objects) is fine; a real part of it is not
        if sum(1 for m in p["members"] if m in used) > max(2, p["matched"] // 5):
            continue
        used.update(p["members"])
        keep.append(p)
    return keep


# ---- port of src/select.h rules ----
def lasso_rule(found, selected_idx):
    add, drop = [], []
    for p in found:
        inn = sum(1 for m in p["members"] if m in selected_idx)
        if inn == 0 or inn == p["matched"]:
            continue
        if 2 * inn >= p["matched"]:
            add += [m for m in p["members"] if m not in selected_idx]
        else:
            drop += [m for m in p["members"] if m in selected_idx]
    return add, drop


def erased(rec, ri, page_rects, page):
    full, partial = find_rec(rec, ri, page_rects, Index(page_rects), scale_allowed(rec, page))
    return not full and partial is not None and max(2, (rec.n + 1) // 2) <= partial["matched"] < rec.n


def is_mark(recs, page_rects, p, i):
    """Port of select.h isMark: a highlight / underline on object p."""
    if i in p["members"]:
        return False
    r = page_rects[i]
    em = recs[p["rec"]].em * p["k"]
    if r[2] < 1.5 * em or r[2] < 1.5 * r[3]:
        return False
    bx, by, bw, bh = p["box"]
    m = 0.25 * em
    ix = max(0, min(r[0] + r[2], bx + bw + m) - max(r[0], bx - m))
    iy = max(0, min(r[1] + r[3], by + bh + m) - max(r[1], by - m))
    return ix * iy >= 0.8 * r[2] * r[3]


def selected_objects(recs, page_rects, found, sel_idx):
    """Port of select.h selectedObjects: (objects, only, other, marks)."""
    out, covered = [], set()
    for p in found:
        if all(m in sel_idx for m in p["members"]):
            out.append(p)
            covered.update(p["members"])
    rest, marks = [], []
    for i in sel_idx:
        if i in covered:
            continue
        (marks if any(is_mark(recs, page_rects, p, i) for p in out) else rest).append(i)
    return out, bool(out) and not rest, rest, marks


# ---- tests ----
FAILS = []


def check(name, cond):
    print(("ok   " if cond else "FAIL ") + name)
    if not cond:
        FAILS.append(name)


def handwriting(rng, n, area):
    """Random 'handwriting' stroke rects in an area."""
    out = []
    for _ in range(n):
        w, h = rng.randint(3, 60), rng.randint(3, 60)
        out.append((rng.randint(area[0], area[2]), rng.randint(area[1], area[3]), w, h))
    return out


def main():
    rng = random.Random(7)
    em = 40
    text = "Hello world 42"
    strokes = layout(text, -500, 200, em)
    made = rects_of(strokes, em)
    rec = Rec("P", text, made)
    other = handwriting(rng, 400, (-800, 0, 800, 2600))

    # 1. found where it was made, among handwriting
    page = other + made
    f = find_objects([rec], page, "P")
    check("found in place among 400 handwriting strokes", len(f) == 1 and f[0]["matched"] == rec.n)

    # 2. moved (fractional offset: rects round differently)
    moved = rects_of(strokes, em, dx=313.4, dy=-120.7)
    f = find_objects([rec], other + moved, "P")
    check("found after a move", len(f) == 1 and abs(f[0]["d"][0] - (rec_box(made)[0] + 313.4)) < 3)
    moved_far = rects_of(strokes, em, dx=-41.5, dy=1913.5)
    check("found after another move", len(find_objects([rec], other + moved_far, "P")) == 1)

    # 3. scaled 1.5x (pen width scaled too, and not scaled)
    for w in (None, max(1.5, em / 20)):
        sc = rects_of(strokes, em, scale=1.5, dx=200, dy=500, width=None if w is None else w)
        if w is None:
            sc = rects_of(strokes, em, scale=1.5, dx=200, dy=500, width=max(1.5, min(em * 1.5 / 20, 9.0)))
        f = find_objects([rec], other + sc, "P")
        check(f"found scaled 1.5x (pen width {'scaled' if w is None else 'kept'})", len(f) == 1 and abs(f[0]["k"] - 1.5) < 0.08)

    # 4. a pasted copy: both found
    copy = rects_of(strokes, em, dx=0, dy=700)
    f = find_objects([rec], other + made + copy, "P")
    check("original and pasted copy both found", len(f) == 2)

    # 5. pasted onto another page (same size): found; scaled there: not (tiny records never)
    f = find_objects([rec], other + copy, "Q")
    check("copy on another page found", len(f) == 1)
    f = find_objects([rec], other + rects_of(strokes, em, scale=1.4, dy=300), "Q")
    check("scaled copy on another page not found (no scaling across pages)", len(f) == 0)

    # 6. a short record does not turn handwriting into an object
    dash = Rec("P", "-", rects_of(layout("-", 0, 0, em), em))
    hw = [(10, 10, dash.rects[0][2], dash.rects[0][3])] + other
    f = find_objects([dash], hw, "P")
    check("one-stroke record matches only at its own size", all(abs(p["k"] - 1) < 0.03 for p in f))
    f = find_objects([dash], [(10, 10, dash.rects[0][2] * 2, dash.rects[0][3] * 2)], "P")
    check("one-stroke record never matches a scaled stroke", len(f) == 0)

    # 7. part of a bigger object: the bigger record wins
    small = Rec("P", "Hello", rects_of(layout("Hello", -500, 200, em), em))
    f = find_objects([rec, small], page, "P")
    check("'Hello' record inside the 'Hello world 42' object: only the big one", len(f) == 1 and f[0]["rec"] == 0)
    f = find_objects([small, rec], page, "P")   # newer small record: still the big one
    check("same when the small record is newer", len(f) == 1 and f[0]["rec"] == 1)

    # 8. lasso rule: more than half -> add the rest; less -> drop
    f = find_objects([rec], page, "P")
    members = f[0]["members"]
    most = set(members[: int(len(members) * 0.7)])
    add, drop = lasso_rule(f, most)
    check("lasso over 70%: the rest is added", sorted(add) == sorted(set(members) - most) and not drop)
    few = set(members[:3])
    add, drop = lasso_rule(f, few)
    check("lasso over 3 strokes: they are taken out", sorted(drop) == sorted(few) and not add)
    half = set(members[: (len(members) + 1) // 2])
    add, drop = lasso_rule(f, half)
    check("lasso over half: the rest is added", add and not drop)
    whole = set(members)
    check("lasso over all of it: unchanged", lasso_rule(f, whole) == ([], []))

    # 9. erased in part -> plain strokes; erased in full -> record kept (undo)
    gone = made[:4] + made[5:]
    check("one stroke erased: record dropped", erased(rec, 0, other + gone, "P"))
    split = list(made)
    big = max(range(len(made)), key=lambda i: made[i][2])
    l, t, w, h = split[big]
    split[big:big + 1] = [(l, t, w // 2 - 2, h), (l + w // 2 + 2, t, w - w // 2 - 2, h)]   # the eraser cut a stroke in two
    check("one stroke cut by the eraser: record dropped", erased(rec, 0, other + split, "P"))
    check("whole object deleted: record kept", not erased(rec, 0, other, "P"))
    check("intact object: record kept", not erased(rec, 0, page, "P"))


    # 11. a highlight over the object: still found; object + highlight = only objects; other handwriting is not
    bx, by = rec_box(made)
    hl = (bx - 4, by + 6, rec.w + 8, int(em * 0.7))                  # highlighter band over the line
    under = (bx, by + rec.h + 2, int(rec.w * 0.8), 6)                # underline just below it
    note = (bx + 40, by + 10, 18, 22)                                # a handwritten letter on it
    page2 = other + made + [hl, under, note]
    f = find_objects([rec], page2, "P")
    check("highlighted object still found", len(f) == 1)
    hi, ui, ni = len(page2) - 3, len(page2) - 2, len(page2) - 1
    objs, only, rest, marks = selected_objects([rec], page2, f, set(f[0]["members"]) | {hi, ui})
    check("object + highlight + underline: only objects (2 marks)", only and sorted(marks) == [hi, ui])
    objs, only, rest, marks = selected_objects([rec], page2, f, set(f[0]["members"]) | {hi, ni})
    check("object + handwritten letter on it: not only objects", not only and rest == [ni])
    far = (bx + rec.w + 300, by, 200, 30)                            # a wide stroke elsewhere
    page3 = page2 + [far]
    objs, only, rest, marks = selected_objects([rec], page3, f, set(f[0]["members"]) | {len(page3) - 1})
    check("object + a wide stroke elsewhere: not a mark", not only)

    # 10. timing: a page with 2,000 strokes and 50 objects
    import time
    objs, all_rects, recs = [], list(handwriting(rng, 1500, (-800, 0, 800, 2600))), []
    for i in range(50):
        t = f"object {i} text"
        s = layout(t, rng.randint(-800, 300), rng.randint(0, 2500), rng.choice([30, 40, 60]))
        rr = rects_of(s, 40)
        recs.append(Rec("P", t, rr))
        all_rects += rr
    t0 = time.time()
    f = find_objects(recs, all_rects, "P")
    dt = time.time() - t0
    check(f"50 objects among 2,000 strokes all found ({len(f)}; {dt * 1000:.0f} ms in Python)", len(f) == 50)

    print(f"\n{len(FAILS)} failed" if FAILS else "\nall passed")
    return 1 if FAILS else 0


def rec_box(rects):
    return (min(r[0] for r in rects), min(r[1] for r in rects))


if __name__ == "__main__":
    sys.exit(main())
