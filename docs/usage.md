# Using rmpp-textobj

- **Add text object** (T tool menu, under "Add text"): tap where the text goes, type, OK. The new object is selected
  so you can move it right away. It starts at about body-text size; the size you pick in the edit box is kept for
  the next one.
- **Edit**: with the selection tool, double tap a text object with a finger to select it (or lasso it), double
  tap it again (pen or finger) to open the edit box (text, Thin / Medium / Thick, size). Selecting several objects and double tapping joins them into one.
- **Always whole**: a lasso that covers part of a text object selects all of it (at least half of its strokes
  inside) or none of it, so moving, deleting and copying act on whole objects. A pasted copy is a text object too.
  Erasing part of an object turns it into plain strokes.
- **Highlights**: a highlighter stroke on a text object is kept with the object, with the characters it covers and
  its colour: it moves and is copied with it, and editing the text draws it again on the same characters as a band
  as high as the line (text inserted before it moves it; its text deleted removes it). Erasing the band removes the
  highlight. Highlights drawn before this version come back yellow after an edit. The stroke stays as drawn until
  the next edit; snapping it to the characters right away (`snapHighlights=true` in
  `~/.config/rmpp-textobj/settings.conf`) is experimental and does not work on the device yet.
- **With handwriting**: an underline drawn on a text object goes with it (selected with it, never recognized as
  handwriting); editing the text removes it. Other handwriting stays separate: a lasso takes text objects (whole) and handwriting
  together as usual; with handwriting in the selection a double tap selects the tapped object alone, and another double tap edits it. Handwriting drawn across an object
  cannot be left out of its selection; the object can be edited once it is moved off.
- **Handwriting to text objects**: [rmpp-hwr](https://github.com/lorancecall/rmpp-hwr) recognizes lasso-selected
  handwriting and hands the text to rmpp-textobj (its text object mode).

Glyphs: every language's glyphs come from [rmpp-lang](https://github.com/lorancecall/rmpp-lang)'s language add-ons
(`glyphs.rmgl`, one file per language: Korean, Chinese, Japanese, Cyrillic, ...); this package carries ASCII and the
space only (`glyphs.json`). Text with characters that no installed glyph file has is not turned into an object: the
edit box names the characters and OK stays off until they are removed or their language is added (Settings >
Languages).

Glyphs: every language's glyphs come from [rmpp-lang](https://github.com/lorancecall/rmpp-lang)'s language add-ons
(`glyphs.rmgl`, one file per language: Korean, Chinese, Japanese, Cyrillic, ...); this package carries ASCII and the
space only (`glyphs.json`). Text with characters that no installed glyph file has is not turned into an object: the
edit box names the characters and OK stays off until they are removed or their language is added (Settings >
Languages).
