# Installing rmpp-textobj

The usual way is **rmpp setup** from [rmpp-packages](https://github.com/lorancecall/rmpp-packages#install), as the
desktop app or the script: tick **Text objects (beta)**, or **Handwriting recognition (beta)**, which brings it.
This page covers the rest. Steps that are the same for every rmpp add-on live in rmpp-packages and are linked here,
not repeated.

## Packages

| Package | Installs with it |
|---|---|
| `rmpp-textobj` | `rmpp-lang`, `rmpp-preload` |

`rmpp-hwr` installs `rmpp-textobj` too (its text object mode). The glyphs of each language come with that
language's rmpp-lang add-on (`rmpp-lang-ko`, `-ja`, `-zh`, `-ru`, `-bg`); rmpp-textobj itself has ASCII only.
Dependencies are installed automatically, in rmpp setup and with `vellum add`. All packages and what needs what:
[packages.md](https://github.com/lorancecall/rmpp-packages/blob/main/docs/packages.md).

## Without rmpp setup

Follow [manual-install.md](https://github.com/lorancecall/rmpp-packages/blob/main/docs/manual-install.md) (Vellum and
the rmpp package repository), then on the Paper Pro:

```sh
vellum add rmpp-textobj        # with rmpp-lang, rmpp-preload
vellum add rmpp-lang-ko        # the languages whose glyphs you need: rmpp-lang-ja, -zh, -ru, -bg
```

## Uninstall

- **Just this add-on:** untick it in rmpp setup and press **Remove unticked installed**, or
  `vellum del rmpp-textobj`. It stays while `rmpp-hwr` is installed (rmpp-hwr needs it).
- **What stays:** text objects already on your pages (as ordinary pen strokes), their records in
  `/home/root/.local/share/rmpp-textobj/objects.json` and the settings in `/home/root/.config/rmpp-textobj/`. Delete
  these by hand to start fresh, or use step 3 of
  [uninstall.md](https://github.com/lorancecall/rmpp-packages/blob/main/docs/uninstall.md).
- **Everything rmpp, and before a factory reset:** [uninstall.md](https://github.com/lorancecall/rmpp-packages/blob/main/docs/uninstall.md).

## Next to other mods

It runs next to xovi and other Vellum packages: [how-it-works.md](https://github.com/lorancecall/rmpp-packages/blob/main/docs/how-it-works.md#next-to-other-mods).

## Build and install by hand

For development. Cross-compiled with the Arm GNU aarch64 toolchain against Qt 6.10 headers and the Paper Pro's Qt
libraries (`SDK=../sdk` by default):

```sh
./build.sh                       # -> build/librmpp-textobj-ui.so
python tests/match_test.py       # tests on the PC
python tests/highlight_test.py
```

The ASCII glyphs: `python tools/glyphs.py build --font NotoSansKR-Regular.ttf -o data/glyphs.json`.

Install the package first (above, or rmpp setup): it sets up everything around the library (preload link). Then
replace the library with your build:

```sh
scp build/librmpp-textobj-ui.so rmpp:/home/root/
ssh rmpp 'mount -o remount,rw / &&
  cp /home/root/librmpp-textobj-ui.so /usr/lib/rmpp/rmpp-textobj/librmpp-textobj-ui.so.new &&
  mv -f /usr/lib/rmpp/rmpp-textobj/librmpp-textobj-ui.so.new /usr/lib/rmpp/rmpp-textobj/librmpp-textobj-ui.so &&
  sync && mount -o remount,ro / ;
  systemctl reset-failed xochitl; systemctl restart xochitl'
```

The package recipe (VELBUILD) is in
[packaging/](../packaging/) (rmpp-packages builds and publishes them from there); the next `vellum upgrade`
or reinstall puts the released files back.

On the Paper Pro, never run `systemctl stop xochitl` (it powers the Paper Pro off); run `systemctl reset-failed xochitl`
before `systemctl restart xochitl` (repeated restarts otherwise hit systemd's limit). The root file system is
read-only: `mount -o remount,rw /` before changing files under `/usr/lib/rmpp`, `mount -o remount,ro /` after.
