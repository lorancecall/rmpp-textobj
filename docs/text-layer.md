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

## Design (S'): no hidden layer switching

The first design hid the layer changes: the object was selected on T right after a conversion and after a double
tap, with a pair of switches (S1 to T, S2 back) that undo and redo were meant to skip. That needed a model of
xochitl's undo history that cannot be checked (switches cannot be merged, removed or inspected; undo is asynchronous;
T cannot be seen from L). Over about fifteen test rounds every fix moved the leak somewhere else (table below), and one
build broke a notebook. The developer chose the simpler design (S'): **T is made current only by the user's own
action**, and only rules on what can be observed are used.

### Conversion (lasso convert, or Add text object)

1. Delete the handwriting on L (step **A**; not for Add text object).
2. Draw the glyph strokes on L (k steps), select and record them, take copies, undo the k strokes (checked).
3. If the page has no T: `addLayer`, its name, `moveLayer` to the bottom (checked; taken as found if it is not where
   expected), L current again (**X1-X4**; L's index goes up by one).
4. Add the copies to T in one step (**B**) while L stays current. The object is **not selected**: a selection on T
   needs T current, which would be a step.

History: `A, [X1-X4], B`. One undo from the result: the user's undo takes B (unseen from L), and `steps + 1` more
undos (X4..X1, A) are sent right behind it; the handwriting comes back selected. One redo from the start: the user's
redo takes A, and the rest is sent right behind it.

### Double tap (finger)

- A QML overlay over the page (z 9000, above the pen tool's input) watches finger taps without taking them (passive
  grab); two taps within 450 ms and 60 px make a double tap (the TapHandler's own double tap missed some with the pen
  tool).
- Left alone: taps on the toolbar strip (x < 130 while it is shown); a double tap with an undo or redo within 800 ms
  (the two- and three-finger gestures look like double taps), checked after 350 ms.
- Large contacts (a palm, over 45 px) are not taps.
- **Nothing changes (tool or layer) unless the double tap is on a text object.** An object on the current layer: the
  selection tool is turned on if needed, the object selected and its edit box opened.
- Else, if the tap is on one of T's objects (their boxes, known from conversions and from whenever T is current, kept
  per page in `textlayer.json` so they hold after a restart; pages that no longer exist, a document deleted and the
  trash emptied, are dropped from it on load; at most 200 boxes per page and 500 pages; `vellum purge` removes it
  with the folder): the selection tool on, T made current, **a step the user
  sees** (the layers menu shows it), and the object is selected there (one rect: T holds only objects) and its edit
  box opened. The edit is one undo step (the one-layer chain of #6/#15, T being current).

### Object mode (T current)

- The user picked T in the layers menu: the selection tool is turned on.
- T current for any reason (picked, a double tap, an undo): choosing another tool (pen, eraser, text) makes the
  writing layer current, a step the user started by choosing the tool. Ink never goes onto T.
- A page that opens on T: **nothing is done** (no layer or tool switch); only a later change of tool counts.

Planned: dashed boxes on every object, a tap edits (overlap: a short message), drag moves, lasso multi-selects (Edit
off for more than one); the same overlay will let a double tap take the tap before xochitl's typed text does.

### Layers menu

The Text objects row: up, down and merge hidden, its name read-only; **delete stays** (the developer: it must be
deletable). The row below it cannot move up past it, the row above it cannot move down or merge into it (their
buttons disabled in place). Only the button items are touched: changing a row's `layer*Enabled` switches made xochitl
rebuild its buttons (they flickered). Applied before every frame (`QQuickWindow::afterAnimating`) for 4 s after the
layers button is pressed, so nothing shows for a frame.

### Removed with (S')

Hidden selections and their S1/S2 pairs, the settle loop, the press queue, the close guard, the switch back on open,
moving stray strokes off T. None is needed when T is never made current behind the user's back.

## Undo and redo

All in `aboutToUndoRedo` (edit.h), for every undo and redo (toolbar buttons and gestures):

1. **Ours?** Our own `undo()`/`redo()` calls are counted (each emits the signal once) and skipped.
2. **The newest conversion on the page the step starts from:**
   - T conversion, undo from its result (a layer other than T current, as right after it; never by a stored layer index:
     undoing a conversion can remove T and move L down): `steps + 1` more undos right behind;
   - T conversion, redo from its start: `steps + 1` more redos right behind;
   - a conversion or edit on the current layer (an object on L, or an edit on T while T is current): the one-layer
     chain of #6/#15 (result -> neither -> one more step; resent only if the page did not change at all).
   Sent right behind the user's step, not after seeing it: waiting showed the state between (handwriting gone, no
   object yet) as if it were a step of its own.
3. **Layer change only:** a user's undo or redo after which the current layer changed (a double tap's switch to T, or
   the switch back when a tool was chosen) is followed by **exactly one** more step the same way (through 2, so a
   conversion or edit there goes in one press). One more, never a loop: the worst case is one extra step.

**We push steps only as part of the user's own action**: a conversion, a double tap, choosing a tool on T.

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
| a59a3c3e | Redo of a conversion took 4 presses | Undoing the conversion also removed the Text objects layer it made, so the user's layer index moved down; the redo check compared the stored index | The user's layer is any layer but T, not a stored index |
| 6ad906ef | The tool switched to the selection tool while writing | Touches while writing (a palm, a resting finger) counted as a double tap, and the tool was switched before looking for an object | Nothing changes unless the tap is on a known object; large contacts ignored; object boxes kept across restarts |
| a05db063 | Nothing improved: undo stopped or skipped around selections | The pair bookkeeping (where our switches sit in xochitl's unseen history) drifted with pending undos, edits in sessions and fast presses | Design (S'): no hidden switching at all |

**Known limits.** After a conversion the object is not selected (it is moved after a double tap, or in object mode).
After a double tap the user is on T (visible in the layers menu) until another tool is chosen. Typed text over an
object takes a double tap there first. Objects follow typed text like ink (stock anchoring, decided).

## Testing a build

On a fresh notebook:
1. Write with the pen at once: the tool stays, the ink goes to Layer 1.
2. Convert handwriting: the object appears unselected; the layers menu shows Text objects below Layer 1 (no up,
   down or merge; delete there).
3. Undo once: the handwriting is back, selected; redo once: the object is back. No step in between.
4. More strokes, then fast undos and redos (button and gesture): one step per press, redo never cut.
5. Finger double tap on the object (pen tool and selection tool): Text objects becomes current, the edit box opens.
6. Close the edit box, choose the pen: Layer 1 current again; writing goes there.
7. Undo after 5-6: one press takes the switch back together with the step before it; the edit is one undo.
8. Pick Text objects in the layers menu: the selection tool turns on; choose the pen: back to Layer 1.

## Log lines (`journalctl -u xochitl | grep rmpp-textobj`)

- `text layer: moveLayer(int,int)`, `text layer: added (layer 1, user's layer 2)`: the layer made at the bottom.
- `new ink: N strokes in one undo step ..., on the text layer`: B done.
- `undo: conversion undone in one step (text layer, N step(s) more)` / `redo: ... redone ...`: the conversion chain.
- `undo changed only the current layer: one more`: the layer-change rule.
- `double tap seen at x, y`, `an undo/redo gesture, left alone`, `no text object near it`, `to the text layer for
  the object there`, `selecting a text object`, `edit: ...`: the double tap path.
- `text layer: picked by the user`, `another tool chosen on it: layer N current for writing`, `the page opened on
  it: left as it is`: object mode.
