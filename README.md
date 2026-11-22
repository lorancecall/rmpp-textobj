# rmpp-textobj

**Text objects for the reMarkable Paper Pro**: typed text drawn as ordinary pen strokes in a clean stroke font, that
you can move, resize, copy and erase like handwriting, and edit as text with a double tap.

> Part of the **rmpp add-ons**, built on [rmpp-packages](https://github.com/lorancecall/rmpp-packages): it is
> installed by rmpp setup, loaded into xochitl at every boot by rmpp-preload (no xovi), and kept up to date with
> Vellum. How that differs from xovi: [how it works](https://github.com/lorancecall/rmpp-packages/blob/main/docs/how-it-works.md).
>
> Status: experimental.

On the tablet:

- **Add text object** (T tool menu, under "Add text"): tap where the text goes, type, OK. The new object is selected
  so you can move it right away. It starts at about body-text size; the size you pick in the edit box is kept for
  the next one.
- **Edit**: with the selection tool, double tap a text object to select it, double tap it again to open the edit box
  (text, Thin / Medium / Thick, size). Selecting several objects and double tapping joins them into one.
- **Always whole**: a lasso that covers part of a text object selects all of it (at least half of its strokes
  inside) or none of it, so moving, deleting and copying act on whole objects. A pasted copy is a text object too.
  Erasing part of an object turns it into plain strokes.
- **Highlights**: the highlighter on a text object snaps to its characters as on typed text: the stroke is replaced
  by a band as high as the line and as wide as the characters it covers, in the same colour. The highlight is kept
  with the object: it moves and is copied with it, and editing the text draws it again on the same characters
  (text inserted before it moves it; its text deleted removes it). Erasing the band removes the highlight.
  Highlights drawn before this version come back yellow after an edit. Snapping can be turned off with
  `snapHighlights=false` in `~/.config/rmpp-textobj/settings.conf` (the highlight is still kept).
- **With handwriting**: an underline drawn on a text object goes with it (selected with it, never recognized as
  handwriting); editing the text removes it. Other handwriting stays separate: a lasso takes text objects (whole) and handwriting together as usual; with handwriting in the selection
  a double tap selects the tapped object alone, and another double tap edits it. Handwriting drawn across an object
  cannot be left out of its selection; the object can be edited once it is moved off.
- **Handwriting to text objects**: [rmpp-hwr](https://github.com/lorancecall/rmpp-hwr) recognizes lasso-selected
  handwriting and hands the text to rmpp-textobj (its text object mode).

Glyphs: every language's glyphs come from [rmpp-lang](https://github.com/lorancecall/rmpp-lang)'s language add-ons
(`glyphs.rmgl`, one file per language: Korean, Chinese, Japanese, Cyrillic, ...); this package carries ASCII and the
space only (`glyphs.json`). Text with characters that no installed glyph file has is not turned into an object: the
edit box names the characters and OK stays off until they are removed or their language is added (Settings >
Languages).

## Install

- Install **rmpp setup** (desktop app or one-line script) from
  [rmpp-packages](https://github.com/lorancecall/rmpp-packages#install) and tick **rmpp-textobj** (not preselected),
  with the **rmpp-lang** language add-ons whose glyphs you need (Korean, Japanese, Chinese, ...). Developer mode is
  needed: the rmpp-packages page explains it.
- After a reMarkable OS update: **Restore** in rmpp setup.
- Other ways (Vellum on the command line, building and installing it yourself) and removal:
  [docs/install.md](docs/install.md).

## How it works

- `src/textobj.cpp`: loaded into xochitl by rmpp-preload (`/usr/lib/rmpp/preload.d/55-rmpp-textobj.so`, before
  rmpp-hwr's 60). Finds xochitl's QML items at run time; no xochitl headers.
- `src/glyphs.h`: glyph lookup. RMGL files are memory-mapped the first time a character is looked up (nothing at
  xochitl start) and released after two idle minutes; one binary search per character. Order: the text's language
  (kana -> ja, Hangul -> ko, Han only -> the language whose file has all of it) -> the other installed languages ->
  `glyphs.json`.
- `src/objects.h`: one record per object in `/home/root/.local/share/rmpp-textobj/objects.json` (text, size, and the
  bounds of each of its strokes). An object is found on a page by its strokes' pattern, wherever it was moved and at
  whatever size; nothing tracks positions. Records written by rmpp-hwr 0.2-0.4 (`~/.local/share/rmpp-hwr/ink.json`)
  are taken over once; their strokes are learned the first time each such object is selected.
- `src/select.h`: the selection rules (double tap, partial lasso, erased objects) and the highlight check.
- `src/highlight.h`: highlights: which characters a highlighter stroke covers, the bands, following an edit
  (`tests/highlight_test.py` ports the geometry with unit tests).
- `src/api.h`: the C API other add-ons call (rmpp-hwr): the selected objects' text, open the edit box, draw
  recognized lines as an object.
- `tests/match_test.py`: the stroke-pattern matching ported to Python, with unit tests (`python tests/match_test.py`).

## Build

`./build.sh` (Arm GNU aarch64 toolchain and Qt 6.10 in `../sdk`); the ASCII glyphs are centre lines of Noto Sans KR
(`tools/glyphs.py`, the same method rmpp-lang uses for the language files). Details, tests and trying a build on the
tablet: [docs/install.md](docs/install.md#building-and-installing-it-yourself-developers).

## License

GPL-3.0 (see LICENSE). `data/glyphs.json`: glyph centre lines derived from Noto Sans KR, SIL Open Font License 1.1
(see NOTICE and data/OFL-NotoSansKR.txt).
