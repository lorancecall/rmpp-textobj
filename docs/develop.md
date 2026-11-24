# Developing rmpp-textobj

Build, tests and trying a build on the Paper Pro: [install.md](install.md#build-and-install-by-hand).

## Code

- `src/textobj.cpp`: loaded into xochitl by rmpp-preload (`/usr/lib/rmpp/preload.d/55-rmpp-textobj.so`, before
  rmpp-hwr's 60). Finds xochitl's QML items at run time; no xochitl headers.
- `src/glyphs.h`: glyph lookup. RMGL files are memory-mapped the first time a character is looked up (nothing at
  xochitl start) and released after two idle minutes; one binary search per character. Order: the text's language
  (kana -> ja, Hangul -> ko, Han only -> the language whose file has all of it) -> the other installed languages ->
  `glyphs.json`.
- `src/carry.h`: each object's record (text, size, weight, line boxes) rides in its own strokes, in the `speed`
  field of their points, which the Fineliner does not draw and xochitl keeps through moves, scaling, copy and paste
  and sync. So the record syncs with the page, works on another tablet, follows a copy to another page and goes away
  with the page; a stock tablet sees plain strokes. No record file is kept, and nothing is ever written next to a
  notebook (an unknown file in a document folder stops that notebook's sync: the cloud rejects it). Also a small
  reader for `.rm` v6 page files.
- `src/objects.h`: records in memory, made here or read from the page file's strokes when a page is looked at. An
  object is found on a page by its strokes' pattern, wherever it was moved and at whatever size; nothing tracks
  positions.
- `src/select.h`: the selection rules (double tap, partial lasso, erased objects) and the highlight check.
- `src/highlight.h`: highlights: which characters a highlighter stroke covers, the bands, following an edit
  (`tests/highlight_test.py` ports the geometry with unit tests).
- `src/layer.h`: the Text objects layer (made at the bottom, object mode, the layers menu):
  see [text-layer.md](text-layer.md).
- `src/api.h`: the C API other add-ons call (rmpp-hwr): the selected objects' text, open the edit box, draw
  recognized lines as an object.
- `tests/match_test.py`: the stroke-pattern matching ported to Python, with unit tests (`python tests/match_test.py`).

## Glyphs

`data/glyphs.json` (ASCII and the space) holds centre lines of Noto Sans KR, made with
`python tools/glyphs.py build --font NotoSansKR-Regular.ttf -o data/glyphs.json`, the same method rmpp-lang uses for
the language files (`glyphs.rmgl`).
