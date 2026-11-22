# rmpp-textobj

**Text objects for the reMarkable Paper Pro**: typed text drawn as ordinary pen strokes in a clean stroke font, that
you can move, resize, copy and erase like handwriting, and edit as text with a double tap.

On the tablet:

- **Add text object** (T tool menu, under "Add text"): tap where the text goes, type, OK. The new object is selected
  so you can move it right away. It starts at about body-text size; the size you pick in the edit box is kept for
  the next one.
- **Edit**: with the selection tool, double tap a text object to select it, double tap it again to open the edit box
  (text, Thin / Medium / Thick, size). Selecting several objects and double tapping joins them into one.
- **Always whole**: a lasso that covers part of a text object selects all of it (at least half of its strokes
  inside) or none of it, so moving, deleting and copying act on whole objects. A pasted copy is a text object too.
  Erasing part of an object turns it into plain strokes.
- **Handwriting to text objects**: [rmpp-hwr](https://github.com/lorancecall/rmpp-hwr) recognizes lasso-selected
  handwriting and hands the text to rmpp-textobj (its text object mode).

Glyphs: every language's glyphs come from [rmpp-lang](https://github.com/lorancecall/rmpp-lang)'s language add-ons
(`glyphs.rmgl`, one file per language: Korean, Chinese, Japanese, Cyrillic, ...); this package carries ASCII and the
space only (`glyphs.json`). Text with characters that no installed glyph file has is not turned into an object: the
edit box names the characters and OK stays off until they are removed or their language is added (Settings >
Languages).

> Status: experimental. Install with Vellum from [rmpp-packages](https://github.com/lorancecall/rmpp-packages)
> (`vellum add rmpp-textobj`); it needs `rmpp-preload` and `rmpp-lang`.

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
- `src/select.h`: the selection rules (double tap, partial lasso, erased objects).
- `src/api.h`: the C API other add-ons call (rmpp-hwr): the selected objects' text, open the edit box, draw
  recognized lines as an object.
- `tests/match_test.py`: the stroke-pattern matching ported to Python, with unit tests (`python tests/match_test.py`).

## Build

Needs the Arm GNU aarch64 toolchain and Qt 6.10 headers/device libraries in `../sdk` (as for rmpp-hwr):
`./build.sh` -> `build/librmpp-textobj-ui.so`. The ASCII glyphs: `python tools/glyphs.py build --font
NotoSansKR-Regular.ttf -o data/glyphs.json` (centre lines of Noto Sans KR; the same method rmpp-lang uses for the
language files).

## License

GPL-3.0 (see LICENSE). `data/glyphs.json`: glyph centre lines derived from Noto Sans KR, SIL Open Font License 1.1
(see NOTICE and data/OFL-NotoSansKR.txt).
