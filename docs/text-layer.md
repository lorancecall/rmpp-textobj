# The Text objects layer

How text objects live on a layer of their own without the user seeing a layer change, why each part is the way it
is, and what went wrong on the way (issue #3). Code: `src/layer.h` (the layer, selections on it, the layers menu),
`src/edit.h` (conversion, undo/redo chaining), `src/select.h` (double tap).

## What we have to work with

**Layers.** A page has the user's layers (Layer 1, ...) and, from the first conversion, the **Text objects** layer
(T), made by us at the bottom, as xochitl's own Text row is shown below the layers. xochitl's Text (typed text) and
Template rows are not scene layers: they are not counted in `layerCount`. L below is the layer the user writes on.

**SceneController calls used** (OS 3.29.1): `addLayer()` (adds on top, makes it current), `setLayerName(int, QString)`,
`moveLayer(int, int)` (signature looked up and logged), `setCurrentLayer(int)`, `layerName(int)`, `layerCount`,
`currentLayer`; `cloneSelectedItems(int, double)`, `getItemBoundingRect(...)`,
`cloneAddAndSelectItems(int layer, items, scale, centre)` (adds items to any layer in one step),
`deleteSelectedItems(int)`, `clearSelectedItems()`, `addSelectionRect(QRect, mode)`,
`getLineBoundingRectsToBeSelected(QRect)` (strokes of the **current layer only**), `undo()`, `redo()`,
`undoAvailable`, `redoAvailable`. Signals: `aboutToUndo()`, `aboutToRedo()` (before every undo or redo, whatever
started it: toolbar button, two- or three-finger tap, our own calls), `currentLayerChanged()`,
`selectionItemCountChanged()`.

**What is an undo step.** Every one of these is a step of its own: `addLayer`, `setLayerName`, `moveLayer`,
`setCurrentLayer`, `cloneAddAndSelectItems`, `deleteSelectedItems`, each `addDrawingLine`, a move or resize of a
selection. Not steps: making or ending a selection. Any new step clears the redo history. Undo/redo calls take effect
a moment after they are made, and an `undo()` right after a selection ends is ignored.

**What xochitl does not give us.** No undo grouping (a transaction does not group strokes), no way to see another
layer's strokes without making it current, no selection on a layer that is not current (dropped within a second), no
lasso path (only its result), no way to veto a step.

**Stock behaviours that matter.**
- Ink under typed text is anchored to its characters: it moves when the text grows and is deleted when they are
  (backspacing through the newlines a typed-text conversion padded with). Our objects are anchored like any ink;
  decided: objects follow typed text as ink does.
- A double tap on typed text selects a word there (xochitl takes it).
- The page file stores the current layer; a page closed while T is current opens on T.

## Flows

### Conversion (lasso convert, or Add text object)

1. Delete the handwriting on L (step **A**; not for Add text object).
2. Draw the glyph strokes on L (k steps), select and record them, take copies, undo the k strokes (checked).
3. If the page has no T: `addLayer`, its name, `moveLayer` to the bottom, L current again (**X1-X4**; L's index goes
   up by one).
4. Add the copies to T in one step (**B**) while L stays current.
5. Hidden selection (below) so the user can move the new object at once.

History after an untouched conversion: `A, [X1-X4], B, S1, S2`.

### Hidden selection on T (session)

Started by a conversion, a finger double tap, or a redo of a conversion (only when nothing is left to redo).

- **S1**: `setCurrentLayer(T)` (a step), then select the object there (one rect: T holds only objects).
- The user moves, resizes, edits (double tap or the edit box) or deletes it.
- **End** (`selectionItemCountChanged` to 0: a tap outside, a tool change), checked 600 ms later (a move is applied
  just after the selection ends):
  - strokes on T that are not objects nor marks on one (written there by mistake) are moved to L, if every object
    found at the start is still found, the selection tool is already on (never a tool switch for it) and exactly those
    strokes got selected;
  - **S2**: `setCurrentLayer(L)` (a step). The pair (S1, S2) is remembered as **untouched** (T unchanged) or not.
- An undo or redo pressed while selected only ends the selection in xochitl: it is kept and applied after S2.

Undoing S1 instead of adding S2 (the first way) left S1 in the redo history: the redo button came on after a tap
outside, and a redo replayed the switch.

### Double tap (finger)

- A QML overlay over the page (z 9000, above the pen tool's input) watches finger taps without taking them (passive
  grab); two taps within 450 ms and 60 px make a double tap (the TapHandler's own double tap missed some with the pen
  tool).
- Left alone: taps on the toolbar strip (x < 130 while it is shown); a double tap with an undo or redo within 800 ms
  (the two- and three-finger gestures look like double taps), checked after 350 ms.
- Any tool: the selection tool is turned on first.
- An object on L (made before the layer): selected and edited there, as before.
- Else, if T may have an object there (positions read whenever T is current, per page; none read yet counts as yes):
  hidden selection on T, select the object at the tap, open the edit box. Nothing there: the session ends untouched.

### Close guard; nothing on open

- The document's close button pressed while T is current (and not picked by the user): L is made current first, so
  the page is not saved on T.
- **Nothing is done when a page opens**: no layer switch, no tool switch, no strokes moved. A page that opens on T is
  taken as the user's pick (no undo or redo is ever chained for it); the user switches layers as usual. An automatic
  switch back on open (5a45c7d7) turned on the lasso to move strokes, selected none, and with the settle below ate the
  notebook's history.

### Layers menu

The Text objects row looks like xochitl's Text row: its buttons (up, down, merge, delete) hidden, its name
read-only. The row below it cannot move up past it, the row above cannot move down or merge into it (their buttons
disabled in place). Only the button items are touched: changing a row's `layer*Enabled` switches made xochitl rebuild
its buttons (they flickered). Applied before every frame (`QQuickWindow::afterAnimating`) for 4 s after the layers
button is pressed, so nothing shows for a frame.

### Object mode

T picked in the layers menu: the selection tool is turned on. Choosing another tool (pen, eraser, text) while T is
current makes the user's writing layer (the topmost other layer) current first, so ink never goes onto T; that switch
is a step the user started. A page that opens on T gets only the watch: nothing is switched until the user changes
the tool.

Planned: dashed boxes on every object (overlay, follows zoom and scrolling), a tap edits (a tap where objects
overlap: a short message), drag moves, lasso multi-selects (Edit off for more than one). The same overlay will let a
double tap on an object take the tap before xochitl's typed text does.

## Undo and redo

All in `aboutToUndoRedo` (edit.h), for every undo and redo:

1. **Ours?** Our own `undo()`/`redo()` calls are counted (each emits the signal once) and skipped.
2. **Press queue.** While our own steps follow a user's step (a conversion's chain or a settle), a press in the
   chain's direction does one of its steps; any other press is taken back at once and done again, in order, after
   the chain.
3. **Session open?** The press is kept until the session ends (above).
4. **userStep:** the newest conversion on the page the step starts from:
   - **T conversion, undo, from its result** (L as right after it): if its selection's pair is still on top, the pair
     goes first (settle); else the user's undo takes B, and `steps + 1` more undos (X4..X1, A) are sent right behind
     it: the handwriting is back, selected as soon as it is there.
   - **T conversion, redo, from its start**: the user's redo takes A, and `steps + 1` more redos (X1..X4, B) are sent
     right behind it.
   - Sent right behind, not after seeing the user's step: undo calls are applied in order, and waiting showed the
     state between (handwriting gone, no object yet) as if it were a step (1ae3249f).
   - **L conversion** (object on L, or an edit on T while T is current): the one-layer chain of #6/#15 (result ->
     neither -> one more step; resent only if the page did not change at all).
5. **Settle:** after a step that leaves T current, one more step the same way until L is current, **only through a
   pair we made**: with no pair on record in that direction (T current for any other reason, or picked by the user),
   nothing is undone or redone; at most 8 steps; one settle at a time. Leaving T past a pair: an **untouched** pair is
   passed through to the user's own step (userStep again); a changed pair stops there (one undo = the moves of that
   selection).

**We never push a step outside the user's own action** (a conversion, a selection on T, the close guard). The earlier
"switch back as a new step after a redo that ended on T" is gone: with pairs, a redo never ends between S1 and S2.

## Bugs found on the device, and fixes

| Build | Symptom | Cause | Fix |
|---|---|---|---|
| 7409652c | Handwriting back unselected after undo | #15's reselect not done for T conversions | Reselect after the chain |
| 7409652c | Stayed on T after moving the object | A move is applied after the selection ends; looked untouched, the undo meant for S1 took the move | Compare 600 ms later; later replaced by S2 |
| 7409652c | Double tap selects, does not edit | Edit was a second step | Double tap opens the edit box |
| 0d797a6b | First undo after a move did nothing | It took S2 and was taken for the conversion's undo; no settle followed | Settle whenever a step lands on T |
| 0d797a6b | No double tap with the pen tool | Overlay below the pen tool's input | Overlay at z 9000 |
| 595df1e8 | "는" from earlier tests added to a conversion | Page opened on T; handwriting went onto T | Open fallback |
| 595df1e8 | Redo brought back later pen strokes | #15 resent a redo that looked unfinished | Resend only if the page did not change |
| 0d009cd3 | Redo stuck, button enabled | A redo that landed on our undone S1 undid it again: it stayed on top of the redo history | New step S2 when nothing is left to redo |
| 0d009cd3 | Redo history cut | The two-finger undo gesture seen as a double tap started a session | Double taps with an undo/redo are left alone |
| 4c8ab633 | Quick redo during a chain cut the history | Presses raced our chain; reselecting after a redo is a switch (a step) | Press queue; reselect only when nothing is left to redo |
| 0e3040b5 | "테" written by hand could not be lassoed | Page opened on T (closed while an object was selected there); strokes in the first 3 s went onto T | Close guard; strays moved at every session end |
| e10a6f86 | Layers menu buttons showed for a moment | Hidden after the rows were painted | Lock before every frame |
| cb57b732 | Redo button on after a tap outside; next double tap slow | Undoing S1 left it in the redo history | S2 as a step, untouched pairs passed through |
| cb57b732 | Double tap slow (~2.4 s) | Waits and a stroke-by-stroke selection | Shorter waits, one-rect select on T |
| cb57b732 | Layer 1's arrows flickered | Changing row switches rebuilt the buttons | Touch only the buttons |
| cb57b732 | One undo took several steps | Several settles at once; toolbar taps seen as double taps | One settle at a time; toolbar strip excluded; look on T only near a known object |
| 8798ea08 | Extra step when undoing a conversion | The selection's pair after it was taken for the conversion's undo | The conversion remembers its pair; the pair goes first |
| 8798ea08 | Pen-tool double taps missed | TapHandler's double tap | Taps counted by hand |
| 1ae3249f | An extra step (handwriting gone, object not there yet) on redo, and on undo after more strokes | The chain waited to see the user's step before sending the rest | The rest sent right behind the user's step |
| 1ae3249f | Pen ink went onto T when the user had picked it | Nothing kept the pen off T | Object mode: the selection tool on pick; another tool chosen: the writing layer current |
| 5a45c7d7 | Notebook broken: tool switched to the lasso on open, writing went to T, Layer 1 could not be picked (jumped back) | Old page with T on top opened on T; the open fallback turned on the lasso to move the new strokes and selected none; then every undo ran settle up to 20 steps (T current), undoing the user's strokes and their own switch to Layer 1 | Nothing on open (T current = the user's pick); settle only through a pair we made, at most 8 steps; strays moved only with the selection tool already on and exactly selected |

**Known limits.** Typed text over an object takes a double tap there first (fixed with the object-mode overlay,
planned). Objects follow typed text like ink (stock anchoring, decided). A page change during a hidden selection is
not handled. Pages made before the layer moved to the bottom keep it on top (three test notebooks; their files are
kept for debugging).

## Testing a build

On a fresh page (an old page keeps the layer where it was):
1. Convert handwriting: it becomes an object, selected; the layers menu shows Text objects below Layer 1.
2. Tap outside: the redo button stays off; write and erase: only Layer 1 changes.
3. Undo once: the handwriting is back, selected; redo once: the object is back.
4. Move the object, deselect, undo: the move goes; undo again: the handwriting is back.
5. Many strokes, then fast undos and redos (button and gesture): one step per press, redo never cut.
6. Finger double tap on the object with the pen tool and with the selection tool: the edit box opens.
7. Select the object, close the notebook, reopen: writing goes to Layer 1.
8. Layers menu: Text objects row without buttons from the first frame, Layer 1's arrows steady.

## Log lines (`journalctl -u xochitl | grep rmpp-textobj`)

- `text layer: added (layer 1, user's layer 2)`: the layer made; `moveLayer(...)`: the signature found.
- `new ink: N strokes in one undo step ..., on the text layer`: B done.
- `text layer: object selected (N strokes on the layer)`: S1 and the selection; N = all strokes on T.
- `text layer: back to layer N (untouched: ...)` / `(object changed)`: S2 and the pair's kind.
- `undo from a conversion's result (text layer)`, `undo: conversion undone in one step (text layer, N step(s) more,
  M by presses)`: the conversion chain.
- `undo: a selection's switches come first`: the pair on top goes before the conversion.
- `text layer: N more undo step(s) to leave it`, `an untouched selection passed: the undo goes on`: settle.
- `... pressed during the chain: one of its steps` / `queued` / `done now`: the press queue.
- `double tap seen at x, y`, `an undo/redo gesture, left alone`, `no text object near it`, `the selection tool
  turned on`, `selecting a text object`: the double tap path.
- `the page opened on it`, `the document closes: layer N current first`, `N stroke(s) on it are not text objects`:
  fallback, close guard, safety net.
