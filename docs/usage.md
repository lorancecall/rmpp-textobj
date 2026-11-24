# Using rmpp-textobj

- **Add text object** (T tool menu, under "Add text"): tap where the text goes, type, OK. The new object is selected
  so you can move it right away. It starts at about body-text size; the size you pick in the edit box is kept for
  the next one.
- **Edit**: double tap a text object with a finger: it is selected and the edit box opens (text, Thin / Medium /
  Thick, size); Cancel leaves it selected, to move it. A lasso selection of objects opens the box with a double tap
  on it (pen or finger); selecting several objects and double tapping joins them into one.
- **Always whole**: a lasso that covers part of a text object selects all of it (at least half of its strokes
  inside) or none of it, so moving, deleting and copying act on whole objects. A pasted copy is a text object too.
  Erasing part of an object turns it into plain strokes.
- **Highlights**: with the highlighter's Snap to text on, a highlighter stroke over a text object becomes a straight
  band over the characters it covers, in its colour, on the Text objects layer with the object: it is selected,
  moved, copied and deleted with it, and editing the text draws it again on the same characters (text inserted
  before it moves it; its text deleted removes it). One undo takes the band back.
- **Highlights in the edit box**: the Highlight button works as bold does in a word processor: on, what you type is
  highlighted; with characters selected, it turns their highlight on or off. The square next to it picks the colour.
  The preview shows the highlights as they will be drawn. (The eraser on your writing layer does not reach the
  bands; take a highlight off in the edit box.) Highlights drawn before this version come back yellow after an edit.
- **With handwriting**: an underline drawn on a text object goes with it (selected with it, never recognized as
  handwriting); editing the text removes it. Other handwriting stays separate: a lasso takes text objects (whole) and handwriting
  together as usual; with handwriting in the selection a double tap selects the tapped object alone and edits it. Handwriting drawn across an object
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
