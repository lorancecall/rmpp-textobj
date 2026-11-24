# The Text objects layer

How text objects live on a layer of their own, why each part is the way it is, and what went wrong on the way
(issue #3). Code: `src/layer.h` (the layer, object mode, the layers menu), `src/edit.h` (conversion, undo/redo
chaining), `src/select.h` (double tap).

## What we have to work with

**Layers.** A page has the user's layers (Layer 1, ...) and, from the first conversion, the **Text objects** layer
(T), made by us at the bottom, as xochitl's own Text row is shown below the layers. xochitl's Text (typed text) and
Template rows are not scene layers: they are not counted in `layerCount`. L below is the layer the user writes on (the
topmost layer that is not T).

**SceneController calls used** (OS 3.29.1): `addLayer()` (adds on top, makes it current), `setLayerName(int, QString)`,
`moveLayer(int, int)` (signature looked up and logged), `setCurrentLayer(int)`, `layerName(int)`, `layerCount`,
`currentLayer`; `cloneSelectedItems(int, double)`, `getItemBoundingRect(...)`,
`cloneAddAndSelectItems(int layer, items, scale, centre)` (adds items to any layer in one step),
`deleteSelectedItems(int)`, `clearSelectedItems()`, `addSelectionRect(QRect, mode)`,
`getLineBoundingRectsToBeSelected(QRect)` (strokes of the **current layer only**), `undo()`, `redo()`,
`undoAvailable`, `redoAvailable`. Signals: `aboutToUndo()`, `aboutToRedo()` (before every undo or redo, whatever
started it: toolbar button, two- or three-finger tap, our own calls), `currentLayerChanged()`.

**What is an undo step.** Every one of these is a step of its own: `addLayer`, `setLayerName`, `moveLayer`,
`setCurrentLayer`, `cloneAddAndSelectItems`, `deleteSelectedItems`, each `addDrawingLine`, a move or resize of a
selection. Not steps: making or ending a selection. Any new step clears the redo history. Undo/redo calls take effect
a moment after they are made, in the order they were made, and an `undo()` right after a selection ends is ignored.

**What xochitl does not give us.** No undo grouping (a transaction does not group strokes), no way to merge, remove
or inspect steps, no way to see another layer's strokes without making it current, no selection on a layer that is
not current (dropped within a second), no lasso path (only its result), no way to veto a step.

**Stock behaviours that matter.**
- Ink under typed text is anchored to its characters: it moves when the text grows and is deleted when they are
  (backspacing through the newlines a typed-text conversion padded with). Our objects are anchored like any ink;
  decided: objects follow typed text as ink does.
- A double tap on typed text selects a word there (xochitl takes it).
- The page file stores the current layer; a page closed while T is current opens on T.

## Probed facts that shape the design (2026-11-23)

- `aboutToUndo`/`aboutToRedo` are emitted inside xochitl's `undo()`/`redo()`, before the step. The step is usually
  applied inside the call as well, but once it came about 15 ms later, behind a pending edit. Calls are applied in
  order.
- With fast presses (buttons and gestures), the page seen at each `aboutToUndo`/`aboutToRedo` already includes every
  earlier press.
- Layer calls are applied later, not inside the call (`addLayer` then `layerCount - 1` in the same tick still names the
  old index). A layer change ends the selection.
- No call reads another layer's strokes.
- The history survives a page turn; closing a notebook clears it.
- `penHandler.strokeCompleted` fires for every pen stroke (and lasso). A move or resize is not a step when the pen
  lifts: `pendingEditChanged` carries its transform, and the step (move and resize together) is committed when the
  selection ends.

## Design

**T is made current only by the user's own action**, and everything is read from the page when it is needed: T by
its name, the writing layer by the name of the last other layer the user had current, never a kept index.

### Conversion (lasso convert, or Add text object)

1. If the page has no T and the writing layer is current: make T first. `addLayer` (on top, current), its name,
   `moveLayer` to the bottom, the writing layer current again (**X**, 3-4 steps), each step waited for. The handwriting
   is then selected again by its strokes and checked; if it is not exactly the handwriting, nothing is deleted.
2. Delete the handwriting (**A**; not for Add text object).
3. Draw the glyph strokes, record them, take copies, undo the k strokes (checked). Add the copies to T in one step
   (**B**) while the writing layer stays current. Then Text objects is made current (a switch, one group with the
   conversion) and the object selected there: a selection left on a layer that is not current is never committed.
4. One undo group: X + A + B + the switch.

An edit on T (T current) is A', B' on T: one group of 2.

### Double tap (finger)

- A QML overlay over the page (z 9000, above the pen tool's input) watches finger taps without taking them. Two taps
  within 450 ms and 60 px make a double tap. Large contacts (a palm), the toolbar strip, and a double tap that comes
  with an undo or redo (the gestures) are left alone.
- **Nothing changes unless it is on a text object.**
- An object on the current layer: selection tool on, the object selected, its edit box opened.
- Else, one of T's known boxes there: the selection tool on, T made current (one group with the step before it), the
  object selected (one rect) and its edit box opened.
- Two or more objects there (on the current layer or among T's boxes): a short "Text objects overlap here." message,
  nothing else.
- T's boxes: added at a conversion, read again whenever T is current and a selection ends there. Kept per page in
  `textlayer.json`: pages gone are dropped on load, at most 200 boxes a page and 500 pages, and `vellum purge` removes
  it.

### T current (object mode)

- T current for any reason (a double tap, a pick in the layers menu, an undo, a page that opens on it): the selection
  tool is turned on (not a step).
- Choosing another tool there (the selection tool's `selected` turning false): the writing layer current again, one
  group with the step before it. Ink never goes onto T.
- A single finger tap on an object: it is selected (no step); a second tap within 450 ms opens the edit box (the
  double tap). The first finger of an undo/redo gesture is left alone (an undo or redo came with it).
- Back to the writing layer when the user ends object mode with nothing selected: a finger tap on no object, or a
  lasso or pen tap that selected nothing. One group with the step before it (the switch to T when nothing happened
  there, else the move made there). Never because a selection merely ended (a new lasso and our own flows end
  selections too), never after an undo or redo, never on a pick in the layers menu.
- Single finger taps on other layers do nothing (a palm or finger while writing).

- A dashed box around every object while T is current and was chosen in the layers panel (not when a conversion or a
  double tap made it current) (plain Rectangles over DeviceSceneView; a Canvas painted
  nothing, probed). The boxes are read from the page when its content may have changed (layer change, selection
  end, undo/redo, conversion, edit) and mapped with `sceneToView` whenever the view moves (`viewportChanged`,
  `viewScaleChanged`, `scrollPositionChanged`); hidden while `gesturing`.
- Lasso several objects: they move and resize together (one pending edit, one step). Edit is refused for more than
  one object on T ("double tap one to edit it"); joining objects into one edit stays on other layers.
- A tool change during our own tap sequence (tool, layer, selection, edit box) is ours: it is looked at once when the
  sequence ends (no time windows; a 5 s cap only as a fail-safe).

### Objects kept apart

- A conversion onto Text objects: if the new object would overlap a known object there, it is put at the nearest
  free place (up, down, left or right, at most its own size away). With no room near, nothing is converted (a
  message); nothing has changed by then.
- Checked again on T with its real strokes once the conversion switched there (the known boxes may be old): still
  overlapping, the object is selected and pushed (moveSelectedItems + applyPendingEdit, one more step in the
  conversion's group); no room near, the whole conversion group is undone (a message).
- A move or resize on Text objects: pushed the same way inside the same pending edit, so the move and the push are
  one step (`moveSelectedItems` adds to the pending edit: probed). It waits for the pen to be up after the last
  change (`penHandler.timeSincePenUp()` restarts at each lift: probed). A selected group is pushed as one block,
  each object checked against the others' boxes (not one box around the group). With no room near for all of
  them, the move is put back (`cancelPendingEdit`: no step, probed) and the selection ended (a further move of it
  after a cancel is not committed: probed).
- An undo of a conversion drops its box from `textlayer.json`, and a redo puts it back, so an undone object does not
  push the next one.

### Highlights (bands)

- A highlighter stroke over an object on the writing layer becomes bands on T (`highlightToTextLayer`): the stroke
  is undone, one band per line (end to end over the characters, em-box high) is drawn on T in the stroke's colour, the
  highlight is kept in the object's record (character range, colour). One undo press takes the bands back (a group
  of the bands plus the layer switches around them: `setCurrentLayer` and each visibility change are steps).
- Bands are part of their object: they move with it, have no dashed box of their own, and are not reached by the
  eraser (it works on the current layer only; decided).
- The edit box has a Highlight button (on/off for the selected characters, or for what is typed next, as bold in a
  word processor) and a colour square that opens a palette (yellow, green, pink, blue, orange, gray). The preview
  shows the bands. Colours already on the object are read from its record when the box opens.
- One edit makes one object with one record (its id), selected and moved as a unit; when its bands are not matched to
  the expected rects exactly, the nearest strokes of the right count are taken as the bands so it is not split.

### Typed text under an object

- An overlay: an object may lie over typed text and is not pushed away by it (only other objects push).
- A finger double tap where both are: the object. xochitl may take the same double tap and select a word of the
  typed text (keyboard up; seen with the highlighter in use), and the selection tool cannot be turned on then: the
  text selection and cursor are ended first (`clearSelectedText`, `setCursorVisible(false, false)`).
- A highlighter stroke over both: xochitl snaps it to the typed text (a glyph range: `highlightWithLine` in the pen
  handler's `onStrokeCompleted`), and the object got nothing. The object comes first: at `strokeCompleted(Line)` (its
  `boundingRect` is read through the Line gadget) a stroke that lies on an object is looked for on the page 600 ms
  later; when no stroke of its bounds is there, it became a glyph range, which is undone (the page's strokes stay as
  they were, checked) and the object gets the bands. A stroke on typed text alone still snaps to it. xochitl's
  "Snap to text" setting is not reachable from the add-on (no object exposes it: probed 11 801 objects).
- The pen hook was connected as `strokeCompleted()`, a signal that does not exist, so it never ran: a stroke that
  does not change the page's strokes (a typed-text highlight) left the undo groups as they were, and the next undo
  press took a whole group under it. Now `strokeCompleted(Line)`.

### Layers menu

- The T row: up, down and merge hidden. Picking it and delete stay; rename is not blocked.
- The row below T cannot move up past it; the row above cannot move down or merge into it.
- The last other layer cannot be deleted.
- Only button items are touched. What we hid or disabled on a row that is no longer in one of those places is put
  back. Applied before every frame while the menu was opened in the last 30 s.

## Undo and redo: groups

Each of our actions is one **group** of n steps on top of xochitl's history:

| Action | n |
|---|---|
| Conversion | 2, plus 3-4 when T was made |
| Add text object | 1, plus 3-4 when T was made |
| Edit | 2 |
| A move or resize (committed when the selection ends) | 1 (the groups under it stay) |
| Our layer switch | 1 + the group under it if that is still on top, else 2 (with the step before it), or 1 at the start of the history |

How a press is handled:
- A press (button or gesture) while a group is on top: the other n - 1 steps are sent **inside**
  `aboutToUndo`/`aboutToRedo`, and the group moves to the other side (undone or redone).
- Our own calls are told by a re-entrancy count.
- **A group is on top only while nothing else happened.** A pen stroke, typed text, another page, or
  a press that was not one of ours marks all groups unknown.
- An undo also compares the page with the fingerprint taken after the group (page, layer names and order, current
  layer, its strokes, typed text length).
- Unknown means a plain single step: fail-safe, the worst case is one press too many, never steps destroyed.
- A press while something is selected only ends the selection (xochitl, probed 6 of 6); the groups stay. The toolbar
  buttons end an unmoved selection themselves on press, so a button press is always a real step (probed 3 of 3,
  1 of 3 without). A moved selection is left to xochitl: clearing it drops the move (probed).

Removed: hidden selections, S1/S2 pairs, settle loops, the press queue, the close guard, the open fallback, stray
moving, matching conversions by the page's strokes, every timing window.

## Bugs found on the device, and fixes

| Build | Symptom | Cause | Fix |
|---|---|---|---|
| 7409652c | Handwriting back unselected after undo | #15's reselect not done for T conversions | Reselect after the chain |
| 7409652c | Stayed on T after moving the object | A move is applied after the selection ends; looked untouched, the undo meant for S1 took the move | Compare 600 ms later; later replaced by S2 |
| 7409652c | Double tap selects, does not edit | Edit was a second step | Double tap opens the edit box |
| 0d797a6b | First undo after a move did nothing | It took S2 and was taken for the conversion's undo; no settle followed | Settle whenever a step lands on T |
| 0d797a6b | No double tap with the pen tool | Overlay below the pen tool's input | Overlay at z 9000 |
| 595df1e8 | "는" from earlier tests added to a conversion | Page opened on T; handwriting went onto T | Open fallback (removed in S') |
| 595df1e8 | Redo brought back later pen strokes | #15 resent a redo that looked unfinished | Resend only if the page did not change |
| 0d009cd3 | Redo stuck, button enabled | A redo that landed on our undone S1 undid it again: it stayed on top of the redo history | New step S2 when nothing is left to redo |
| 0d009cd3 | Redo history cut | The two-finger undo gesture seen as a double tap started a session | Double taps with an undo/redo are left alone |
| 4c8ab633 | Quick redo during a chain cut the history | Presses raced our chain; reselecting after a redo is a switch (a step) | Press queue; reselect only when nothing is left to redo |
| 0e3040b5 | "테" written by hand could not be lassoed | Page opened on T (closed while an object was selected there); strokes in the first 3 s went onto T | Close guard; strays moved at session end (both removed in S') |
| e10a6f86 | Layers menu buttons showed for a moment | Hidden after the rows were painted | Lock before every frame |
| cb57b732 | Redo button on after a tap outside; next double tap slow | Undoing S1 left it in the redo history | S2 as a step, untouched pairs passed through |
| cb57b732 | Double tap slow (~2.4 s) | Waits and a stroke-by-stroke selection | Shorter waits, one-rect select on T |
| cb57b732 | Layer 1's arrows flickered | Changing row switches rebuilt the buttons | Touch only the buttons |
| cb57b732 | One undo took several steps | Several settles at once; toolbar taps seen as double taps | One settle at a time; toolbar strip excluded; look on T only near a known object |
| 8798ea08 | Extra step when undoing a conversion | The selection's pair after it was taken for the conversion's undo | The conversion remembers its pair; the pair goes first |
| 8798ea08 | Pen-tool double taps missed | TapHandler's double tap | Taps counted by hand |
| 5a45c7d7 | Notebook broken: lasso on open, writing went to T, Layer 1 could not be picked | Old page with T on top opened on T; the open fallback turned on the lasso and selected nothing; then every undo ran settle up to 20 steps, undoing the user's strokes and their own switch to Layer 1 | Nothing on open; settle bounded (both superseded by S') |
| 1ae3249f | An extra step (handwriting gone, object not there yet) on redo, and on undo after more strokes | The chain waited to see the user's step before sending the rest | The rest sent right behind the user's step |
| 1ae3249f | Pen ink went onto T when the user had picked it | Nothing kept the pen off T | Object mode |
| a59a3c3e | A finger double tap on an object did nothing | The page's object boxes were stored empty: during a redo the layer was current for a moment before its objects were back, and that empty list was kept | Boxes not read during an undo or redo; never an empty list over a layer that has strokes |
| a59a3c3e | The Text objects row could not be picked in the layers menu | Its name field made read-only blocked the row's own tap | The name field left alone (rename not blocked) |
| a59a3c3e | A conversion stayed plain strokes on Layer 1 | A stray stroke was being taken out of the new ink's selection when the copies were made (97 instead of 96) | Grouping waits until the take-out is done |
| a59a3c3e | Redo of a conversion took 4 presses | Undoing the conversion also removed the Text objects layer it made, so the user's layer index moved down; the redo check compared the stored index | The user's layer is any layer but T, not a stored index |
| 6ad906ef | The tool switched to the selection tool while writing | Touches while writing (a palm, a resting finger) counted as a double tap, and the tool was switched before looking for an object | Nothing changes unless the tap is on a known object; large contacts ignored; object boxes kept across restarts |
| a59a3c3e (S') | Chains, boxes and switches still drifted (independent review) | Matching a conversion by the page's strokes fires when it is not on top; observe-then-send paths; time windows | Groups with activity tracking and a page fingerprint; chains sent inside the signal; T made before the handwriting is deleted |
| a05db063 | Nothing improved: undo stopped or skipped around selections | The pair bookkeeping (where our switches sit in xochitl's unseen history) drifted with pending undos, edits in sessions and fast presses | Design (S'): no hidden switching at all |

**Known limits.**
- After a double tap the user is on T (visible in the layers menu) until another tool is chosen.
- Objects follow typed text like ink (stock anchoring, decided).
- The eraser does not reach bands on T; a highlight is removed in the edit box (Highlight off) or by undo.
- A double tap on an object over typed text with the Text tool in use: not checked.
- A highlighter stroke over an object and typed text: the part over typed text beyond the object is not highlighted
  (the object comes first, the whole stroke goes to it).
- Edit box preview colours for blue and gray are estimates (yellow, green, pink and orange are sampled on the device).
- A two-line object was once seen boxed line by line after a pushed move on the device; not reproduced in four
  tries. For 5 s after a pushed move, a record found only in part is logged (`objects: record ... found in part`)
  with the strokes that are off.

## Testing a build

On a fresh notebook:
1. Write with the pen at once: ink on Layer 1.
2. Convert handwriting: Text objects (below Layer 1 in the layers menu) is current, the object selected; it can be
   moved at once.
3. One undo: the handwriting is back (and the layer it made is gone); one redo: the object is back.
4. Strokes, then fast undos and redos (button and gesture): one step per press past the conversion's group.
5. Finger double tap on the object (any tool): Text objects current, selection tool, edit box.
6. Cancel the edit box, choose the pen: Layer 1 current again; one undo goes back past both switches together with
   the step before them.
7. Edit an object, choose the pen, undo: the edit and the switch in one press.
8. Delete Text objects in the layers menu, convert again: a new one at the bottom.
9. A tap where two objects overlap, or on an object crossed by another on Text objects: the overlap message.
10. Undo with the toolbar button while an object is selected: a real undo every time (`undo/redo button: the
    selection ended first`).
11. Convert next to (or over) an object: the new one lands beside it; no room near: not converted, message.
12. Move an object onto another: pushed off it on lift; one undo puts it back where it was. No room: put back.
13. On Text objects, single tap an object: selected; tap again quickly: the edit box.
14. On Text objects, tap on no object (or lasso nothing): Layer 1 current; one undo: back on Text objects together
    with the step before.
15. On Layer 1, single taps do nothing.
16. Text objects chosen in the layers panel: a dashed box on every object; Layer 1 current, or T made current by a
    conversion or a double tap: none.
17. Zoom and scroll: the boxes follow once the view is still.
18. Lasso two objects, move: both move; one undo puts both back.
19. Move a group onto another object: pushed as a block; no room: all put back (nothing to undo).
20. A group around an unselected object, nudged: not pushed.
21. Double tap with two objects selected on Text objects: the message, no edit box.
22. Nudge one object by less than its own size: it stays where it was put.

## Log lines (`journalctl -u xochitl | grep rmpp-textobj`)

- `text layer: moveLayer(int,int)`, `text layer: added (layer 1)`: the layer made at the bottom.
- `convert: the handwriting could not be selected again`: the conversion stopped before deleting anything.
- `new ink: N strokes in one undo step ..., on the text layer`, `one press: a group of N step(s)`: a group.
- `undo: N step(s) in one press` / `redo: ...`: a group undone or redone.
- `one press: <why>: groups dropped`: activity, the next presses are plain.
- `double tap seen at x, y`, `not on a text object`, `to the text layer for the object there`, `selecting a text
  object`, `edit: ...`: the double tap path.
- `text layer: another tool chosen on it`, `the page opened on it: the selection tool`: T current.
- `put lines: ...`: why rmpp-hwr's call did not make an object.
