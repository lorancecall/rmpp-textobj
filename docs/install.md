# Installing rmpp-textobj: other ways and removal

The usual way is **rmpp setup** (see the README's [Install](../README.md#install)). This page is for Vellum on the
command line and for building and installing it yourself.

rmpp-textobj is one Vellum package, `rmpp-textobj`. It needs `rmpp-preload` (loads it into xochitl) and `rmpp-lang`
(its language add-ons carry the glyphs of every language; rmpp-textobj itself has ASCII only).

Files: the package's data (`glyphs.json`) is in `/home/root/.vellum/share/rmpp-textobj/`; the UI library goes to
`/usr/lib/rmpp/rmpp-textobj/` and is loaded through `/usr/lib/rmpp/preload.d/55-rmpp-textobj.so`. The text object
records are in `/home/root/.local/share/rmpp-textobj/objects.json` and the settings in
`/home/root/.config/rmpp-textobj/settings.conf`; the package never deletes them.

## With Vellum on the command line

On the tablet (SSH, developer mode), if Vellum is not there yet:

```sh
wget -qO- https://lorancecall.github.io/rmpp-packages/install.sh | sh
```

Then:

```sh
vellum add rmpp-textobj rmpp-lang-ko     # the language add-ons whose glyphs you need: rmpp-lang-ja, -zh, -ru, -bg
vellum upgrade                           # later: updates
vellum reenable                          # after a reMarkable OS update
```

## Build and install by hand

Building needs the Arm GNU aarch64 toolchain and Qt 6.10 headers/device libraries in `../sdk` (as for rmpp-hwr):
`./build.sh` -> `build/librmpp-textobj-ui.so`. The ASCII glyphs: `python tools/glyphs.py build --font
NotoSansKR-Regular.ttf -o data/glyphs.json`. Tests (on the PC): `python tests/match_test.py` and
`python tests/highlight_test.py`.

To try a build on a tablet that has the package: the root file system is read-only, so

```sh
mount -o remount,rw /
cp librmpp-textobj-ui.so /usr/lib/rmpp/rmpp-textobj/librmpp-textobj-ui.so.new
mv /usr/lib/rmpp/rmpp-textobj/librmpp-textobj-ui.so.new /usr/lib/rmpp/rmpp-textobj/librmpp-textobj-ui.so
systemctl reset-failed xochitl && systemctl restart xochitl
mount -o remount,ro /
```

Never `systemctl stop xochitl` (the screen stays blank until the next restart). `vellum upgrade` puts the
package's own file back.

## Removal

- `vellum del rmpp-textobj`. Text objects already on your pages stay as ordinary pen strokes; the records in
  `/home/root/.local/share/rmpp-textobj/` stay too (delete the folder to remove them).
- Everything the rmpp add-ons installed: **Remove everything** in rmpp setup.
