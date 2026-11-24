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

// ---- one press for our multi-step actions (docs/text-layer.md, "Undo and redo") ----
// xochitl makes every call a step of its own (deleting the handwriting, adding the object, making the Text objects
// layer, switching layers). Each of our actions is remembered as a group of n steps; an undo or redo pressed while that
// group is on top of xochitl's history sends the other n - 1 steps right away, inside aboutToUndo/aboutToRedo (emitted
// inside xochitl's undo()/redo(), before the step: probed). A group is on top only while nothing else happened: any
// activity marks the groups unknown (fail-safe: the next press is then a plain single step), and an undo also checks
// the page against the fingerprint taken after the group. Our own undo()/redo() calls are told by a re-entrancy count.
struct Group {
    QString page;
    int n = 1;
    QString post;   // pageSig once the group was done ("" not taken yet)
    QList<QRectF> boxes;   // text objects it added (layer.h textlayer.json: dropped on undo, back on redo)
};
QList<Group> g_undoGroups, g_redoGroups;
bool g_groupsKnown = false;
int g_own = 0;   // > 0 inside our own undo()/redo() calls

// layer.h
int textLayer(QObject *sc);
bool layerApi(QObject *sc);
void ensureTextLayer(QPointer<QObject> scp, std::function<void(bool, int)> done);
QSet<QString> layerKeys(QObject *sc);
void rememberTextBox(const QString &page, const QRectF &box);
QString pageSig(QObject *sc);
bool freePlace(const QRectF &box, const QList<QRectF> &others, QPointF *offset);
QList<QRectF> textBoxesOf(const QString &page);
void forgetTextBox(const QString &page, const QRectF &box);
void requestBoxes();
void switchLayer(QObject *sc, int to);
void switchLayerWith(QObject *sc, int to, bool stepBefore);
void repaintLayer(QObject *sc, int layer);
int textObjectsAt(const QString &page, const QPointF &at, QRectF *box);
void waitFor(QPointer<QObject> scp, std::function<bool()> cond, std::function<void(bool)> next, int tries);

QObject *pageScene(QString *page) {
    QQuickItem *dv = documentView();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    if (sc) *page = dv->property("currentPageId").toString();
    return sc;
}

void ownUndoRedo(QObject *sc, bool undo, int times = 1) {
    ++g_own;
    for (int i = 0; i < times; ++i) QMetaObject::invokeMethod(sc, undo ? "undo" : "redo", Qt::DirectConnection);
    --g_own;
}

// Something that may be a step of its own happened: the groups are no longer known to be on top.
void groupsUnknown(const char *why) {
    if (g_groupsKnown || !g_undoGroups.isEmpty() || !g_redoGroups.isEmpty()) TLOG("one press: %s: groups dropped", why);
    g_groupsKnown = false;
    g_undoGroups.clear();
    g_redoGroups.clear();
}

// A group of n steps just done on top of the history. merge: it joins the group under it if that one is still on top
// (a layer switch goes with the action next to it: one press undoes both). The fingerprint is taken a moment later.
// sigBefore: the page as it was before this group's own steps (a layer switch changes the fingerprint itself, so the
// group under it is matched against the page before the switch; device 2026-11-24: the switch after a tap off an
// object never merged, and its group of 2 took one step of the conversion or edit under it).
void pushGroup(QObject *sc, const QString &page, int n, bool merge, const QList<QRectF> &boxes = {},
               const QString &sigBefore = QString()) {
    if (!g_groupsKnown) { g_undoGroups.clear(); g_redoGroups.clear(); }
    QList<QRectF> bs = boxes;
    if (merge && !g_undoGroups.isEmpty() && g_undoGroups.last().page == page
        && (g_undoGroups.last().post.isEmpty() || g_undoGroups.last().post == pageSig(sc)
            || (!sigBefore.isEmpty() && g_undoGroups.last().post == sigBefore))) {
        const Group under = g_undoGroups.takeLast();   // the step "before" the switch is that group
        n += under.n - 1;
        bs += under.boxes;
    }
    g_undoGroups.append(Group{page, n, QString(), bs});
    if (g_undoGroups.size() > 50) g_undoGroups.removeFirst();
    g_redoGroups.clear();
    g_groupsKnown = true;
    TLOG("one press: a group of %d step(s)", n);
    const int idx = int(g_undoGroups.size()) - 1;
    QPointer<QObject> scp(sc);
    // selected strokes are left out of the page's stroke list, so the fingerprint is taken with nothing selected (a
    // conversion leaves its object selected; an empty fingerprint is not checked)
    auto take = std::make_shared<std::function<void(int)>>();
    *take = [scp, idx, take](int tries) {
        // no longer on top (a move pushed over it while the object was still selected): left unchecked
        if (!scp || idx != g_undoGroups.size() - 1 || !g_undoGroups[idx].post.isEmpty()) return;
        if (scp->property("selectionItemCount").toInt() == 0) { g_undoGroups[idx].post = pageSig(scp); return; }
        if (tries < 150) QTimer::singleShot(400, qApp, [take, tries] { (*take)(tries + 1); });
    };
    QTimer::singleShot(400, qApp, [take] { (*take)(0); });
}

int g_putSteps = 0;   // steps of the action being put before its new ink is added (handwriting deleted, layer made)

// After a conversion, on the Text objects layer (its strokes readable now): the object is checked against the other
// objects there (the known boxes used before may be old). Overlapping: pushed to the nearest free place in the same
// undo group (a move of the selection, applied: one more step); no room near: the whole conversion is undone. Then
// the object is left selected.
extern bool g_pushing;   // layer.h
void placeOnTextLayer(QPointer<QObject> scp, const QString &page, const QRectF &box, const QSet<QString> &newKeys, int text) {
    // the conversion is done once this ends: putting() off at once (it stayed on for ~4 s, and a move, tap or double
    // tap right after a conversion was ignored: device 064e18a3)
    if (!scp || scp->property("currentLayer").toInt() != text) { g_putting = false; return; }
    const QList<QRect> all = pageLineRects(scp);
    QList<int> mine;
    QList<QRectF> others;
    for (int i = 0; i < all.size(); ++i) {
        if (newKeys.contains(rectKey(all[i]))) mine << i;
        else others << QRectF(all[i]);
    }
    if (mine.size() != newKeys.size()) {   // not found exactly: select by its box, unchecked
        TLOG("convert: the object not found exactly on the text layer (%lld of %lld): not checked",
             (long long)mine.size(), (long long)newKeys.size());
        if (textObjectsAt(page, box.center(), nullptr) <= 1) selectRect(scp, box.toAlignedRect().adjusted(-2, -2, 2, 2), 0);
        g_putting = false;
        return;
    }
    QPointF off;
    if (!freePlace(box, others, &off)) {
        if (!g_undoGroups.isEmpty() && g_undoGroups.last().page == page) {
            const Group g = g_undoGroups.takeLast();
            ownUndoRedo(scp, true, g.n);
            g_redoGroups.append(g);
            for (const QRectF &b : g.boxes) forgetTextBox(page, b);
            TLOG("convert: no room near on the text layer: the conversion undone (%d steps)", g.n);
        } else {
            groupsUnknown("conversion without room");
            TLOG("convert: no room near on the text layer, the conversion left (its group unknown)");
        }
        if (QQuickItem *dv = documentView()) showHint(dv, QStringLiteral("No room for the text object here: other text objects are in the way."));
        g_putting = false;
        return;
    }
    setSelection(scp, all, mine, [scp, page, box, off, text] {
        g_putting = false;
        requestBoxes();
        if (!scp || off.isNull()) return;
        g_pushing = true;
        QMetaObject::invokeMethod(scp, "moveSelectedItems", Qt::DirectConnection, Q_ARG(int, text), Q_ARG(QPointF, off));
        QMetaObject::invokeMethod(scp, "applyPendingEdit", Qt::DirectConnection, Q_ARG(int, text));
        QTimer::singleShot(0, qApp, [] { g_pushing = false; });
        pushGroup(scp, page, 2, true, {box.translated(off)});   // joins the conversion's group: n = 1 + its n
        if (!g_undoGroups.isEmpty()) g_undoGroups.last().boxes.removeAll(box);
        forgetTextBox(page, box);
        rememberTextBox(page, box.translated(off));
        TLOG("convert: pushed %.0f, %.0f off another object on the text layer", off.x(), off.y());
    });
}

// One undo step for new ink (#6). addDrawingLine makes an undo step per stroke (a conversion took ~28 undos to
// bring the handwriting back); SceneController::cloneAddAndSelectItems adds a list of items as one step, centred on a
// point. So, once the new ink is recorded and selected: copy it, undo the k additions, check that exactly the new
// strokes went (else redo them and keep the ink as it is), and add the copies back in one step, where they were.
void groupNewInk(QPointer<QObject> scp, const QSet<QString> &before, int k, const QString &page = QString(),
                 const QSet<QString> &hand = {}) {
    QObject *sc = scp;
    if (!sc) return;
    if (k <= 1) { if (k == 1) pushGroup(sc, page, g_putSteps + 1, false); return; }   // one stroke: one step already
    QSet<QString> newKeys;   // the new ink's strokes (selected now): the object found again on the Text objects layer
    for (const QRect &r : lineRects(sc)) newKeys.insert(rectKey(r));
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
    TLOG("one undo step: %lld copies of %d strokes", (long long)items.size(), k);
    if (items.size() != k) {
        TLOG("one undo step: %lld items selected, %d added: left as is", (long long)items.size(), k);
        groupsUnknown("ink left as separate strokes");
        return;
    }
    QRectF box;
    void *a2[] = {&box, &items};
    sc->qt_metacall(QMetaObject::InvokeMetaMethod, bound, a2);
    if (box.isEmpty()) return;
    QMetaObject::invokeMethod(sc, "clearSelectedItems");
    // undo() right after ending a selection is ignored (device, OS 3.29.1), and takes effect a moment after it is
    // called: undo once the selection is gone, then wait until the new strokes are gone
    auto check = std::make_shared<std::function<void(int)>>();
    *check = [scp, before, k, items, box, add, check, page, newKeys](int tries) {
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
            groupsUnknown("ink left as separate strokes");
            TLOG("one undo step: the undo did not match (%d new, %d of %lld kept): redone, left as is", fresh, kept,
                 (long long)before.size());
            return;
        }
        // into the Text objects layer (made beforehand by putInk) while the user's layer stays current; an edit made on
        // the Text objects layer (it is current then) stays there
        const int text = textLayer(sc), cur = sc->property("currentLayer").toInt();
        const bool toText = text >= 0 && text != cur;
        int lay = toText ? text : cur;
        QList<std::shared_ptr<void>> its = items;
        double sc1 = 1.0;
        QPointF centre = box.center();
        void *a3[] = {nullptr, &lay, &its, &sc1, &centre};
        sc->qt_metacall(QMetaObject::InvokeMetaMethod, add, a3);
        TLOG("new ink: %d strokes in one undo step (%d checks)%s", k, tries + 1, toText ? ", on the text layer" : "");
        pushGroup(sc, page, g_putSteps + 1, false, toText ? QList<QRectF>{box} : QList<QRectF>{});
        if (!toText) { g_putting = false; requestBoxes(); }   // done (onto the Text objects layer: placeOnTextLayer ends it)
        if (toText) {
            // cloneAddAndSelectItems leaves the object selected on the Text objects layer while the writing layer is
            // current: xochitl never commits a move of that selection (device, f1b714c2: it jumped back). So the Text
            // objects layer is made current (one group with the conversion: switchLayer merges) and the object
            // selected there; choosing the pen goes back to the writing layer (layer.h toolChanged).
            rememberTextBox(page, box);
            QMetaObject::invokeMethod(sc, "clearSelectedItems");
            useSelectionTool();
            switchLayerWith(sc, text, true);   // the addition above is the step before it
            waitFor(scp, [scp, text] { return scp->property("currentLayer").toInt() == text; }, [scp, page, box, newKeys, text](bool ok) {
                if (!ok || !scp) return;
                QTimer::singleShot(150, scp, [scp, page, box, newKeys, text] { placeOnTextLayer(scp, page, box, newKeys, text); });
            }, 0);
        }
    };
    QTimer::singleShot(400, sc, [scp, k, check] {
        if (!scp) return;
        TLOG("one undo step: undoing the %d strokes", k);
        ownUndoRedo(scp, true, k);
        TLOG("one undo step: undone");
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
        bool tookOut = false;   // others taken out of the selection: grouping waits until that is done
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
                tookOut = true;
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
        QList<QRect> glyphs = nb > 0 ? splitBands(rects, &found) : rects;
        if (glyphs.size() != n && nb > 0 && rects.size() == n + nb) {
            // the count is right but a band was not within the tolerance: the strokes nearest the expected bands (centre
            // and size) are taken for them, so the object is still recorded whole (device 2026-11-24: "not recorded",
            // the sentence's words then boxed one by one)
            QList<QRect> g = rects;
            QList<Hl> f = hls;
            for (Hl &h : f) {
                QList<QRectF> got;
                for (const QRectF &e : h.rects) {
                    int best = -1;
                    double bestErr = 0;
                    for (int i = 0; i < g.size(); ++i) {
                        const double err = (centre(g[i]) - e.center()).manhattanLength() + qAbs(g[i].width() - e.width())
                                         + qAbs(g[i].height() - e.height());
                        if (best < 0 || err < bestErr) { best = i; bestErr = err; }
                    }
                    if (best >= 0) got << QRectF(g.takeAt(best));
                }
                h.rects = got;
            }
            if (g.size() == n) {
                TLOG("new ink: %d band(s) taken by the nearest strokes (not within the tolerance)", nb);
                glyphs = g;
                found = f;
            }
        }
        if (glyphs.size() != n) {
            TLOG("new ink: %lld strokes are not %d glyphs and %d bands: not recorded", (long long)rects.size(), n, nb);
            QStringList want, near;   // diagnosis: the bands expected and the strokes as tall as a band
            double hmin = 1e9;
            for (const Hl &h : hls) for (const QRectF &e : h.rects) {
                want << QStringLiteral("%1,%2 %3x%4").arg(e.x(), 0, 'f', 1).arg(e.y(), 0, 'f', 1).arg(e.width(), 0, 'f', 1).arg(e.height(), 0, 'f', 1);
                hmin = qMin(hmin, e.height());
            }
            for (const QRect &r : rects)
                if (r.height() >= 0.7 * hmin && r.width() >= r.height()) near << QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
            TLOG("new ink: bands expected %s; band-like strokes %s", qPrintable(want.join(QLatin1Char(' '))), qPrintable(near.join(QLatin1Char(' '))));
            g_putting = false;
            return;
        }
        updateRecords(makeRecord(page, text, glyphs, pl.origin, pl.em, pl.weight, pl.carryId ? carryKey(pl.carryId) : QString(),
                                 pl.lines, hlJson(found)), removeIds);
        {   // the rects a page file read will compute (strokeRect) against the ones xochitl reports: calibration
            const double w = std::lround(qBound(1.5, pl.em / (pl.weight == 0 ? 32.0 : pl.weight == 2 ? 13.0 : 20.0), 9.0) * 6) / 4.0;
            int same = 0, off = 0;
            double worst = 0;
            for (const QList<QPointF> &s : pl.strokes) {
                const QRect c = strokeRect(s, w);
                double best = 1e9;
                for (const QRect &g : glyphs) {
                    if ((QRectF(g).center() - QRectF(c).center()).manhattanLength() > 3) continue;
                    best = qMin(best, double(qMax(qAbs(g.width() - c.width()), qAbs(g.height() - c.height()))));
                }
                if (best < 1e8) { (best == 0 ? same : off)++; worst = qMax(worst, best); }
            }
            TLOG("rect check: %d same, %d off (worst %.0f) of %lld strokes, pen %.2f", same, off, worst,
                 (long long)pl.strokes.size(), w);
            for (int si = 0; si < qMin<qsizetype>(4, pl.strokes.size()); ++si) {   // points' bounds against xochitl's rect
                const QRectF f = pointsFrame({pl.strokes[si]});
                for (const QRect &g : glyphs)
                    if ((QRectF(g).center() - f.center()).manhattanLength() <= 4)
                        TLOG("rect check: points %.2f,%.2f %.2fx%.2f, xochitl %d,%d %dx%d", f.x(), f.y(), f.width(),
                             f.height(), g.x(), g.y(), g.width(), g.height());
            }
        }
        TLOG("recorded: %d strokes, %lld highlight(s)", n, (long long)found.size());
        // the copies are taken from the selection: with others still being taken out of it, they came out one too
        // many and the ink was left as plain strokes (device, a59a3c3e)
        const int k = int(rects.size());
        QTimer::singleShot(tookOut ? 1500 : 0, qApp, [scp, before, k, page, hand] { if (scp) groupNewInk(scp, before, k, page, hand); });
        QTimer::singleShot(tookOut ? 5300 : 3800, qApp, [] { g_putting = false; });   // after the selection settled and the ink was regrouped
    });
}

// Adds the strokes of pl as a text object: ends (or deletes) the selection, adds the strokes once the scene has
// settled, selects them (the user moves the new object right away; double tap edits it) and records them.
// The page is redrawn once: fullRefresh (after the edit box: it and the keyboard left ghosting) = one ghost-buster
// refresh; otherwise one updateAfterStroke.
// Highlights (hls, band rects in page units) are drawn first, under the glyphs.
void putInkNow(QPointer<QObject> scp, QPointer<QObject> php, QPointer<QQuickItem> dvp, bool deleteSelection,
               const Placed &pl, const QString &text, const QStringList &removeIds, bool fullRefresh, const QList<Hl> &hls);

// The Text objects layer is made first, when the user's layer is current and the page has none (every conversion is
// then: [the layer's steps], handwriting deleted, object added). A layer change ends the selection (probed), so the
// handwriting is selected again by its strokes and checked before it is deleted.
void putInk(QPointer<QObject> scp, QPointer<QObject> php, QPointer<QQuickItem> dvp, int /*layer*/, bool deleteSelection,
            const Placed &pl, const QString &text, const QStringList &removeIds, bool fullRefresh = false,
            const QList<Hl> &hls = {}) {
    const int cur = scp->property("currentLayer").toInt(), t = textLayer(scp);
    if (t >= 0 && cur != t) {   // onto the Text objects layer: kept apart from its objects, or not converted
        QRectF box;
        for (const QList<QPointF> &st : pl.strokes) for (const QPointF &q : st) box |= QRectF(q, QSizeF(0.01, 0.01));
        const QString page = dvp ? dvp->property("currentPageId").toString() : QString();
        QPointF off;
        if (!box.isEmpty() && !freePlace(box.adjusted(-3, -3, 3, 3), textBoxesOf(page), &off)) {
            TLOG("convert: no room near for the text object: nothing changed");
            showHint(dvp, QStringLiteral("No room for the text object here: other text objects are in the way."));
            return;
        }
        if (!off.isNull()) {
            TLOG("convert: pushed %.0f, %.0f off another text object", off.x(), off.y());
            Placed moved = pl;
            for (QList<QPointF> &st : moved.strokes) for (QPointF &q : st) q += off;
            moved.origin += off;
            for (LineBox &l : moved.lines) l.o += off;
            putInk(scp, php, dvp, cur, deleteSelection, moved, text, removeIds, fullRefresh, hls);
            return;
        }
    }
    g_putting = true;
    g_putStarted.start();
    g_putSteps = deleteSelection ? 1 : 0;
    if (t >= 0 || !layerApi(scp) || (t >= 0 && cur == t)) {
        putInkNow(scp, php, dvp, deleteSelection, pl, text, removeIds, fullRefresh, hls);
        return;
    }
    QSet<QString> hand;
    if (deleteSelection) for (const QRect &r : lineRects(scp)) hand.insert(rectKey(r));
    QMetaObject::invokeMethod(scp, "clearSelectedItems");
    ensureTextLayer(scp, [=](bool ok, int steps) {
        if (!scp) { g_putting = false; return; }
        g_putStarted.start();
        g_putSteps += steps;
        if (!ok) TLOG("text layer: not made: the object goes on the current layer");
        if (!deleteSelection || hand.isEmpty()) { putInkNow(scp, php, dvp, deleteSelection, pl, text, removeIds, fullRefresh, hls); return; }
        const QList<QRect> all = pageLineRects(scp);
        QList<int> want;
        for (int i = 0; i < all.size(); ++i) if (hand.contains(rectKey(all[i]))) want << i;
        const bool switched = useSelectionTool();
        QTimer::singleShot(switched ? 500 : 0, scp, [=] {
            if (!scp) { g_putting = false; return; }
            setSelection(scp, all, want, [=] {
                if (!scp) { g_putting = false; return; }
                QSet<QString> sel;
                for (const QRect &r : lineRects(scp)) sel.insert(rectKey(r));
                if (sel != hand) {   // not exactly the handwriting: nothing is deleted
                    TLOG("convert: the handwriting could not be selected again (%lld of %lld): stopped",
                         (long long)sel.size(), (long long)hand.size());
                    QMetaObject::invokeMethod(scp, "clearSelectedItems");
                    groupsUnknown("conversion stopped");
                    g_putting = false;
                    showHint(dvp, QStringLiteral("The handwriting could not be converted. Select it again and convert."));
                    return;
                }
                putInkNow(scp, php, dvp, deleteSelection, pl, text, removeIds, fullRefresh, hls);
            });
        });
    });
}

void putInkNow(QPointer<QObject> scp, QPointer<QObject> php, QPointer<QQuickItem> dvp, bool deleteSelection,
               const Placed &pl, const QString &text, const QStringList &removeIds, bool fullRefresh, const QList<Hl> &hls) {
    const int layer = scp->property("currentLayer").toInt();
    // addDrawingLine is ignored while a selection is active
    QSet<QString> hand;
    if (deleteSelection) for (const QRect &r : lineRects(scp)) hand.insert(rectKey(r));
    if (deleteSelection) QMetaObject::invokeMethod(scp, "deleteSelectedItems", Q_ARG(int, layer));
    else endSelection(scp, dvp);
    QTimer::singleShot(400, scp, [=] {
        if (!scp || !php) { g_putting = false; return; }
        QSet<QString> before;
        for (const QRect &r : pageLineRects(scp)) before.insert(rectKey(r));
        const int bands = hls.isEmpty() ? 0 : addBands(scp, hls);
        Placed cp = pl;
        cp.carryId = newCarryId();
        makeCarryRoom(&cp.strokes, carryWordCount(int(packRecord(text, cp).size())));
        const QList<QList<quint16>> words = carrySpeeds(cp.carryId, packRecord(text, cp), cp.strokes);
        if (words.isEmpty())
            TLOG("new ink: %lld strokes cannot carry the record (over 4095): an object until xochitl restarts",
                 (long long)cp.strokes.size());
        const int added = addInkStrokes(scp, php, cp.strokes, cp.em, cp.weight, words);
        if (added > 0) {
            selectInk(scp, inkAndBands(cp, bands > 0 ? hls : QList<Hl>()));
            recordNew(scp, dvp ? dvp->property("currentPageId").toString() : scp->property("pageId").toString(), text, cp,
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

QElapsedTimer g_anyUserStep;   // the user's last undo or redo (select.h: undo/redo gestures are not double taps)
// Every undo and redo (toolbar buttons, gestures).
// - Something selected: xochitl spends the press on ending the selection (scratch probe, 6 of 6 with undo() called
//   while selected; a selection cleared inside aboutToUndo is too late). Nothing else happens: the groups stay.
// - The toolbar buttons end a selection themselves on press (below, watchUndoRedo), so a button press is always a
//   real step (scratch probe, 3 of 3; without it 1 of 3).
void aboutToUndoRedo(bool undo) {
    if (g_own > 0) return;
    requestBoxes();   // read once this press is done
    g_anyUserStep.start();
    QString page;
    QObject *sc = pageScene(&page);
    if (!sc) return;
    if (sc->property("selectionItemCount").toInt() > 0) return;
    QList<Group> &from = undo ? g_undoGroups : g_redoGroups;
    QList<Group> &to = undo ? g_redoGroups : g_undoGroups;
    bool ok = g_groupsKnown && !from.isEmpty() && from.last().page == page;
    if (ok && undo && !from.last().post.isEmpty() && pageSig(sc) != from.last().post) ok = false;
    if (!ok) { groupsUnknown(undo ? "an undo of something else" : "a redo of something else"); return; }
    const Group g = from.takeLast();
    to.append(g);
    if (g.n > 1) ownUndoRedo(sc, undo, g.n - 1);
    for (const QRectF &b : g.boxes) { if (undo) forgetTextBox(page, b); else rememberTextBox(page, b); }
    TLOG("%s: %d step(s) in one press", undo ? "undo" : "redo", g.n);
}

// The toolbar's undo or redo button pressed while something is selected and not moved: the selection is ended first,
// so the click that follows is a real undo or redo. A moved or resized selection is left to xochitl (clearing it
// would drop the move; the groups are unknown after a move anyway).
void beforeButtonUndoRedo() {
    QString page;
    QObject *sc = pageScene(&page);
    if (!sc || sc->property("selectionItemCount").toInt() == 0) return;
    if (!sc->property("pendingEdit").value<QTransform>().isIdentity()) return;
    QMetaObject::invokeMethod(sc, "clearSelectedItems", Qt::DirectConnection);
    TLOG("undo/redo button: the selection ended first");
}

void watchUndoRedo() {
    QQuickItem *dv = documentView();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    if (!sc || sc->property("_rmppTextobjUndo").toBool()) return;
    const int su = sc->metaObject()->indexOfSignal("aboutToUndo()"), sr = sc->metaObject()->indexOfSignal("aboutToRedo()");
    if (su < 0 || sr < 0) { TLOG("no aboutToUndo()/aboutToRedo(): one undo for a conversion is off"); return; }
    sc->setProperty("_rmppTextobjUndo", true);
    for (const char *name : {"undoButton", "redoButton"}) {
        QQuickItem *b = findAnywhere([name](QQuickItem *i) { return i->objectName() == QLatin1String(name); });
        const int ps = b ? b->metaObject()->indexOfSignal("pressed()") : -1;
        if (ps < 0) { TLOG("no %s pressed(): a press with a selection is left to xochitl", name); continue; }
        if (b->property("_rmppTextobjPress").toBool()) continue;
        b->setProperty("_rmppTextobjPress", true);
        auto *hook = new SignalHook;
        hook->setParent(b);
        hook->fn = [] { beforeButtonUndoRedo(); };
        QMetaObject::connect(b, ps, hook, QObject::staticMetaObject.methodCount(), Qt::DirectConnection, nullptr);
    }
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
    // highlights, per character as bold in a word processor (developer, #3): [{a, b, c, argb}], UTF-16 offsets in
    // the text; the Highlight button turns it on for what is typed next, or on/off for the selected characters
    property string hlsInit: ""
    property var hls: []
    property bool hlOn: false
    property int hlColor: 3
    property int hlArgb: 0
    property string seenText: initial   // the text the ranges are for
    property int seenCursor: -1         // the cursor position the button's state was taken at
    property int edits: 0               // text changes seen (a cursor move that came with one is not a move)
    property bool paletteOpen: false
    readonly property var hlColors: [3, 4, 5, 14, 15, 16]
    function hlString() { return hls.map(h => h.a + "," + h.b + "," + h.c + "," + h.argb).join(";"); }
    function colourOf(c, argb) {
        if (c === 9 && argb) return "#" + (argb & 0xffffff).toString(16).padStart(6, "0");
        return ({3: "#fff275", 4: "#acff85", 5: "#db91e6", 14: "#8fd1ff", 15: "#ffc870", 16: "#c8c8c8"})[c] || "#fff275";
    }
    function isHl(i) { for (const h of hls) if (h.a <= i && i < h.b) return true; return false; }
    function covered(a, b) { for (let i = a; i < b; ++i) if (!isHl(i)) return false; return b > a; }
    // [a, b) off, then on in colour c (argb); touching ranges of one colour merge
    function setRange(a, b, on, c, argb) {
        let out = [];
        for (const h of hls) {
            if (h.b <= a || h.a >= b) { out.push(h); continue; }
            if (h.a < a) out.push({a: h.a, b: a, c: h.c, argb: h.argb});
            if (h.b > b) out.push({a: b, b: h.b, c: h.c, argb: h.argb});
        }
        if (on && b > a) out.push({a: a, b: b, c: c, argb: argb});
        out.sort((x, y) => x.a - y.a);
        let merged = [];
        for (const h of out) {
            const l = merged.length ? merged[merged.length - 1] : null;
            if (l && l.b === h.a && l.c === h.c && l.argb === h.argb) l.b = h.b; else merged.push(h);
        }
        hls = merged;
    }
    // the text changed: the ranges follow (text before them inserted or deleted), and what was typed takes the
    // button's state
    function textEdited(t) {
        const o = seenText;
        seenText = t;
        ++edits;
        let p = 0;
        while (p < o.length && p < t.length && o[p] === t[p]) ++p;
        let so = o.length, sn = t.length;
        while (so > p && sn > p && o[so - 1] === t[sn - 1]) { --so; --sn; }
        const del = so - p, ins = sn - p;
        let out = [];
        for (const h of hls) {
            let a = h.a <= p ? h.a : Math.max(p, h.a - del), b = h.b <= p ? h.b : Math.max(p, h.b - del);
            if (a >= p) a += ins;
            if (b > p) b += ins;
            if (b > a) out.push({a: a, b: b, c: h.c, argb: h.argb});
        }
        hls = out;
        if (ins > 0) setRange(p, p + ins, hlOn, hlColor, hlArgb);
    }
    function toggleHl() {
        console.log("[rmpp-textobj] edit box: highlight button, selection " + input.selectionStart + "-" + input.selectionEnd + ", was " + hlOn);
        const s = input.selectionStart, e = input.selectionEnd;
        if (s < e) { const on = !covered(s, e); setRange(s, e, on, hlColor, hlArgb); hlOn = on; }
        else hlOn = !hlOn;
        seenCursor = input.cursorPosition;   // the button's state holds until the cursor goes elsewhere
        console.log("[rmpp-textobj] edit box: highlight now " + hlOn + ", ranges " + hlString());
        refresh();
    }
    function pickColour(c) {
        paletteOpen = false;
        hlColor = c;
        hlArgb = 0;
        const s = input.selectionStart, e = input.selectionEnd;
        if (s < e && covered(s, e)) setRange(s, e, true, hlColor, hlArgb);   // the selected highlight recoloured
        refresh();
    }
    Component.onCompleted: {
        let out = [];
        for (const part of hlsInit.split(";")) {
            const v = part.split(",").map(Number);
            if (v.length === 4 && v[1] > v[0]) out.push({a: v[0], b: v[1], c: v[2], argb: v[3]});
        }
        hls = out;
        if (out.length) { hlColor = out[0].c; hlArgb = out[0].argb; }
    }
    function refresh() { tobj.inkText = input.text; tobj.inkWeight = weight; tobj.inkSize = sizePct; tobj.inkHls = hlString(); rev++; }
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
                onTextChanged: { shade.textEdited(text); typing.restart(); }
                // the button shows whether the character before the cursor is highlighted (typing goes on with it);
                // a text change not yet seen is left to textEdited
                // (only when the cursor went somewhere else: a tap on the button sent the same position again and
                // turned it off, device 2026-11-24)
                // (looked at a moment later: typing moves the cursor before the text changes, device 2026-11-24)
                onCursorPositionChanged: {
                    const n = shade.edits;
                    Qt.callLater(() => {
                        if (shade.edits !== n) { shade.seenCursor = input.cursorPosition; return; }   // typed
                        if (input.selectionStart !== input.selectionEnd || input.cursorPosition === shade.seenCursor) return;
                        shade.seenCursor = input.cursorPosition;
                        shade.hlOn = shade.isHl(input.cursorPosition - 1);
                    });
                }
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
        Item { width: 24; height: 1 }
        ArkControls.Button {
            objectName: "rmppTextobjHighlight"
            text: "Highlight"
            focusPolicy: Qt.NoFocus   // the text field keeps the focus and its selection
            type: shade.hlOn ? ArkTokens.Button.primary : ArkTokens.Button.secondary
            onClicked: shade.toggleHl()
        }
        Rectangle {   // the highlight colour: a tap takes the next one
            objectName: "rmppTextobjHighlightColour"
            width: 56; height: 56; anchors.verticalCenter: parent.verticalCenter
            color: shade.colourOf(shade.hlColor, shade.hlArgb); border.color: "black"; border.width: 2
            MouseArea { anchors.fill: parent; onClicked: shade.paletteOpen = !shade.paletteOpen }
        }
    }
    Rectangle {   // the colours to pick from (the highlighter's), above the colour square
        objectName: "rmppTextobjPalette"
        visible: shade.paletteOpen
        x: 760; y: 540; width: palette.width + 24; height: 80
        color: "white"; border.color: "black"; border.width: 2
        Row {
            id: palette
            x: 12; anchors.verticalCenter: parent.verticalCenter; spacing: 12
            Repeater {
                model: shade.hlColors
                delegate: Rectangle {
                    required property int modelData
                    objectName: "rmppTextobjColour" + modelData
                    width: 56; height: 56
                    color: shade.colourOf(modelData, 0)
                    border.color: "black"; border.width: shade.hlColor === modelData ? 5 : 1
                    MouseArea { anchors.fill: parent; onClicked: shade.pickColour(parent.modelData) }
                }
            }
        }
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
                tobj.inkHls = shade.hlString();
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

// The edit box's highlights ("a,b,c,argb;...", UTF-16 offsets in the box's text) as Hl ranges in code points of
// the trimmed text (the text drawn), without rects.
QList<Hl> boxHighlights(const QString &boxText, const QString &spec) {
    QList<Hl> out;
    int lead = 0;
    while (lead < boxText.size() && boxText.at(lead).isSpace()) ++lead;
    const int len = int(boxText.trimmed().toUcs4().size());
    auto cp = [&](int u16) { return int(boxText.left(qBound(0, u16, int(boxText.size()))).toUcs4().size())
                                 - int(boxText.left(lead).toUcs4().size()); };
    for (const QString &part : spec.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QStringList v = part.split(QLatin1Char(','));
        if (v.size() != 4) continue;
        Hl h;
        h.a = qBound(0, cp(v[0].toInt()), len);
        h.b = qBound(0, cp(v[1].toInt()), len);
        h.color = v[2].toInt();
        h.argb = v[3].toUInt();
        if (h.b > h.a) out.append(h);
    }
    return out;
}

// The edit box's highlights as it starts: ranges in code points of text, as "a,b,c,argb;..." in UTF-16 offsets.
QString boxHighlightSpec(const QString &text, const QList<Hl> &hls) {
    const QList<uint> u = text.toUcs4();
    auto u16 = [&](int cp) { return int(QString::fromUcs4(reinterpret_cast<const char32_t *>(u.constData()), qBound(0, cp, int(u.size()))).size()); };
    QStringList parts;
    for (const Hl &h : hls) parts << QStringLiteral("%1,%2,%3,%4").arg(u16(h.a)).arg(u16(h.b)).arg(h.color).arg(h.argb);
    return parts.join(QLatin1Char(';'));
}

QColor highlightColour(int c, uint argb) {
    if (c == 9 && argb) return QColor::fromRgb(argb | 0xff000000u);
    switch (c) {
    case 4: return QColor(0xac, 0xff, 0x85);
    case 5: return QColor(0xdb, 0x91, 0xe6);   // as the device paints it (sampled 2026-11-24)
    case 14: return QColor(0x8f, 0xd1, 0xff);
    case 15: return QColor(0xff, 0xc8, 0x70);
    case 16: return QColor(0xc8, 0xc8, 0xc8);
    default: return QColor(0xff, 0xf2, 0x75);
    }
}

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
        const QString boxText = g_state->value(QStringLiteral("inkText")).toString();
        const Placed pl = placeText(boxText.trimmed(), QPointF(12, 8), em, g_edit.maxW * scale);
        static const double divisor[] = {32.0, 20.0, 13.0};
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        // the highlights under the glyphs, as the bands will be drawn
        for (const Hl &h : boxHighlights(boxText, g_state->value(QStringLiteral("inkHls")).toString()))
            for (const QRectF &r : bandRects(boxText.trimmed(), pl.lines, h.a, h.b)) p.fillRect(r, highlightColour(h.color, h.argb));
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
    // the boxes at rest read again (none are read while the box is open): a move of the object left selected by
    // Cancel is kept apart from the others (device 2026-11-24: "no boxes read at rest", the move not checked)
    requestBoxes();
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
    const QString spec = boxHighlightSpec(text, e.hls);
    g_state->insert(QStringLiteral("inkHls"), spec);
    QObject *o = createWithState(dv, kEditQml, "InkEdit",
                                 {{QStringLiteral("initial"), text}, {QStringLiteral("penManager"), QVariant::fromValue(mgr)},
                                  {QStringLiteral("weight"), weight}, {QStringLiteral("title"), title},
                                  {QStringLiteral("sizePct"), e.pct0}, {QStringLiteral("hlsInit"), spec}});
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
    if (objs.size() > 1 && sc->property("currentLayer").toInt() == textLayer(sc)) {
        // object mode: several objects move and resize together, one is edited at a time (developer, #3); joining
        // objects into one edit stays on the other layers
        showHint(dv, QStringLiteral("Several text objects are selected: double tap one to edit it."));
        TLOG("edit: %lld objects selected on the text layer: not edited", (long long)objs.size());
        return;
    }
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
                // not in the record (drawn before a restart, on another tablet, or freehand): the colour its stroke has
                // in the page file; yellow when the file does not have it
                // a band drawn here that did not match its recorded rects: the record's highlight on these characters
                // gives the colour (device 2026-11-24: a pink band came back yellow, not yet in the page file)
                bool fromRec = false;
                for (int j = 0; j < recHl.size(); ++j)
                    if (recHl[j].a < h.b && h.a < recHl[j].b) {
                        h.color = recHl[j].color; h.argb = recHl[j].argb; shown[j] = false; fromRec = true;
                        QStringList er;
                        for (const QRectF &q : recHl[j].rects) er << QStringLiteral("%1,%2 %3x%4").arg(q.x(), 0, 'f', 0).arg(q.y(), 0, 'f', 0).arg(q.width(), 0, 'f', 0).arg(q.height(), 0, 'f', 0);
                        TLOG("edit: band %d,%d %dx%d not matched to recorded %s: colour %d from the record", po.rects[m].x(),
                             po.rects[m].y(), po.rects[m].width(), po.rects[m].height(), qPrintable(er.join(QLatin1Char(' '))), h.color);
                        break;
                    }
                if (!fromRec && fileHighlightColor(pageId, QRectF(po.rects[m]), &h.color, &h.argb))
                    TLOG("edit: a highlight taken up with its colour from the page file (%d)", h.color);
                e.hls.append(h);
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
    const QString boxText = g_state->value(QStringLiteral("inkText")).toString();
    const QString text = boxText.trimmed();
    QList<Hl> boxHls = boxHighlights(boxText, g_state->value(QStringLiteral("inkHls")).toString());
    const int weight = qBound(0, g_state->value(QStringLiteral("inkWeight")).toInt(), 2);
    const double size = qBound(50, g_state->value(QStringLiteral("inkSize")).toInt(), 400) / double(qMax(1, e.pct0));
    auto sameHls = [](QList<Hl> x, QList<Hl> y) {
        auto key = [](const Hl &h) { return QStringLiteral("%1,%2,%3,%4").arg(h.a).arg(h.b).arg(h.color).arg(h.argb); };
        QStringList kx, ky;
        for (const Hl &h : x) kx << key(h);
        for (const Hl &h : y) ky << key(h);
        kx.sort(); ky.sort();
        return kx == ky;
    };
    if (!e.create && !e.text0.isEmpty() && text == e.text0 && weight == e.weight0 && qFuzzyCompare(size, 1.0)
        && sameHls(boxHls, e.hls))
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
        QList<Hl> hls;
        for (Hl h : boxHls) { h.rects = bandRects(text, pl.lines, h.a, h.b); if (!h.rects.isEmpty()) hls << h; }
        QTimer::singleShot(switchTool ? 600 : 0, qApp, [e, pl, text, hls] {
            putInk(e.sc, e.ph, e.dv, e.layer, false, pl, text, {}, true, hls);
        });
        return true;
    }
    // the box's highlights (it keeps them with the text as it is edited)
    QList<Hl> hls;
    for (Hl h : boxHls) { h.rects = bandRects(text, pl.lines, h.a, h.b); if (!h.rects.isEmpty()) hls << h; }
    TLOG("edit: %lld highlight(s) from the box", (long long)hls.size());
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
