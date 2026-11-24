#!/usr/bin/env python3
"""Device test steps for rmpp-textobj on a reMarkable Paper Pro (developer tool, not shipped).

Drives the tablet with rmpp-studio's rmpp_ctl.py (tap, text, pen strokes through rmpp-remote) and reads the add-on's
log. Everything happens on one notebook named "scratch"; the tool refuses to draw anywhere else.

  device.py deploy "<note>"        build/librmpp-textobj-ui.so to the tablet, scratch cleared, xochitl restarted
  device.py open                   close any notebook, open scratch (page 1)
  device.py object "Text" X Y      a new text object: Text tool, tap at X,Y, type, convert, back to writing
  device.py highlight X1 X2 Y      a wavy highlighter stroke from X1 to X2 around line Y (highlighter picked first)
  device.py tool pen|highlighter|eraser|select|text
  device.py erase X1 X2 Y          an eraser stroke along Y
  device.py undo | redo
  device.py dtap X Y               a finger double tap
  device.py drag X1 Y1 X2 Y2       a pen drag (moves a selection with the selection tool)
  device.py text "Text"            type into the focused field
  device.py key ctrl+a             a key (rmpp_ctl key)
  device.py shot NAME              screenshot to $RMPP_SHOTS/NAME.png
  device.py log [SECONDS]          the add-on's log lines of the last SECONDS (default 30)
  device.py run STEPS              one step per line (same words as above), "wait MS" allowed; "#" comments
  device.py test [NAME...]         run tests/device/NAME.steps (all when none given) and print PASS/FAIL per test

Checks (in a steps file; a failed check is reported and the test goes on):
  expect-log REGEX                 an add-on log line since the test started matches (waits up to 10 s)
  expect-no-log REGEX              no add-on log line since the test started matches
  expect-color X Y COLOR           the screen around X,Y is COLOR: white, ink (dark), green, yellow, blue, pink, gray,
                                   or R,G,B
  expect-ink X1 Y1 X2 Y2 [COLOR]   at least 20 pixels of COLOR (default ink: dark) in the rectangle
  expect-none X1 Y1 X2 Y2 [COLOR]  fewer than 5 such pixels
  mark                             later expect-log / expect-no-log look only at lines after this point

Environment: RMPP_HOST (default rmpp), RMPP_CTL (default ../rmpp-studio/rmpp_ctl.py), RMPP_SHOTS (default
build/shots), RMPP_LOCK (tablet lock owner file; default ../.sessions/tablet.lock/owner).
Coordinates are screen pixels (portrait 1620 x 2160), as in screenshots.
"""
import glob
import os
import re
import shlex
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
HOST = os.environ.get("RMPP_HOST", "rmpp")
CTL = os.environ.get("RMPP_CTL", os.path.join(REPO, "..", "rmpp-studio", "rmpp_ctl.py"))
SHOTS = os.environ.get("RMPP_SHOTS", os.path.join(REPO, "build", "shots"))
LOCK = os.environ.get("RMPP_LOCK", os.path.join(REPO, "..", ".sessions", "tablet.lock", "owner"))
DOCS = "/home/root/.local/share/remarkable/xochitl"
LIB = "/usr/lib/rmpp/rmpp-textobj/librmpp-textobj-ui.so"

# left toolbar (toolbar shown), screen pixels
TOOLS = {"pen": (56, 167), "highlighter": (56, 279), "text": (56, 392), "eraser": (56, 504), "select": (56, 616),
         "layers": (56, 728), "undo": (56, 840), "redo": (56, 952)}


COLORS = {"white": (255, 255, 255), "ink": (0, 0, 0), "green": (172, 255, 133), "yellow": (255, 242, 117),
          "blue": (143, 209, 255), "pink": (255, 168, 214), "gray": (200, 200, 200)}

STATE = {"mark": 0, "fails": [], "checks": 0}


def ctl(*args, check=True, stdin=None):
    r = subprocess.run([sys.executable, CTL, *map(str, args)], capture_output=True, text=True, input=stdin,
                       env=dict(os.environ, RMPP_HOST=HOST))
    if check and r.returncode != 0:
        sys.exit(f"rmpp_ctl {' '.join(map(str, args))}: {r.stderr.strip() or r.stdout.strip()}")
    return r.stdout


def ssh(cmd, check=True):
    r = subprocess.run(["ssh", "-o", "ConnectTimeout=8", HOST, cmd], capture_output=True, text=True)
    if check and r.returncode != 0:
        sys.exit(f"ssh: {cmd[:80]}: {r.stderr.strip()}")
    return r.stdout


def wait(ms):
    time.sleep(ms / 1000)


def doc_open():
    return "ToolButton" in ctl("find", "#hide-show-toolbar-button", check=False)


def close_docs():
    for _ in range(3):
        if not doc_open():
            return
        ctl("tap", "#close-documentview-button", check=False)
        wait(3000)
    if doc_open():
        sys.exit("a notebook is still open")


def scratch_id():
    """The id of the one notebook named "scratch" (refuses none or several)."""
    out = ssh(f"cd {DOCS} && grep -l '\"visibleName\": \"scratch\"' *.metadata", check=False).split()
    ids = [f[:-len(".metadata")] for f in out]
    if len(ids) != 1:
        sys.exit(f"expected one notebook named scratch, found {len(ids)}")
    return ids[0]


def lock_note(text):
    try:
        with open(LOCK, "w", encoding="utf-8") as f:
            f.write(f"convert-to-ink | {text} | {time.strftime('%H:%M', time.gmtime())}Z\n")
    except OSError:
        pass


def step_open():
    close_docs()
    wait(1000)
    # the library item (list or grid view; rmpp_ctl's open knows the grid only)
    for line in ctl("find", '"scratch"', check=False).splitlines():
        m = re.match(r"\s*(-?\d+),(-?\d+) (\d+)x(\d+) Content(List|Grid)Item", line)
        if m:
            x, y, w, h = map(int, m.groups()[:4])
            ctl("tap", x + w // 4, y + h // 2)
            break
    else:
        sys.exit("no notebook named scratch in My files")
    wait(4000)
    if not doc_open():
        sys.exit("scratch did not open")
    # the toolbar is needed for the tool buttons
    if "ToolButton" not in ctl("find", "#close-documentview-button", check=False):
        ctl("tap", 56, 56)
        wait(1500)
    # every test starts on the writing layer: a page can open on the Text objects layer, and a tap on no object there
    # goes back to writing (on the writing layer the tap does nothing)
    ctl("tap", 1300, 1950)
    wait(1500)


def step_tool(name):
    x, y = TOOLS[name]
    ctl("tap", x, y)
    wait(1500)


def step_object(text, x, y):
    # once more if the add did not start: the first taps after a notebook opened were sometimes not taken
    # (device 2026-11-24: the pen stayed on)
    for attempt in range(2):
        since = device_now()
        step_tool("text")
        ctl("tap", 296, 504)          # the tool's option: a text object
        wait(1500)
        ctl("tap", x, y)
        wait(2500)
        if any("add text object at view" in l for l in log_since(since)):
            break
        print("  (add text object did not start: again)", flush=True)
    ctl("text", text)
    wait(1500)
    ctl("tap", 1470, 766)         # convert
    wait(9000)                    # a tap while the object is being put is ignored
    ctl("tap", 1300, 1700)        # off the object: writing again
    wait(2000)


def wavy(x1, x2, y, amp=7, n=8):
    pts = []
    for i in range(n + 1):
        x = x1 + (x2 - x1) * i / n
        pts.append(f"{int(x)},{int(y + (amp if i % 2 else -amp))}")
    return pts


def pen_stroke(pts):
    """One pen stroke through pts ("X,Y" strings). The pen hovers at the start first: the first injected stroke after a
    restart otherwise began where the pen had last been seen (a line across the page, device 2026-11-24)."""
    x0, y0 = pts[0].split(",")
    script = ["pen out", "sleep 100", f"pen hover {x0} {y0}", "sleep 150", f"pen hover {x0} {y0}", "sleep 150",
              f"pen hover {x0} {y0}", "sleep 100",
              f"stroke 0.5 1200 : {' '.join(pts)}", "pen out"]
    ctl("input", stdin="\n".join(script) + "\n")


def step_highlight(x1, x2, y):
    step_tool("highlighter")
    pen_stroke(wavy(int(x1), int(x2), int(y)))
    wait(5000)


def step_erase(x1, x2, y):
    step_tool("eraser")
    pen_stroke(wavy(int(x1), int(x2), int(y), amp=4, n=6))
    wait(4000)


def step_dtap(x, y):
    """A finger double tap."""
    ctl("input", stdin=f"tap {x} {y} 50\nsleep 120\ntap {x} {y} 50\n")
    wait(2000)


def step_drag(x1, y1, x2, y2, n=12):
    """A pen drag (pressed) from x1,y1 to x2,y2: moves a selection with the selection tool."""
    script = [f"pen hover {x1} {y1}", "sleep 150", f"pen down {x1} {y1} 0.5", "sleep 200"]
    for i in range(1, n + 1):
        script += [f"pen move {x1 + (x2 - x1) * i // n} {y1 + (y2 - y1) * i // n} 0.5", "sleep 30"]
    script += ["sleep 200", "pen up", "pen out"]
    ctl("input", stdin="\n".join(script) + "\n")
    wait(2500)


def step_shot(name):
    os.makedirs(SHOTS, exist_ok=True)
    path = os.path.join(SHOTS, name + ".png")
    ctl("shot", path)
    print(path)


def step_log(seconds=30):
    out = ssh(f'journalctl -u xochitl --since "-{int(seconds)}s" --no-pager -o cat | grep -F "[rmpp-textobj]" '
              f'| grep -v "rect check"', check=False)
    print(out.rstrip())


def device_now():
    return int(ssh("date +%s").strip())


def log_since(mark):
    out = ssh(f'journalctl -u xochitl --since "@{mark}" --no-pager -o cat | grep -F "[rmpp-textobj]" '
              f'| grep -v "rect check"', check=False)
    return out.splitlines()


def check(ok, what):
    STATE["checks"] += 1
    print(("  ok   " if ok else "  FAIL ") + what, flush=True)
    if not ok:
        STATE["fails"].append(what)


def expect_log(regex, present=True):
    rx = re.compile(regex)
    deadline = time.time() + (10 if present else 0)
    while True:
        hit = [l for l in log_since(STATE["mark"]) if rx.search(l)]
        if hit or time.time() >= deadline:
            break
        wait(1500)
    if present:
        check(bool(hit), f'log "{regex}"' + (f": {hit[-1].strip()[:110]}" if hit else ""))
    else:
        check(not hit, f'no log "{regex}"' + (f": {hit[0].strip()[:110]}" if hit else ""))


def expect_color(x, y, color):
    from PIL import Image   # Pillow, on the PC
    want = COLORS[color] if color in COLORS else tuple(int(v) for v in color.split(","))
    os.makedirs(SHOTS, exist_ok=True)
    path = os.path.join(SHOTS, "_check.png")
    ctl("shot", path)
    im = Image.open(path).convert("RGB")
    px = [im.getpixel((x + dx, y + dy)) for dx in range(-2, 3) for dy in range(-2, 3)]
    got = tuple(sum(p[i] for p in px) // len(px) for i in range(3))
    check(max(abs(a - b) for a, b in zip(got, want)) <= 40, f"colour at {x},{y} is {color} (got {got})")


def region_count(x1, y1, x2, y2, color):
    """Pixels in the rectangle near COLOR ("ink": dark) on a fresh screenshot."""
    from PIL import Image
    os.makedirs(SHOTS, exist_ok=True)
    path = os.path.join(SHOTS, "_check.png")
    ctl("shot", path)
    im = Image.open(path).convert("RGB").crop((x1, y1, x2, y2))
    if color == "ink":
        return sum(1 for p in im.getdata() if sum(p) < 300)
    want = COLORS[color] if color in COLORS else tuple(int(v) for v in color.split(","))
    return sum(1 for p in im.getdata() if max(abs(a - b) for a, b in zip(p, want)) <= 40)


def expect_region(x1, y1, x2, y2, color="ink", present=True):
    n = region_count(x1, y1, x2, y2, color)
    ok = n >= 20 if present else n < 5
    check(ok, f"{color} {'in' if present else 'not in'} {x1},{y1}-{x2},{y2} ({n} px)")


def step_deploy(note):
    so = os.path.join(REPO, "build", "librmpp-textobj-ui.so")
    lock_note(f"#3 deploy + restart: {note}")
    close_docs()
    sid = scratch_id()
    subprocess.run(["scp", "-q", so, f"{HOST}:/tmp/textobj.new"], check=True)
    # scratch's pages go with the restart (fresh test objects each round); the notebook itself stays
    print(ssh(f"set -e; grep -q '\"visibleName\": \"scratch\"' {DOCS}/{sid}.metadata; "
              f"mount -o remount,rw /; cp /tmp/textobj.new {LIB}.new; mv {LIB}.new {LIB}; rm -f /tmp/textobj.new; "
              f"sha256sum {LIB} | cut -c1-8; rm -f {DOCS}/{sid}/*.rm").strip())
    log = os.path.join(REPO, "..", ".sessions", "restarts.log")
    with open(log, "a", encoding="utf-8") as f:
        f.write(f"{time.strftime('%H:%M', time.gmtime())}Z convert-to-ink test build: rmpp-textobj ({note}); "
                f"scratch pages cleared; notebook closed first\n")
    ssh("systemctl reset-failed xochitl; (nohup sh /usr/lib/rmpp/restart >/dev/null 2>&1 &)")
    wait(5000)
    for _ in range(40):
        if subprocess.run(["ssh", "-o", "ConnectTimeout=5", HOST,
                           'systemctl is-active -q xochitl && journalctl -u xochitl --since "-60s" --no-pager '
                           '| grep -q "rmpp-textobj\\] loaded"'], capture_output=True).returncode == 0:
            break
        wait(3000)
    print(ssh("mount -o remount,ro / && echo RO; systemctl is-active xochitl").strip())
    # the pen seen once out of the page after the restart: the first injected stroke otherwise began at a stale point
    ctl("input", stdin="pen hover 1500 2050\nsleep 200\npen hover 1510 2060\nsleep 200\npen out\n", check=False)
    lock_note(f"#3 {note} on device, test")


def run_step(words):
    cmd, args = words[0], words[1:]
    if cmd == "open":
        step_open()
    elif cmd == "object":
        step_object(args[0], int(args[1]), int(args[2]))
    elif cmd == "highlight":
        step_highlight(*map(int, args))
    elif cmd == "erase":
        step_erase(*map(int, args))
    elif cmd == "tool":
        step_tool(args[0])
    elif cmd in ("undo", "redo"):
        step_tool(cmd)
    elif cmd == "tap":
        ctl("tap", *args)
        wait(1500)
    elif cmd == "dtap":
        step_dtap(int(args[0]), int(args[1]))
    elif cmd == "drag":
        step_drag(*map(int, args))
    elif cmd == "text":
        ctl("text", args[0])
        wait(1000)
    elif cmd == "key":
        ctl("key", args[0])
        wait(800)
    elif cmd == "swipe":
        ctl("swipe", *args)
        wait(2500)
    elif cmd == "wait":
        wait(int(args[0]))
    elif cmd == "shot":
        step_shot(args[0])
    elif cmd == "log":
        step_log(*(int(a) for a in args))
    elif cmd == "mark":
        STATE["mark"] = device_now()
    elif cmd == "expect-log":
        expect_log(args[0])
    elif cmd == "expect-no-log":
        expect_log(args[0], present=False)
    elif cmd == "expect-color":
        expect_color(int(args[0]), int(args[1]), args[2])
    elif cmd in ("expect-ink", "expect-none"):
        expect_region(*map(int, args[:4]), *(args[4:5] or ["ink"]), present=cmd == "expect-ink")
    elif cmd == "deploy":
        step_deploy(" ".join(args) or "test build")
    else:
        sys.exit(f"unknown step: {cmd}")


def run_file(path):
    STATE["mark"] = device_now()
    STATE["fails"] = []
    STATE["checks"] = 0
    for line in open(path, encoding="utf-8"):
        words = shlex.split(line, comments=True)
        if words:
            print("::", " ".join(words), flush=True)
            run_step(words)
    return STATE["checks"], list(STATE["fails"])


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    if sys.argv[1] == "run":
        checks, fails = run_file(sys.argv[2])
        sys.exit(1 if fails else 0)
    elif sys.argv[1] == "test":
        names = sys.argv[2:] or sorted(os.path.basename(p)[:-6] for p in glob.glob(os.path.join(HERE, "device", "*.steps")))
        results = []
        for n in names:
            print(f"== {n}", flush=True)
            results.append((n, *run_file(os.path.join(HERE, "device", n + ".steps"))))
        print()
        for n, checks, fails in results:
            print(f"{n:16} {'PASS' if not fails else 'FAIL'} ({checks - len(fails)}/{checks})"
                  + (f": {fails[0]}" if fails else ""))
        sys.exit(1 if any(f for _, _, f in results) else 0)
    else:
        run_step(sys.argv[1:])


if __name__ == "__main__":
    main()
