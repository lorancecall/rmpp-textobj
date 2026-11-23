#!/usr/bin/env python3
"""Unit test of the converted object's size (src/ink.h snapEm / placeLines), ported to Python so it runs on the PC.
One size per object: the longest line's handwriting size, snapped to kEmSteps.

    python tests/size_test.py             (exit code 0 = all passed)

Keep this in step with src/ink.h.
"""
import math
import sys

STEPS = [28, 40, 52, 68, 88, 116]


def snap(h):
    return min(STEPS, key=lambda s: abs(math.log(h / s)))


def object_em(lines):
    """lines: (text, handwriting em) -> the object's em"""
    best = max(lines, key=lambda l: (len(l[0].strip()), l[1]))
    return snap(best[1])


CASES = [
    # sizes seen on the device for one-line objects (the em the old code used): all land on a few steps
    ([("하하허", 40.0)], 40),
    ([("test 하면 걸과가 어떻게 나올가?", 56.8)], 52),
    ([("안녕", 40.2)], 40),
    ([("어퍼시오.", 48.2)], 52),
    ([("폰트사이즈는?", 41.5)], 40),
    ([("안녕?", 51.0)], 52),
    # a stray dot's line does not set the size
    ([(".", 3.3), ("왜일 까?", 54.0)], 52),
    # the longest line sets it
    ([("1~", 27.7), ("to", 29.4), ("TT", 63.6), ("hello there", 30)], 28),
    ([("x", 300)], 116),
    ([("x", 5)], 28),
]

fails = 0
for lines, want in CASES:
    got = object_em(lines)
    if got != want:
        fails += 1
        print(f"FAIL {lines}: {got} (want {want})")
print(f"{len(CASES) - fails}/{len(CASES)} passed")
sys.exit(1 if fails else 0)
