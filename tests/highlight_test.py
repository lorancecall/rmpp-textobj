#!/usr/bin/env python3
"""Unit test of the highlight geometry (src/highlight.h), ported 1:1 to Python so it runs on the PC without the
tablet or Qt: which characters a highlighter stroke covers, where the snapped bands go, and how a highlight follows
an edit of the text.

    python tests/highlight_test.py        (exit code 0 = all passed)

Keep this in step with src/highlight.h.
"""
import json
import sys
from pathlib import Path

GLYPHS = json.loads((Path(__file__).resolve().parent.parent / "data" / "glyphs.json").read_text(encoding="utf-8"))["glyphs"]

LINE_GAP = 1.15      # lines are 1.15 em apart (src/ink.h placeText)
BAND = 0.95          # band height, em (the em box, a little inside)


def spans(line):
    """(x0, x1) in em of each character of one line, as src/ink.h layoutLine places them."""
    out, x = [], 0.0
    for ch in line:
        g = GLYPHS.get(ch)
        if not g:
            out.append((x, x + 0.5))
            x += 0.5
            continue
        out.append((x, x + g["adv"]))
        x += g["adv"] + 0.04
    return out


def line_starts(text, lines):
    """Offset of each visual line in text (lines are the drawn lines, in order: wrapping drops a space or splits a
    word, so each one is found after the previous)."""
    out, pos = [], 0
    for ln in lines:
        i = text.find(ln, pos) if ln else pos
        if i < 0:
            i = pos
        out.append(i)
        pos = i + len(ln)
    return out


def uniform(lines, origin, em):
    """Line boxes (text, x, y, em: page units, top-left of the em box) of text placed by placeText: one em size,
    lines 1.15 em apart."""
    return [(t, origin[0], origin[1] + i * LINE_GAP * em, em) for i, t in enumerate(lines)]


def range_from_rect(text, lines, r):
    """Characters [a, b) of text a highlighter stroke with bounds r (page units) covers, on an object with the line
    boxes lines. The line: the one the stroke overlaps most (at least 40% of its em box height); the characters: at
    least half of their width inside the stroke. None if none."""
    best, best_ov = -1, 0.0
    for i, (t, x, y, em) in enumerate(lines):
        ov = min(r[1] + r[3], y + em) - max(r[1], y)
        if ov >= 0.4 * em and ov > best_ov:
            best, best_ov = i, ov
    if best < 0:
        return None
    t, x, y, em = lines[best]
    a = b = -1
    for j, (x0, x1) in enumerate(spans(t)):
        if t[j].isspace():
            continue
        px0, px1 = x + x0 * em, x + x1 * em
        if min(px1, r[0] + r[2]) - max(px0, r[0]) >= 0.5 * (px1 - px0):
            if a < 0:
                a = j
            b = j + 1
    if a < 0:
        return None
    s = line_starts(text, [ln[0] for ln in lines])[best]
    return s + a, s + b


def bands(text, lines, a, b):
    """The bands of highlight [a, b): one per visual line it touches, leading and trailing spaces left out. Each is
    the rect of its ink (page units): the characters' width, BAND em high around the middle of the em box. (It is
    drawn as a straight highlighter stroke as thick as the band, from half the thickness inside each end.)"""
    out = []
    for s, (t, x, y, em) in zip(line_starts(text, [ln[0] for ln in lines]), lines):
        la, lb = max(a, s) - s, min(b, s + len(t)) - s
        while la < lb and t[la].isspace():
            la += 1
        while lb > la and t[lb - 1].isspace():
            lb -= 1
        if la >= lb:
            continue
        sp = spans(t)
        h = BAND * em
        out.append((x + sp[la][0] * em, y + 0.5 * em - h / 2, (sp[lb - 1][1] - sp[la][0]) * em, h))
    return out


def map_range(old, new, a, b):
    """Highlight [a, b) of old text moved onto new text: the common start and end of the two texts keep their
    highlight; the changed middle part is highlighted if the highlight covered it (in part). None when nothing is
    left."""
    p = 0
    while p < len(old) and p < len(new) and old[p] == new[p]:
        p += 1
    s = 0
    while s < len(old) - p and s < len(new) - p and old[-1 - s] == new[-1 - s]:
        s += 1
    oe, ne = len(old) - s, len(new) - s     # changed: old[p:oe] -> new[p:ne]

    def pos(x, end):
        if x <= p:
            return x
        if x >= oe:
            return x - oe + ne
        return ne if end else p             # inside the change: to its end (end of a range) / start
    na, nb = pos(a, False), pos(b, True)
    while na < nb and new[na].isspace():
        na += 1
    while nb > na and new[nb - 1].isspace():
        nb -= 1
    return (na, nb) if na < nb else None


# ---- tests ----
def stroke_over(text, lines, a, b, slack=0.15):
    """A freehand highlighter stroke over characters [a, b): a little off in every direction."""
    l, t, w, h = bands(text, lines, a, b)[0]
    em = h / BAND
    return (l - slack * em, t - 0.05 * em, w + 2 * slack * em, 1.0 * em)


TESTS = []


def test(f):
    TESTS.append(f)
    return f


@test
def stroke_picks_the_word():
    text = "hello world"
    lines = uniform([text], (100.0, 200.0), 40.0)
    got = range_from_rect(text, lines, stroke_over(text, lines, 6, 11))
    assert got == (6, 11), got


@test
def short_stroke_takes_mostly_covered_characters():
    text, em = "hello world", 40.0
    sp = spans(text)
    # from the middle of "w" (half of it) to a third into "d" (not half)
    l = sp[6][0] * em + 0.45 * (sp[6][1] - sp[6][0]) * em
    r_ = sp[10][0] * em + 0.3 * (sp[10][1] - sp[10][0]) * em
    got = range_from_rect(text, uniform([text], (0.0, 0.0), em), (l, 5, r_ - l, 30))
    assert got == (6, 10), got


@test
def leading_space_not_highlighted():
    text, em = "hello world", 40.0
    sp = spans(text)
    got = range_from_rect(text, uniform([text], (0.0, 0.0), em), (sp[4][1] * em + 1, 5, (sp[8][1] - sp[4][1]) * em, 30))
    assert got == (6, 9), got


@test
def second_wrapped_line():
    text = "the quick brown fox jumps"
    texts = ["the quick", "brown fox", "jumps"]
    assert line_starts(text, texts) == [0, 10, 20]
    lines = uniform(texts, (50.0, 50.0), 30.0)
    r = stroke_over(text, lines, 16, 19)   # "fox", on the second line
    assert r[1] > 50 + 30
    got = range_from_rect(text, lines, r)
    assert got == (16, 19), got


@test
def stroke_between_lines_goes_to_the_closer():
    lines = uniform(["ab", "cd"], (0.0, 0.0), 40.0)
    # mostly on line 2 (y 46..86), a bit on line 1
    got = range_from_rect("ab\ncd", lines, (0, 30, 60, 46))
    assert got == (3, 5), got


@test
def stroke_off_the_text():
    assert range_from_rect("abc", uniform(["abc"], (0.0, 0.0), 40.0), (0, 100, 80, 30)) is None


@test
def lines_of_their_own_size():
    # rmpp-hwr's recognized lines: each at the size and place of its handwriting
    text = "big\nsmall"
    lines = [("big", 0.0, 0.0, 60.0), ("small", 20.0, 80.0, 30.0)]
    got = range_from_rect(text, lines, stroke_over(text, lines, 4, 9))
    assert got == (4, 9), got
    (l, t, w, h), = bands(text, lines, 4, 9)
    assert abs(h - BAND * 30) < 1e-9 and abs(t + h / 2 - 95) < 1e-9 and abs(l - 20) < 1e-9


@test
def bands_over_two_lines():
    text = "one two\nthree four"
    texts = ["one two", "three four"]
    bs = bands(text, uniform(texts, (0.0, 0.0), 40.0), 4, 13)   # "two\nthree"
    assert len(bs) == 2, bs
    assert abs(bs[0][1] + bs[0][3] / 2 - 20) < 1e-9 and abs(bs[1][1] + bs[1][3] / 2 - 66) < 1e-9, bs
    sp0, sp1 = spans(texts[0]), spans(texts[1])
    assert abs(bs[0][0] - sp0[4][0] * 40) < 1e-9 and abs(bs[0][0] + bs[0][2] - sp0[6][1] * 40) < 1e-9
    assert abs(bs[1][0]) < 1e-9 and abs(bs[1][2] - sp1[4][1] * 40) < 1e-9


@test
def band_round_trip():
    text = "Hello, 세계"
    lines = uniform([text], (10.0, 10.0), 36.0)
    for a, b in [(0, 5), (7, 9), (2, 3)]:
        (r,) = bands(text, lines, a, b)
        assert range_from_rect(text, lines, r) == (a, b), (a, b)


@test
def edit_before_shifts():
    assert map_range("hello world", "oh hello world", 6, 11) == (9, 14)


@test
def edit_after_keeps():
    assert map_range("hello world", "hello world!!", 0, 5) == (0, 5)


@test
def edit_inside_keeps_the_rest():
    # "world" -> "wonderful world": the change sits inside the highlight
    assert map_range("hello world", "hello wonderful world", 6, 11) == (6, 21)
    # one letter changed in the highlight
    assert map_range("big cat sat", "big car sat", 4, 7) == (4, 7)


@test
def edit_removes_highlighted_text():
    assert map_range("hello world", "hello", 6, 11) is None
    assert map_range("a world b", "a b", 2, 7) is None


@test
def edit_partly_removes():
    # "hello world" highlighted "lo wo" (3..8); "lo w" deleted -> "helorld": "o" left
    got = map_range("hello world", "helorld", 3, 8)
    assert got == (3, 4), got


@test
def edit_next_to_highlight_does_not_spread():
    # typing right after the highlighted word: the new text is not highlighted (the common start ends at the word)
    assert map_range("red", "red blue", 0, 3) == (0, 3)
    # replacing the text after a highlight
    assert map_range("red green", "red blue", 0, 3) == (0, 3)


def main():
    failed = 0
    for t in TESTS:
        try:
            t()
            print("ok  ", t.__name__)
        except AssertionError as e:
            failed += 1
            print("FAIL", t.__name__, e)
    print(f"{len(TESTS) - failed}/{len(TESTS)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
