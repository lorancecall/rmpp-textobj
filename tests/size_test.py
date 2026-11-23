#!/usr/bin/env python3
"""Unit test of the converted object's size (src/ink.h snapEm / placeLines), ported to Python so it runs on the PC.
One size per object: the longest line's handwriting size, snapped to the nearest of kSizeSteps (relative to xochitl's body text).

    python tests/size_test.py             (exit code 0 = all passed)

Keep this in step with src/ink.h.
"""
import math
import sys

BODY = 31  # xochitl's body text in em (src/ink.h kBodyEm), = 100%


STEPS = [0.7, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0]


def snap(h):
    best = min(STEPS, key=lambda st: abs(math.log(h / (BODY * st))))
    return round(BODY * best, 3)


def object_em(lines):
    """lines: (text, handwriting em) -> the object's em"""
    best = max(lines, key=lambda l: (len(l[0].strip()), l[1]))
    return snap(best[1])


B = BODY
CASES = [
    # sizes seen on the device for one-line objects (the em the old code used): they follow the handwriting
    ([("하하허", 40.0)], B * 1.25),
    ([("test 하면 걸과가 어떻게 나올가?", 56.8)], B * 1.75),
    ([("안녕", 40.2)], B * 1.25),
    ([("어퍼시오.", 48.2)], B * 1.5),
    ([("폰트사이즈는?", 41.5)], B * 1.25),
    ([("안녕?", 51.0)], B * 1.75),
    # handwriting about the size of typed text -> 100%
    ([("작게", 31.0)], B),
    ([("작게", 34.0)], B),
    # a stray dot's line does not set the size
    ([(".", 3.3), ("왜일 까?", 44.0)], B * 1.5),
    # the longest line sets it
    ([("1~", 27.7), ("to", 29.4), ("TT", 63.6), ("hello there", 30)], B),
    # far out: the end steps
    ([("x", 300)], B * 3),
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
