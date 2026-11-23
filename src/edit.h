// ---- drawing a text object and editing one ----

bool g_putting = false;   // our own ink is being added and selected (the selection rules leave it alone)
QElapsedTimer g_putStarted;

bool putting() { return g_putting && g_putStarted.isValid() && g_putStarted.elapsed() < 8000; }

QString rectKey(const QRect &r);   // src/select.h
void setSelection(QPointer<QObject> scp, const QList<QRect> &rects, const QList<int> &want, std::function<void()> done);

// The strokes to lasso for new ink: the glyph strokes, and the corners of its highlight bands.
QList<QList<QPointF>> inkAndBands(const Placed &pl, const QList<Hl> &hls) {
    QList<QList<QPointF>> out = pl.strokes;
    for (const Hl &h : hls)
        for (const QRectF &r : h.rects) out.append({r.topLeft(), r.bottomRight()});
    return out;
}

// The conversions and edits of this session: the handwriting (or the old object) each deleted, and its new ink, as
// stroke-rect keys. An undo that takes a conversion's new ink away is followed by one more, which brings its
// handwriting back, so that one undo undoes the conversion (watchUndoRedo); a redo likewise. Newest last.
// A conversion to the Text objects layer (layer.h; text >= 0): its new ink is not seen from the user's layer (home),
// so it is told by that layer's strokes: lAfter (the handwriting gone) and lBefore (there); steps: the layer's own
// creation steps taken with it (addLayer, its name, the user's layer current again).
struct LastConv {
    QString page;
    QSet<QString> obj, hand;
    int text = -1, home = -1, steps = 0;
    QSet<QString> lAfter, lBefore;
    QRect box;   // the new ink, to select it after a redo
    int pair = 0;   // layer.h g_pairIdx once the selection after it ended: its pair is on top while g_pairIdx >= pair
};
QList<LastConv> g_convs;
// The selection right after the newest conversion ended: its pair of layer switches is remembered with it.
void convPairDone(int idx) { if (!g_convs.isEmpty() && g_convs.last().text >= 0) g_convs.last().pair = idx; }
void ownUndoRedo(QObject *sc, bool undo, int times = 1);   // below: our own undo()/redo() calls
// layer.h
int textLayer(QObject *sc);
void ensureTextLayer(QPointer<QObject> scp, int home, std::function<void(int, int, int)> done);
void markSessionForConv();
void startTextSession(QPointer<QObject> scp, const QString &page, int home, int text, std::function<void()> select);
void textSessionSelected();
bool textSessionActive();
QSet<QString> layerKeys(QObject *sc);
void settleLayer(bool undo, int n = 0);

// One undo step for new ink (#6). addDrawingLine makes an undo step per stroke (a conversion took ~28 undos to
// bring the handwriting back); SceneController::cloneAddAndSelectItems adds a list of items as one step, centred on a
// point. So, once the new ink is recorded and selected: copy it, undo the k additions, check that exactly the new
// strokes went (else redo them and keep the ink as it is), and add the copies back in one step, where they were.
void groupNewInk(QPointer<QObject> scp, const QSet<QString> &before, int k, const QString &page = QString(),
                 const QSet<QString> &hand = {}) {
    QObject *sc = scp;
    if (!sc || k <= 1) return;
    const QMetaObject *mo = sc->metaObject();
    const int clone = mo->indexOfMethod("cloneSelectedItems(int,double)");
    const int bound = mo->indexOfMethod("getItemBoundingRect(QList<std::shared_ptr<SceneItem>>)");
    const int add = mo->indexOfMethod("cloneAddAndSelectItems(int,QList<std::shared_ptr<SceneItem>>,double,QPointF)");
    if (clone < 0 || bound < 0 || add < 0 || mo->indexOfMethod("undo()") < 0 || mo->indexOfMethod("redo()") < 0) return;
    int layer = sc->property("currentLayer").toInt();
    double scale = 1.0;
    QList<std::shared_ptr<void>> items;
    void *a1[] = {&items, &layer, &scale};
    sc->qt_metacall(QMetaObject::InvokeMetaMethod, clone, a1);
    if (items.size() != k) { TLOG("one undo step: %lld items selected, %d added: left as is", (long long)items.size(), k); return; }
    QRectF box;
    void *a2[] = {&box, &items};
    sc->qt_metacall(QMetaObject::InvokeMetaMethod, bound, a2);
    if (box.isEmpty()) return;
    QMetaObject::invokeMethod(sc, "clearSelectedItems");
    // undo() right after ending a selection is ignored (device, OS 3.29.1), and takes effect a moment after it is
    // called: undo once the selection is gone, then wait until the new strokes are gone
    auto check = std::make_shared<std::function<void(int)>>();
    *check = [scp, before, k, items, box, layer, add, check, page, hand](int tries) {
        QObject *sc = scp;
        if (!sc) return;
        int fresh = 0, kept = 0;
        QSet<QString> seen;
        for (const QRect &r : pageLineRects(sc)) {
            const QString key = rectKey(r);
            if (seen.contains(key)) continue;
            seen.insert(key);
            if (before.contains(key)) ++kept; else ++fresh;
        }
        if (fresh > 0 && kept == before.size() && tries < 15) {   // not all undone yet
            QTimer::singleShot(200, sc, [check, tries] { (*check)(tries + 1); });
            return;
        }
        if (fresh != 0 || kept != before.size()) {   // not exactly the new ink undone: put everything back
            ownUndoRedo(sc, false, k);
            TLOG("one undo step: the undo did not match (%d new, %d of %lld kept): redone, left as is", fresh, kept,
                 (long long)before.size());
            return;
        }
        // into the Text objects layer (layer.h), unless that is where the user is (an edit there)
        auto place = [scp, items, box, add, k, tries, before, page, hand](int text, int steps, int layer) {
            QObject *sc = scp;
            if (!sc) return;
            const bool toText = text >= 0 && text != layer;
            int lay = toText ? text : layer;
            QList<std::shared_ptr<void>> its = items;
            double sc1 = 1.0;
            QPointF centre = box.center();
            void *a3[] = {nullptr, &lay, &its, &sc1, &centre};
            sc->qt_metacall(QMetaObject::InvokeMetaMethod, add, a3);
            TLOG("new ink: %d strokes in one undo step (%d checks)%s", k, tries + 1, toText ? ", on the text layer" : "");
            if (!toText) {
                if (!hand.isEmpty())   // the new ink's keys, once it is on the page again
                    QTimer::singleShot(400, sc, [scp, before, page, hand] {
                        if (!scp) return;
                        LastConv lc;
                        lc.page = page;
                        lc.hand = hand;
                        for (const QRect &r : pageLineRects(scp)) if (!before.contains(rectKey(r))) lc.obj.insert(rectKey(r));
                        if (lc.obj.isEmpty()) return;
                        g_convs.append(lc);
                        if (g_convs.size() > 100) g_convs.removeFirst();
                    });
                return;
            }
            QTimer::singleShot(300, sc, [scp, page, hand, layer, text, steps, box] {
                if (!scp) return;
                if (!hand.isEmpty()) {
                    LastConv lc;
                    lc.page = page;
                    lc.hand = hand;
                    lc.text = text;
                    lc.home = layer;
                    lc.steps = steps;
                    lc.lAfter = layerKeys(scp);
                    lc.lBefore = lc.lAfter + hand;
                    lc.box = box.toAlignedRect();
                    g_convs.append(lc);
                    if (g_convs.size() > 100) g_convs.removeFirst();
                }
                const QRect sel = box.toAlignedRect().adjusted(-2, -2, 2, 2);
                startTextSession(scp, page, layer, text, [scp, sel] {
                    markSessionForConv();
                    const bool switched = useSelectionTool();
                    QTimer::singleShot(switched ? 500 : 0, scp, [scp, sel] {
                        if (!scp) return;
                        selectRect(scp, sel, 0);
                        QTimer::singleShot(500, scp, [] { textSessionSelected(); });
                    });
                });
            });
        };
        if (textSessionActive()) place(-1, 0, layer);
        else ensureTextLayer(scp, layer, place);
    };
    QTimer::singleShot(400, sc, [scp, k, check] {
        if (!scp) return;
        ownUndoRedo(scp, true, k);
        QTimer::singleShot(200, scp, [check] { (*check)(0); });
    });
}

// After new ink is added and selected: record it from the selection's stroke rects (removing the records in
// `removeIds`, the object it replaces). The selection settles after a while (longer for big ink), so measure again
// until it holds all n new strokes and the highlight bands; select again if it is still empty (slow tool switch).
// Bands that did not come (not drawn, not selected) are left out after a few tries.
// `before`: the page's strokes before the new ink was added. The lasso around new ink also takes strokes of an
// object it overlaps: those are left out (and taken out of the selection).
void recordNew(QPointer<QObject> scp, const QString &page, const QString &text, const Placed &pl, int n,
               const QStringList &removeIds, const QList<Hl> &hls = {}, int attempt = 0,
               const QSet<QString> &before = {}, const QSet<QString> &hand = {}) {
    QTimer::singleShot(attempt == 0 ? 300 : 400, qApp, [=] {
        if (!scp) { g_putting = false; return; }
        const int nb = bandCount(hls);
        const int count = scp->property("selectionItemCount").toInt();
        QList<QRect> rects;
        if (count >= n) {
            for (const QRect &r : lineRects(scp)) if (!before.contains(rectKey(r))) rects << r;
            if (rects.size() < count && (rects.size() == n + nb || (nb > 0 && rects.size() == n && attempt >= 4))) {
                // others came along: keep only the new ink selected
                const QList<QRect> all = pageLineRects(scp);
                QSet<QString> mine;
                for (const QRect &r : rects) mine.insert(rectKey(r));
                QList<int> want;
                for (int i = 0; i < all.size(); ++i) if (mine.contains(rectKey(all[i]))) want << i;
                TLOG("new ink: %lld other stroke(s) selected with it, taken out", (long long)(count - rects.size()));
                setSelection(scp, all, want, {});
            }
        }
        const bool full = rects.size() == n + nb, glyphsOnly = nb > 0 && rects.size() == n && attempt >= 4;
        if (!full && !glyphsOnly) rects.clear();
        if (rects.size() != n + nb && !(glyphsOnly && rects.size() == n)) {
            if (count == 0 && (attempt == 1 || attempt == 3)) {
                const bool switched = useSelectionTool();
                TLOG("new ink not selected yet, selecting again%s", switched ? " (selection tool)" : "");
                QTimer::singleShot(switched ? 500 : 0, scp, [scp, pl, hls] { if (scp) selectInk(scp, inkAndBands(pl, hls)); });
            }
            if (attempt < 6) recordNew(scp, page, text, pl, n, removeIds, hls, attempt + 1, before, hand);
            else {
                TLOG("new ink not selected (%d of %d), not recorded: plain strokes", count, n + nb);
                g_putting = false;
            }
            return;
        }
        QList<Hl> found = hls;
        const QList<QRect> glyphs = nb > 0 ? splitBands(rects, &found) : rects;
        if (glyphs.size() != n) {
            TLOG("new ink: %lld strokes are not %d glyphs and %d bands: not recorded", (long long)rects.size(), n, nb);
            g_putting = false;
            return;
        }
        updateRecords(makeRecord(page, text, glyphs, pl.origin, pl.em, pl.weight, QString(), pl.lines, hlJson(found)), removeIds);
        TLOG("recorded: %d strokes, %lld highlight(s)", n, (long long)found.size());
        groupNewInk(scp, before, int(rects.size()), page, hand);
        QTimer::singleShot(3800, qApp, [] { g_putting = false; });   // after the selection settled and the ink was regrouped
    });
}

// Adds the strokes of pl as a text object: ends (or deletes) the selection, adds the strokes once the scene has
// settled, selects them (the user moves the new object right away; double tap edits it) and records them.
// The page is redrawn once: fullRefresh (after the edit box: it and the keyboard left ghosting) = one ghost-buster
// refresh; otherwise one updateAfterStroke.
// Highlights (hls, band rects in page units) are drawn first, under the glyphs.
void putInk(QPointer<QObject> scp, QPointer<QObject> php, QPointer<QQuickItem> dvp, int layer, bool deleteSelection,
            const Placed &pl, const QString &text, const QStringList &removeIds, bool fullRefresh = false,
            const QList<Hl> &hls = {}) {
    g_putting = true;
    g_putStarted.start();
    // addDrawingLine is ignored while a selection is active
    QSet<QString> hand;   // what the new ink replaces (for one undo, g_convs)
    if (deleteSelection) for (const QRect &r : lineRects(scp)) hand.insert(rectKey(r));
    if (deleteSelection) QMetaObject::invokeMethod(scp, "deleteSelectedItems", Q_ARG(int, layer));
    else endSelection(scp, dvp);
    QTimer::singleShot(400, scp, [=] {
        if (!scp || !php) { g_putting = false; return; }
        QSet<QString> before;
        for (const QRect &r : pageLineRects(scp)) before.insert(rectKey(r));
        const int bands = hls.isEmpty() ? 0 : addBands(scp, hls);
        const int added = addInkStrokes(scp, php, pl.strokes, pl.em, pl.weight);
        if (added > 0) {
            selectInk(scp, inkAndBands(pl, bands > 0 ? hls : QList<Hl>()));
            recordNew(scp, dvp ? dvp->property("currentPageId").toString() : scp->property("pageId").toString(), text, pl,
                      added, removeIds, bands > 0 ? hls : QList<Hl>(), 0, before, hand);
        } else {
            g_putting = false;
            if (added < 0) showHint(dvp, QStringLiteral("rmpp-textobj: this xochitl version is not supported"));
        }
        QTimer::singleShot(500, qApp, [dvp, fullRefresh] {
            if (!dvp) return;
            QMetaObject::invokeMethod(dvp, "updateAfterStroke");
            if (fullRefresh) fullRefreshLater(dvp, 0);
        });
    });
}

// One undo for a conversion or edit (g_convs): every undo and redo (aboutToUndoRedo). Only when the undo starts from a
// conversion's result (undo: its new ink on the page, its handwriting not) and ends with neither on the page, one
// more undo brings the handwriting back; redo likewise from the handwriting. Undoing further (the handwriting's own
// strokes) is left alone. A moved or resized object is not its recorded new ink any more: undoing the move first
// brings it back, then the next undo is the conversion's. undo() and redo() take effect a moment later: the page is
// looked at for up to 1.5 s.
// Where a conversion stands on the page: its result (all its new ink there, none of the old strokes it does not share),
// its start (all its handwriting or old object there, none of the new strokes it does not share: an edit keeps most
// glyph strokes as they were), or neither (no stroke of either: the state between the two undo steps).
enum class ConvState { Result, Start, Neither, Other };
ConvState convState(QObject *sc, const LastConv &c) {
    QSet<QString> page;
    for (const QRect &r : pageLineRects(sc)) page.insert(rectKey(r));
    int obj = 0, hand = 0, objOnly = 0, handOnly = 0;
    for (const QString &k : c.obj) if (page.contains(k)) { ++obj; if (!c.hand.contains(k)) ++objOnly; }
    for (const QString &k : c.hand) if (page.contains(k)) { ++hand; if (!c.obj.contains(k)) ++handOnly; }
    if (obj == c.obj.size() && handOnly == 0) return ConvState::Result;
    if (hand == c.hand.size() && objOnly == 0) return ConvState::Start;
    if (obj == 0 && hand == 0) return ConvState::Neither;
    return ConvState::Other;
}

QObject *pageScene(QString *page) {
    QQuickItem *dv = documentView();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    if (sc) *page = dv->property("currentPageId").toString();
    return sc;
}

const char *stateName(ConvState s) {
    return s == ConvState::Result ? "result" : s == ConvState::Start ? "start" : s == ConvState::Neither ? "neither" : "other";
}

// Once the user's undo (redo) is applied, from the conversion's result to neither side on the page: one more step.
// An undo right after a selection ends is ignored: an undo while the new object is still selected only ends the
// selection, so it is sent again (wasSelected); the extra step is checked and sent again too, up to 3 times. At the
// end the side that came back is selected, as after the conversion itself (undo: the handwriting; redo: the object).
void selectSide(QObject *sc, const QSet<QString> &keys) {
    QPointer<QObject> scp(sc);
    const bool switched = useSelectionTool();
    QTimer::singleShot(switched ? 500 : 0, sc, [scp, keys] {
        if (!scp) return;
        const QList<QRect> all = pageLineRects(scp);
        QList<int> want;
        for (int i = 0; i < all.size(); ++i) if (keys.contains(rectKey(all[i]))) want << i;
        setSelection(scp, all, want, {});
    });
}

void afterUndoRedo(bool undo, LastConv c, bool wasSelected, int tries = 0, int sent = 0) {
    QString page;
    QObject *sc = pageScene(&page);
    if (!sc || page != c.page) return;
    const ConvState st = convState(sc, c);
    const ConvState from = undo ? ConvState::Result : ConvState::Start, to = undo ? ConvState::Start : ConvState::Result;
    if (sent == 0 && st == from) {   // the user's step not applied yet
        if (wasSelected && tries == 3 && sc->property("selectionItemCount").toInt() == 0) {
            ownUndoRedo(sc, undo);
            TLOG("%s: only the selection ended: sent again", undo ? "undo" : "redo");
        }
        if (tries < 15) QTimer::singleShot(200, qApp, [undo, c, wasSelected, tries] { afterUndoRedo(undo, c, wasSelected, tries + 1); });
        else TLOG("%s: the page did not change in 3 s: left as is", undo ? "undo" : "redo");
        return;
    }
    // sent again only when the page did not change at all since the last send (the step was ignored): a step that
    // did change it but left the conversion between its two states was another one, and sending more redid the
    // user's later strokes (device, 595df1e8)
    static QSet<QString> sentKeys;
    const QSet<QString> nowKeys = layerKeys(sc);
    if (st == ConvState::Neither && (sent == 0 || (sent < 3 && nowKeys == sentKeys))) {
        sentKeys = nowKeys;
        ownUndoRedo(sc, undo);
        TLOG("%s: conversion %s in one step%s", undo ? "undo" : "redo", undo ? "undone" : "redone", sent ? " (sent again)" : "");
        QTimer::singleShot(800, qApp, [undo, c, sent] { afterUndoRedo(undo, c, false, 0, sent + 1); });
        return;
    }
    if (st == to) selectSide(sc, undo ? c.hand : c.obj);
    else TLOG("%s: after the step the conversion is %s", undo ? "undo" : "redo", stateName(st));
}

// Every undo and redo, from the toolbar buttons, the two-finger tap gesture or anything else, goes through the
// scene controller, which emits aboutToUndo()/aboutToRedo() before applying it: the conversion it starts from is
// found then, and looked at again once it is applied. Our own undo() and redo() calls (groupNewInk, the extra step
// above) are counted and left out: each emits the signal once; a count not used up in 3 s is dropped (a call
// xochitl ignored).
int g_ownSteps = 0;
QElapsedTimer g_ownUndo;   // started when we call undo()/redo() ourselves

void ownUndoRedo(QObject *sc, bool undo, int times) {
    if (!g_ownUndo.isValid() || g_ownUndo.elapsed() > 3000) g_ownSteps = 0;
    g_ownSteps += times;
    g_ownUndo.start();
    for (int i = 0; i < times; ++i) QMetaObject::invokeMethod(sc, undo ? "undo" : "redo");
}

// A conversion to the Text objects layer, seen from the user's layer: an undo from its result that leaves that
// layer as it was took its new ink (B, on the other layer): the layer's creation steps and the handwriting's
// deletion follow. A redo from its start that takes the handwriting away (A): the creation steps and B follow.
// While our own steps follow a user's undo or redo (layerConvStep), more presses (button or gesture) must not race
// with them (device, 4c8ab633: a quick second redo during a conversion's redo chain, and the history was cut). A
// press in the chain's direction does one of the chain's steps; any other press is taken back at once and done
// again, in order, once the chain is over (developer: "queue them, do them later").
struct StepChain {
    bool active = false, undo = false;
    int need = 0;
    QList<bool> queued;
    QElapsedTimer since;
};
StepChain g_chain;
void userStep(bool undo, bool send);

void chainStart(bool undo, int need) {
    g_chain.active = true;
    g_chain.undo = undo;
    g_chain.need = need;
    g_chain.since.start();
}

void flushQueuedSteps() {
    if (g_chain.active || g_chain.queued.isEmpty()) return;
    const bool undo = g_chain.queued.takeFirst();
    TLOG("%s pressed during a chain: done now", undo ? "undo" : "redo");
    userStep(undo, true);
    QTimer::singleShot(1200, qApp, [] { flushQueuedSteps(); });
}

void chainEnd() {
    if (!g_chain.active) return;
    g_chain.active = false;
    g_chain.need = 0;
    QTimer::singleShot(300, qApp, [] { flushQueuedSteps(); });
}

// A press while a chain runs: true if it was taken care of.
bool chainPress(QObject *sc, bool undo) {
    if (!g_chain.active) return false;
    if (g_chain.since.elapsed() > 6000) { g_chain.active = false; return false; }
    if (undo == g_chain.undo && g_chain.need > 0) {
        --g_chain.need;
        TLOG("%s pressed during the chain: one of its steps", undo ? "undo" : "redo");
        return true;
    }
    ownUndoRedo(sc, !undo);   // taken back, done again later
    g_chain.queued.append(undo);
    TLOG("%s pressed during the chain: queued", undo ? "undo" : "redo");
    return true;
}

void layerConvStep(bool undo, LastConv c, int tries = 0) {
    QString page;
    QObject *sc = pageScene(&page);
    if (!sc || page != c.page) { chainEnd(); return; }
    if (sc->property("currentLayer").toInt() != c.home) { chainEnd(); settleLayer(undo); return; }   // a layer step
    const QSet<QString> keys = layerKeys(sc);
    if (undo) {
        if (keys != c.lAfter) { chainEnd(); TLOG("undo: the user's layer changed: not the conversion"); return; }
        const int n = g_chain.need;
        g_chain.need = 0;
        if (n > 0) ownUndoRedo(sc, true, n);
        TLOG("undo: conversion undone in one step (text layer, %d step(s) more, %d by presses)", c.steps + 1, c.steps + 1 - n);
        // the handwriting back, selected as before the conversion: as soon as it is there (it showed late)
        auto back = std::make_shared<std::function<void(int)>>();
        *back = [c, back](int tries) {
            QString page;
            QObject *sc = pageScene(&page);
            if (!sc || page != c.page) { chainEnd(); return; }
            if (!layerKeys(sc).contains(c.hand) && tries < 10) {
                QTimer::singleShot(150, qApp, [back, tries] { (*back)(tries + 1); });
                return;
            }
            const bool queued = !g_chain.queued.isEmpty();
            chainEnd();
            if (!layerKeys(sc).contains(c.hand)) TLOG("undo: the handwriting is not all back");
            else if (!queued) selectSide(sc, c.hand);
        };
        QTimer::singleShot(200, qApp, [back] { (*back)(0); });
        return;
    }
    if (keys == c.lBefore && tries < 15) {   // not applied yet
        QTimer::singleShot(200, qApp, [c, tries] { layerConvStep(false, c, tries + 1); });
        return;
    }
    if (keys != c.lAfter) { chainEnd(); TLOG("redo: the user's layer is not the conversion's result: left as is"); return; }
    const int n = g_chain.need;
    g_chain.need = 0;
    if (n > 0) ownUndoRedo(sc, false, n);
    TLOG("redo: conversion redone in one step (text layer, %d step(s) more, %d by presses)", c.steps + 1, c.steps + 1 - n);
    QTimer::singleShot(900, qApp, [c] {
        // the object selected, as after the conversion; that is a layer switch, a new step, which would cut the
        // redo history: only when nothing is left to redo
        QString page;
        QObject *sc = pageScene(&page);
        const bool queued = !g_chain.queued.isEmpty();
        chainEnd();
        if (!sc || page != c.page || textSessionActive() || sc->property("currentLayer").toInt() != c.home) return;
        if (queued || sc->property("redoAvailable").toBool()) return;
        QPointer<QObject> scp(sc);
        const QRect sel = c.box.adjusted(-2, -2, 2, 2);
        startTextSession(scp, page, c.home, c.text, [scp, sel] {
            const bool switched = useSelectionTool();
            QTimer::singleShot(switched ? 500 : 0, scp, [scp, sel] {
                if (!scp) return;
                selectRect(scp, sel, 0);
                QTimer::singleShot(500, scp, [] { textSessionSelected(); });
            });
        });
    });
}

QElapsedTimer g_userStepAt;   // layer.h: layer changes right after the user's undo or redo are not a pick
extern int g_pairIdx;

// The user's undo or redo, about to be applied (send: we apply it, for one asked for while an object was selected on
// the Text objects layer).
void userStep(bool undo, bool send) {
    g_userStepAt.start();
    QString page;
    QObject *sc = pageScene(&page);
    if (!sc) return;
    const int cur = sc->property("currentLayer").toInt();
    int here = 0;
    for (int i = int(g_convs.size()) - 1; i >= 0; --i) {   // the newest conversion the step starts from
        const LastConv &c = g_convs[i];
        if (c.page != page) continue;
        ++here;
        if (c.text >= 0) {
            if (cur != c.home) continue;
            const QSet<QString> keys = layerKeys(sc);
            if (keys != (undo ? c.lAfter : c.lBefore)) continue;
            if (undo && c.pair > 0 && g_pairIdx >= c.pair) {
                // the layer switches of the selection after it are on top: they go first (settleLayer), then the
                // conversion (an extra step showed when the first undo was taken for the conversion's: 8798ea08)
                TLOG("undo: a selection's switches come first (text layer)");
                if (send) ownUndoRedo(sc, undo);
                QTimer::singleShot(600, qApp, [undo] { settleLayer(undo); });
                return;
            }
            TLOG("%s from a conversion's %s (text layer)", undo ? "undo" : "redo", undo ? "result" : "start");
            chainStart(undo, c.steps + 1);
            const LastConv cc = c;
            QTimer::singleShot(undo ? 600 : 300, qApp, [undo, cc] { layerConvStep(undo, cc); });
            if (send) ownUndoRedo(sc, undo);
            return;
        }
        const ConvState st = convState(sc, c);
        if (st != (undo ? ConvState::Result : ConvState::Start)) {
            if (here == 1) TLOG("%s: the newest conversion on this page is at its %s", undo ? "undo" : "redo", stateName(st));
            continue;
        }
        const bool sel = sc->property("selectionItemCount").toInt() > 0;
        TLOG("%s from a conversion's %s%s", undo ? "undo" : "redo", stateName(st), sel ? " (selected)" : "");
        QTimer::singleShot(300, qApp, [undo, c, sel] { afterUndoRedo(undo, c, sel); });
        if (send) ownUndoRedo(sc, undo);
        return;
    }
    if (here == 0 && !g_convs.isEmpty()) TLOG("%s: no conversion remembered on this page", undo ? "undo" : "redo");
    if (send) ownUndoRedo(sc, undo);
    QTimer::singleShot(600, qApp, [undo] { settleLayer(undo); });
}

void afterPendingStep(bool undo) {
    TLOG("%s asked for while the object was selected: applied now", undo ? "undo" : "redo");
    userStep(undo, true);
}

bool textSessionPending(bool undo);   // layer.h: true if a selection on the Text objects layer takes it

QElapsedTimer g_anyUserStep;   // the user's last undo or redo, also one taken by a selection (select.h: double taps)

void aboutToUndoRedo(bool undo) {
    if (g_ownSteps > 0 && g_ownUndo.isValid() && g_ownUndo.elapsed() < 3000) { --g_ownSteps; return; }
    g_ownSteps = 0;
    g_anyUserStep.start();
    {
        QString page;
        QObject *sc = pageScene(&page);
        if (sc && chainPress(sc, undo)) return;
    }
    if (textSessionPending(undo)) return;
    userStep(undo, false);
}

void watchUndoRedo() {
    QQuickItem *dv = documentView();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    if (!sc || sc->property("_rmppTextobjUndo").toBool()) return;
    const int su = sc->metaObject()->indexOfSignal("aboutToUndo()"), sr = sc->metaObject()->indexOfSignal("aboutToRedo()");
    if (su < 0 || sr < 0) { TLOG("no aboutToUndo()/aboutToRedo(): one undo for a conversion is off"); return; }
    sc->setProperty("_rmppTextobjUndo", true);
    for (const bool undo : {true, false}) {
        auto *hook = new SignalHook;
        hook->setParent(sc);
        hook->fn = [undo] { aboutToUndoRedo(undo); };
        QMetaObject::connect(sc, undo ? su : sr, hook, QObject::staticMetaObject.methodCount(), Qt::DirectConnection, nullptr);
    }
}

// ---- edit box ----
const char *kEditQml = R"QML(
import QtQuick
import ark.tokens as ArkTokens
import ark.controls as ArkControls
import xofm.libs.peninput
Item {
    id: shade
    property string initial: ""
    property string title: "Edit text"
    property var penManager: null
    property int weight: 1        // 0 thin, 1 medium, 2 thick
    property int sizePct: 100     // size of the ink, % of xochitl's body text (kBodyEm)
    property int rev: 0           // bumps the preview image
    function refresh() { tobj.inkText = input.text; tobj.inkWeight = weight; tobj.inkSize = sizePct; rev++; }
    onWeightChanged: refresh()
    onSizePctChanged: refresh()
    Timer { id: typing; interval: 300; onTriggered: shade.refresh() }
    // the item that was tapped to open the box (a toolbar menu entry) may still take the focus: take it back
    Timer {
        interval: 250; running: true
        onTriggered: if (!input.activeFocus) { input.forceActiveFocus(); Qt.inputMethod.show(); }
    }
    anchors.fill: parent
    z: 10000
    // outside the box: the pen draws nothing on the page, and a tap closes the box (cancel)
    PenInputBlocker { anchors.fill: parent; manager: shade.penManager }
    MouseArea { anchors.fill: parent; onClicked: tobj.command = "inkcancel#" + Date.now() }
Rectangle {
    id: box
    width: shade.width - 120; height: 740
    x: 60; y: 90
    color: "white"; border.color: "black"; border.width: 3
    MouseArea { anchors.fill: parent }   // taps inside the box stay in the box
    Text { x: 36; y: 26; text: shade.title; font.pixelSize: 36; font.bold: true }
    Rectangle {
        id: field
        x: 36; y: 86; width: parent.width - 72; height: 260
        border.color: "black"; border.width: 2
        Flickable {
            id: flick
            anchors.fill: parent; anchors.margins: 2
            clip: true
            contentWidth: width; contentHeight: input.contentHeight + 2 * input.topPadding
            boundsBehavior: Flickable.StopAtBounds
            function ensureVisible(r) {
                if (contentY >= r.y) contentY = r.y;
                else if (contentY + height <= r.y + r.height + input.topPadding) contentY = r.y + r.height + input.topPadding - height;
            }
            TextEdit {
                id: input
                width: flick.width
                leftPadding: 18; rightPadding: 18; topPadding: 14; bottomPadding: 14
                font.pixelSize: 40
                wrapMode: TextEdit.Wrap
                text: shade.initial
                focus: true
                onCursorRectangleChanged: flick.ensureVisible(cursorRectangle)
                onTextChanged: typing.restart()
                // a cursor that does not blink: each blink was a screen update (about twice a second, as long as
                // the box is open), which on e-paper flickers (#9)
                cursorDelegate: Rectangle { width: 3; color: "black"; visible: input.activeFocus }
                Component.onCompleted: { forceActiveFocus(); cursorPosition = text.length; Qt.inputMethod.show(); shade.refresh(); }
            }
        }
    }
    Text { x: 36; y: 360; text: "Preview"; font.pixelSize: 26; color: "#555555" }
    Text {   // characters the ink font has no strokes for (they are left out)
        x: 200; y: 360; width: parent.width - 236
        horizontalAlignment: Text.AlignRight; elide: Text.ElideRight
        text: tobj.inkMissing ? "No glyphs for: " + tobj.inkMissing + "  (add the language in Settings > Languages)" : ""
        font.pixelSize: 26; color: "#555555"
    }
    Rectangle {
        x: 36; y: 400; width: parent.width - 72; height: 220
        border.color: "#999999"; border.width: 1
        clip: true
        Image {
            x: 1; y: 1; width: parent.width - 2; height: parent.height - 2
            fillMode: Image.Pad
            horizontalAlignment: Image.AlignLeft; verticalAlignment: Image.AlignTop
            cache: false
            sourceSize: Qt.size(width, height)
            source: "image://rmpptextobj/preview/" + shade.rev
        }
    }
    Row {
        x: 36; y: 636; spacing: 12
        Repeater {
            model: ["Thin", "Medium", "Thick"]
            delegate: ArkControls.Button {
                required property string modelData
                required property int index
                text: modelData
                type: shade.weight === index ? ArkTokens.Button.primary : ArkTokens.Button.secondary
                onClicked: shade.weight = index
            }
        }
        Item { width: 24; height: 1 }
        ArkControls.Button { text: "−"; type: ArkTokens.Button.secondary; onClicked: shade.sizePct = Math.max(50, shade.sizePct - 10) }
        Text { text: shade.sizePct + "%"; font.pixelSize: 32; width: 100; horizontalAlignment: Text.AlignHCenter; anchors.verticalCenter: parent.verticalCenter }
        ArkControls.Button { text: "+"; type: ArkTokens.Button.secondary; onClicked: shade.sizePct = Math.min(400, shade.sizePct + 10) }
    }
    Row {
        x: parent.width - width - 36; y: 636; spacing: 24
        ArkControls.Button { text: "Cancel"; type: ArkTokens.Button.secondary; onClicked: tobj.command = "inkcancel#" + Date.now() }
        ArkControls.Button {
            text: "OK"
            enabled: !tobj.inkMissing   // characters without glyphs: no object is made (nothing left out silently)
            onClicked: {
                Qt.inputMethod.commit();
                tobj.inkText = input.text; tobj.inkWeight = shade.weight; tobj.inkSize = shade.sizePct;
                tobj.command = "inkok#" + Date.now();
            }
        }
    }
}
}
)QML";

struct InkEdit {
    QPointer<QObject> box, sc, ph;
    QPointer<QQuickItem> dv;
    QString page;
    int layer = 0;
    double em = 60, maxW = 600;   // page units, when the box opened
    QPointF origin;               // em origin of the first line (page units)
    bool create = false;          // a new object at origin (nothing selected)
    QString text0;                // the object's text when the box opened (edit)
    int weight0 = 1;
    QStringList removeIds;        // records the edited object(s) replace
    QString joined;               // the text(s) when the box opened, as in the box
    QList<Hl> hls;                // highlights on them (ranges in joined): drawn again on the new text
    int underlines = 0;           // other marks (underlines) on the edited object(s): deleted with them
    int pct0 = 100;               // the size when the box opened, % of xochitl's body text (kBodyEm)
};
InkEdit g_edit;

// The edit box's preview: the text as it will be drawn (glyphs, weight, size, wrap), at page scale, black on white.
class InkPreviewProvider : public QQuickImageProvider {
public:
    InkPreviewProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &, QSize *size, const QSize &req) override {
        const QSize sz(req.width() > 0 ? req.width() : 1400, req.height() > 0 ? req.height() : 220);
        QImage img(sz, QImage::Format_ARGB32);
        img.fill(Qt::white);
        if (size) *size = sz;
        if (!glyphsAvailable()) return img;
        const double scale = qBound(50, g_state->value(QStringLiteral("inkSize")).toInt(), 400) / double(qMax(1, g_edit.pct0));
        const int weight = qBound(0, g_state->value(QStringLiteral("inkWeight")).toInt(), 2);
        const double em = g_edit.em * scale;
        const Placed pl = placeText(g_state->value(QStringLiteral("inkText")).toString().trimmed(), QPointF(12, 8), em,
                                    g_edit.maxW * scale);
        static const double divisor[] = {32.0, 20.0, 13.0};
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Qt::black, qBound(1.5, em / divisor[weight], 9.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (const auto &st : pl.strokes) {
            if (st.size() == 1) p.drawPoint(st[0]);
            else p.drawPolyline(st.constData(), int(st.size()));
        }
        return img;
    }
};

void closeInkEdit() {
    if (g_edit.box) g_edit.box->deleteLater();
    g_edit.box = nullptr;
    qApp->setProperty("rmppKeyboardRefresh", QVariant());
    if (QGuiApplication::inputMethod()) QGuiApplication::inputMethod()->hide();
}

// Characters of the edit box's text that no glyph source has (shown in the box; OK is off while there are any).
void updateInkMissing(const QString &text) {
    const QString missing = missingGlyphs(text);
    if (g_state->value(QStringLiteral("inkMissing")).toString() != missing) g_state->insert(QStringLiteral("inkMissing"), missing);
}

// Opens the edit box over the DocumentView for e (box, dv and page filled in here).
void showInkEditBox(QQuickItem *dv, InkEdit e, const QString &title, const QString &text, int weight) {
    closeInkEdit();
    QQmlEngine *eng = qmlEngine(dv);
    if (!eng || !dv->window()) return;
    // xochitl's pen input: its surface manager keeps the pen from drawing under the box
    QObject *pin = dv->property("penInput").value<QObject *>();
    QObject *mgr = pin ? pin->property("surfaceManager").value<QObject *>() : nullptr;
    if (!eng->imageProvider(QStringLiteral("rmpptextobj"))) eng->addImageProvider(QStringLiteral("rmpptextobj"), new InkPreviewProvider);
    // the size control starts at the object's size in % of xochitl's body text, in steps of 10
    e.pct0 = qBound(50, int(std::lround(e.em / kBodyEm * 10.0)) * 10, 400);
    g_edit = e;   // before the box: its preview reads em and maxW
    g_state->insert(QStringLiteral("inkMissing"), missingGlyphs(text));
    QObject *o = createWithState(dv, kEditQml, "InkEdit",
                                 {{QStringLiteral("initial"), text}, {QStringLiteral("penManager"), QVariant::fromValue(mgr)},
                                  {QStringLiteral("weight"), weight}, {QStringLiteral("title"), title},
                                  {QStringLiteral("sizePct"), e.pct0}});
    if (!o) { g_edit = InkEdit{}; return; }
    g_edit.box = o;
    g_edit.dv = dv;
    g_edit.page = dv->property("currentPageId").toString();
    // rmpp-refresh refreshes once the keyboard has slid in; the edit box gets one full refresh when it closes
    qApp->setProperty("rmppKeyboardRefresh", false);
}

// Edit the selected object(s) (found on the page as `found` placements of po): one object -> its text; several ->
// their texts top to bottom in one box, drawn as one object at the first one's place.
// marks: page rect indices of marks on the objects (highlights: drawn again after the edit; underlines: not).
void openObjectEdit(QQuickItem *dv, QObject *sc, QObject *ph, const PageObjects &po, QList<Placement> objs,
                    const QList<int> &marks = {}) {
    if (objs.isEmpty() || g_edit.box) return;
    std::sort(objs.begin(), objs.end(), [](const Placement &a, const Placement &b) { return a.box.top() < b.box.top(); });
    const QString pageId = dv->property("currentPageId").toString();
    g_langHint.clear();   // the text decides (a hint from an earlier conversion does not apply)
    InkEdit e;
    e.sc = sc; e.ph = ph; e.layer = sc->property("currentLayer").toInt();
    const Rec &first = po.recs[objs.first().rec];
    e.em = first.em * objs.first().k;
    e.origin = originOf(first, objs.first());
    e.maxW = qMax(810.0 - 40.0 - e.origin.x(), e.em * 8);
    QStringList texts;
    QSet<int> used;
    int base = 0;   // offset of this object's text in the joined text (code points)
    for (const Placement &p : objs) {
        const Rec &r = po.recs[p.rec];
        texts << r.text;
        // its highlights: recorded ones that show here (their strokes are among the marks), and freehand ones
        const QList<Hl> recHl = hlOf(r, &p);
        QList<bool> shown(recHl.size(), false);
        for (int m : marks) {
            if (used.contains(m)) continue;
            bool band = false;
            for (int j = 0; j < recHl.size(); ++j)
                for (const QRectF &er : recHl[j].rects)
                    if (sameBand(po.rects[m], er, p.k)) { shown[j] = true; band = true; }
            if (band) { used.insert(m); continue; }
            Hl h;
            if (isHighlightStroke(po, p, m) && rangeFromRect(r.text, pageLines(r, p), QRectF(po.rects[m]), &h.a, &h.b)) {
                h.a += base; h.b += base;
                e.hls.append(h);   // drawn before this build: colour unknown, yellow
                used.insert(m);
            }
        }
        for (int j = 0; j < recHl.size(); ++j)
            if (shown[j]) { Hl h = recHl[j]; h.a += base; h.b += base; h.rects.clear(); e.hls.append(h); }
        base += int(r.text.toUcs4().size()) + 1;
        // the record goes with its strokes, unless another copy of the object stays on this page
        int copies = 0;
        for (const Placement &q : po.found) if (q.rec == p.rec) ++copies;
        if (copies <= 1 && r.page == pageId && !e.removeIds.contains(r.id)) e.removeIds << r.id;
    }
    e.text0 = objs.size() == 1 ? first.text : QString();
    e.joined = texts.join(QLatin1Char('\n'));
    for (int m : marks) if (!used.contains(m)) ++e.underlines;
    e.weight0 = first.weight;
    TLOG("edit: %lld object(s), %lld highlight(s), %d other mark(s)", (long long)objs.size(), (long long)e.hls.size(), e.underlines);
    showInkEditBox(dv, e, objs.size() == 1 ? QStringLiteral("Edit text") : QStringLiteral("Edit text (objects joined)"),
                   texts.join(QLatin1Char('\n')), first.weight);
}

// Called from the 1.5 s timer: the notebook was closed or the page turned while the edit box was open.
void closeStaleInkEdit() {
    if (!g_edit.box) return;
    if (!g_edit.dv || !g_edit.dv->isVisible() || !g_edit.sc || g_edit.dv->property("currentPageId").toString() != g_edit.page)
        closeInkEdit();
}

// Applies the edit; true when the ink is being redrawn (putInk).
bool applyInkEdit(const InkEdit &e) {
    if (!e.sc || !e.ph || !e.dv || e.dv->property("currentPageId").toString() != e.page) return false;
    const QString text = g_state->value(QStringLiteral("inkText")).toString().trimmed();
    const int weight = qBound(0, g_state->value(QStringLiteral("inkWeight")).toInt(), 2);
    const double size = qBound(50, g_state->value(QStringLiteral("inkSize")).toInt(), 400) / double(qMax(1, e.pct0));
    if (!e.create && !e.text0.isEmpty() && text == e.text0 && weight == e.weight0 && qFuzzyCompare(size, 1.0))
        return false;   // nothing changed
    if (blankText(text)) {
        if (!e.create) {   // empty (or only spaces): the object is deleted, strokes and record
            QMetaObject::invokeMethod(e.sc, "deleteSelectedItems", Q_ARG(int, e.layer));
            endSelection(e.sc, e.dv);   // else the selection frame and its image stay on screen (device, OS 3.29)
            updateRecords({}, e.removeIds);
        }
        return false;
    }
    const QString missing = missingGlyphs(text);
    if (!missing.isEmpty()) {   // OK is off in the box then; refuse rather than leave characters out
        showHint(e.dv, QStringLiteral("No glyphs for: %1. Add the language in Settings > Languages.").arg(missing));
        return false;
    }
    const double em = e.em * size;
    Placed pl = placeText(text, e.origin, em, qMax(810.0 - 40.0 - e.origin.x(), em * 8));
    pl.weight = weight;
    if (pl.strokes.isEmpty()) return false;
    TLOG("%s: %s", e.create ? "add text object" : "edit", qPrintable(QString(text).replace(QLatin1Char('\n'), QLatin1String(" | "))));
    if (e.create) {
        // selected and recorded like any new object: needs the selection tool (with the pen, selecting does nothing)
        const bool switchTool = useSelectionTool();
        QTimer::singleShot(switchTool ? 600 : 0, qApp, [e, pl, text] {
            putInk(e.sc, e.ph, e.dv, e.layer, false, pl, text, {}, true);
        });
        return true;
    }
    const QList<Hl> hls = placeHighlights(e.hls, e.joined, text, pl.lines);
    putInk(e.sc, e.ph, e.dv, e.layer, true, pl, text, e.removeIds, true, hls);
    if (e.underlines > 0) showHint(e.dv, QStringLiteral("The marks on the text were removed with the old text. Draw them again if you like."), 5000);
    return true;
}

void finishInkEdit(bool ok) {
    const InkEdit e = g_edit;
    closeInkEdit();
    // one full refresh: putInk's after the new ink is drawn, or here once the keyboard has slid out
    if (!(ok && applyInkEdit(e))) fullRefreshLater(e.dv, 700);
}
