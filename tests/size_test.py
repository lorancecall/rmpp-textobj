#!/usr/bin/env python3
"""Unit test of the converted object's size (src/ink.h snapEm / placeLines), ported to Python so it runs on the PC.
One size per object: the longest line's handwriting size, snapped to kSizeSteps (relative to xochitl's body text).

    python tests/size_test.py             (exit code 0 = all passed)

Keep this in step with src/ink.h.
"""
import math
import sys

BODY = 31  # xochitl's body text in em (src/ink.h kBodyEm), = 100%


def snap(h):
    r = h / BODY
    step = 0.7 if r < 0.6 else 1.0 if r < 1.6 else 1.5 if r < 2.4 else 2.0 if r < 3.4 else 3.0
    return round(BODY * step, 3)


def object_em(lines):
    """lines: (text, handwriting em) -> the object's em"""
    best = max(lines, key=lambda l: (len(l[0].strip()), l[1]))
    return snap(best[1])


B = BODY
CASES = [
    # sizes seen on the device for one-line objects (the em the old code used): ordinary handwriting -> 100%
    ([("하하허", 40.0)], B),
    ([("test 하면 걸과가 어떻게 나올가?", 56.8)], B * 1.5),
    ([("안녕", 40.2)], B),
    ([("어퍼시오.", 48.2)], B),
    ([("폰트사이즈는?", 41.5)], B),
    ([("안녕?", 51.0)], B * 1.5),
    # a stray dot's line does not set the size
    ([(".", 3.3), ("왜일 까?", 44.0)], B),
    # the longest line sets it
    ([("1~", 27.7), ("to", 29.4), ("TT", 63.6), ("hello there", 30)], B),
    # clearly big or small handwriting takes another step
    ([("x", 300)], B * 3),
    ([("x", 80)], B * 2),
    ([("x", 15)], B * 0.7),
]
CASES = [(l, round(w, 3)) for l, w in CASES]

fails = 0
for lines, want in CASES:
    got = object_em(lines)
    if got != want:
        fails += 1
        print(f"FAIL {lines}: {got} (want {want})")
print(f"{len(CASES) - fails}/{len(CASES)} passed")
sys.exit(1 if fails else 0)
