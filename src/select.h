// ---- selection rules: a text object is always handled whole ----
// 1. Selection tool, double tap on a text object that is not selected: it is selected (whole). Double tap on a
//    selected one: the edit box opens. (The pen draws dots on a double tap as always: only the selection tool.)
// 2. A lasso that takes in part of a text object: at least half of its strokes inside -> all of it is selected;
//    fewer -> its strokes are taken out of the selection. So move, delete and copy always act on whole objects. A
//    pasted copy has the same strokes, so it is the same text object (found by its stroke pattern).
// 3. A text object with strokes erased (or partly erased) becomes plain strokes: its record is dropped.
// Marks drawn on a text object (a highlight, an underline: a stroke at least 1.5 em long and wider than tall, lying
// on the object) go with it: a selection of objects and their marks counts as "only text objects" (double tap
// edits, rmpp-hwr does not recognize the marks as handwriting). Editing redraws the text without the marks (they
// are deleted with the old strokes: xochitl deletes the selection, and a mark cannot be taken out of it alone).
// Other handwriting is never part of an object: a lasso takes it along as usual; with it in the selection, a double
// tap on an object selects that object alone (another double tap edits it).
//
// Changing the selection: addSelectionRect mode 0 selects the strokes inside a rect (replacing the selection), mode 1
// takes the strokes inside a rect out. There is no "add": a selection is rebuilt as the strokes inside the bounds of
// what it must hold, minus the other strokes in there. A stroke whose rect encloses a wanted stroke cannot be taken
// out alone; it stays selected (rare: a big loop drawn around a text object).

bool g_adjusting = false;   // our own selection change in progress
QElapsedTimer g_adjustStarted;
bool adjusting() { return g_adjusting && g_adjustStarted.isValid() && g_adjustStarted.elapsed() < 3000; }

QString rectKey(const QRect &r) { return QStringLiteral("%1,%2,%3,%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()); }

// Subtracts the rects from the selection one after another, 60 ms apart (many calls in a row: most of them were
// lost on the device), then runs done.
void subtractRects(QPointer<QObject> scp, const QList<QRect> &rs, int i, std::function<void()> done) {
    if (!scp) { if (done) done(); return; }
    if (i >= rs.size()) { if (done) done(); return; }
    selectRect(scp, rs[i], 1);
    QTimer::singleShot(60, scp, [scp, rs, i, done] { subtractRects(scp, rs, i + 1, done); });
}

// Makes the selection hold exactly the page strokes `want` (indices into rects). Asynchronous (the selection
// updates after a moment); done() runs after it.
// The selection starts as the box around the wanted strokes. Strokes that do not belong go out by subtraction
// (a stroke leaves only when it lies wholly in the subtracted rect). For one wanted stroke (a highlight over
// glyphs), four strips of its box that each miss one of its edges take out all that lies inside it first.
void setSelection(QPointer<QObject> scp, const QList<QRect> &rects, const QList<int> &want, std::function<void()> done = {}) {
    if (!scp || want.isEmpty()) return;
    g_adjusting = true;
    g_adjustStarted.start();
    QList<QRect> wr;
    QSet<QString> wantKeys;
    for (int i : want) { wr << rects[i]; wantKeys.insert(rectKey(rects[i])); }
    const QRect box = unite(wr).adjusted(-1, -1, 1, 1);
    selectRect(scp, box, 0);
    auto finish = [scp, done](int out, int kept) {
        QTimer::singleShot(350, scp, [scp, out, kept, done] {
            if (scp) TLOG("selection set: %d strokes (%d taken out, %d kept)", scp->property("selectionItemCount").toInt(), out, kept);
            if (done) done();
            QTimer::singleShot(300, qApp, [] { g_adjusting = false; });
        });
    };
    // the pass over what is still selected and does not belong, stroke by stroke
    auto strokePass = [scp, wantKeys, finish] {
        if (!scp) { g_adjusting = false; return; }
        int out = 0, kept = 0;
        QList<QRect> sub;
        const QList<QRect> cur = lineRects(scp);
        for (const QRect &r : cur) {
            if (wantKeys.contains(rectKey(r))) continue;
            // take it out only if nothing wanted lies inside its rect (that would go out with it); decided from
            // the rects, as an area query leaves out selected strokes
            bool encloses = false;
            const QRect ra = r.adjusted(-1, -1, 1, 1);   // rounded rects: 1 unit bigger to take the stroke out
            for (const QRect &in : cur) if (wantKeys.contains(rectKey(in)) && ra.contains(in)) encloses = true;
            if (encloses) { ++kept; continue; }
            sub << ra;
            ++out;
        }
        subtractRects(scp, sub, 0, [finish, out, kept] { finish(out, kept); });
    };
    QTimer::singleShot(350, scp, [scp, wr, strokePass] {
        if (!scp) { g_adjusting = false; return; }
        if (wr.size() != 1) { strokePass(); return; }
        const QRect w = wr.first();
        const QList<QRect> strips = {w.adjusted(-1, 2, 1, 1), w.adjusted(-1, -1, 1, -2), w.adjusted(2, -1, 1, 1),
                                     w.adjusted(-1, -1, -2, 1)};
        subtractRects(scp, strips, 0, [scp, strokePass] { QTimer::singleShot(300, scp, strokePass); });
    });
}

// Takes the page strokes `drop` out of the selection.
void dropFromSelection(QPointer<QObject> scp, const QList<QRect> &rects, const QList<int> &drop, const QSet<QString> &selected) {
    if (!scp || drop.isEmpty()) return;
    g_adjusting = true;
    g_adjustStarted.start();
    QSet<QString> dropKeys;
    for (int i : drop) dropKeys.insert(rectKey(rects[i]));
    // The selected strokes that stay. (An area query leaves out selected strokes, so this is decided from the
    // rects, not by asking the scene what lies inside an area.)
    QList<QRect> stay;
    for (const QRect &r : rects) if (selected.contains(rectKey(r)) && !dropKeys.contains(rectKey(r))) stay << r;
    if (stay.isEmpty()) {   // nothing else selected: end the selection
        QQuickItem *dv = documentView();
        endSelection(scp, dv);
        TLOG("lasso: only part of a text object: selection ended");
        QTimer::singleShot(600, qApp, [] { g_adjusting = false; });
        return;
    }
    // One subtraction over all of them when no stroke that stays lies inside (many quick ones get lost on the
    // device); else stroke by stroke.
    QList<QRect> dr;
    for (int i : drop) dr << rects[i];
    const QRect box = unite(dr).adjusted(-1, -1, 1, 1);
    bool inside = false;
    for (const QRect &r : stay) if (box.contains(r)) inside = true;
    QList<QRect> sub;
    if (!inside) sub << box;
    else for (int i : drop) {
        // the reported rects are rounded: a stroke is only taken out by a rect 1 unit bigger
        const QRect ri = rects[i].adjusted(-1, -1, 1, 1);
        bool other = false;   // a stroke that stays inside its rect would go out with it
        for (const QRect &r : stay) if (ri.contains(r)) other = true;
        if (other) continue;
        sub << ri;
    }
    TLOG("lasso: %lld stroke(s) of text objects taken out of the selection", (long long)(inside ? sub.size() : drop.size()));
    subtractRects(scp, sub, 0, [] { QTimer::singleShot(600, qApp, [] { g_adjusting = false; }); });
}

// A legacy record (rmpp-hwr, no rects) whose strokes are exactly the selection: give it its rects now.
// Also when the object was highlighted since: strokes spanning much of the selection's width (marks) are left out
// first (glyph strokes never are that wide).
void upgradeLegacy(QObject *sc, const QString &pageId, const QList<QRect> &sel) {
    if (sel.isEmpty()) return;
    const QRect all = unite(sel);
    QList<int> widths;
    for (const QRect &r : sel) widths << r.width();
    std::sort(widths.begin(), widths.end());
    const int med = widths[widths.size() / 2];
    QList<QRect> glyphs;
    for (const QRect &r : sel)
        if (!(r.width() >= 0.4 * all.width() && r.width() >= 2.5 * med && r.width() >= 1.5 * r.height()) || sel.size() < 3) glyphs << r;
    const QJsonArray recs = readRecords();
    for (const QList<QRect> &cand : {sel, glyphs}) {
        const QRect sr = unite(cand);
        if (sr.isEmpty()) continue;
        for (int i = int(recs.size()) - 1; i >= 0; --i) {
            const Rec r = recFrom(recs[i].toObject());
            if (!r.rects.isEmpty() || r.page != pageId || r.n != cand.size() || r.h <= 0) continue;
            if (qAbs(r.w / r.h - double(sr.width()) / sr.height()) / (r.w / r.h) >= 0.05) continue;
            const double k = sr.width() / qMax(1.0, r.w);
            const QPointF origin = QPointF(sr.topLeft()) + QPointF(r.ox, r.oy) * k;
            updateRecords(makeRecord(r.page, r.text, cand, origin, r.em * k, r.weight, r.id), {r.id});
            TLOG("record from rmpp-hwr now has its strokes (%d%s)", r.n, cand.size() < sel.size() ? ", marks left out" : "");
            return;
        }
        if (glyphs.size() == sel.size()) return;
    }
}

// A double tap on a legacy object (no rects yet, rmpp-textobj#5): the strokes around the tap, grown by nearness
// (a few gap sizes, from a letter gap to a word gap), are offered to upgradeLegacy, which takes them only when
// count and aspect match a legacy record of the page. Returns whether a record got its rects.
bool upgradeLegacyAt(QObject *sc, const QString &pageId, const QList<QRect> &page, const QPointF &at) {
    bool any = false;
    for (const QJsonValue &v : readRecords()) {
        const Rec r = recFrom(v.toObject());
        if (r.rects.isEmpty() && r.page == pageId) { any = true; break; }
    }
    if (!any || page.isEmpty()) return false;
    QList<int> seed;
    for (int i = 0; i < page.size(); ++i) if (page[i].adjusted(-12, -12, 12, 12).contains(at.toPoint())) seed << i;
    if (seed.isEmpty()) return false;
    QList<int> hs;
    for (int i : seed) hs << page[i].height();
    std::sort(hs.begin(), hs.end());
    const int med = qMax(4, hs[hs.size() / 2]);
    for (double f : {0.35, 0.7, 1.1}) {
        const int g = qMax(2, int(med * f));
        QSet<int> in(seed.begin(), seed.end());
        for (bool grew = true; grew;) {
            grew = false;
            for (int i = 0; i < page.size(); ++i) {
                if (in.contains(i)) continue;
                const QRect ri = page[i].adjusted(-g, -g, g, g);
                for (int j : in) if (ri.intersects(page[j])) { in.insert(i); grew = true; break; }
            }
        }
        QList<QRect> cand;
        for (int i : in) cand << page[i];
        const QString was = QString::fromUtf8(QJsonDocument(readRecords()).toJson(QJsonDocument::Compact));
        upgradeLegacy(sc, pageId, cand);
        if (QString::fromUtf8(QJsonDocument(readRecords()).toJson(QJsonDocument::Compact)) != was) return true;
    }
    return false;
}

// Rule 2, after a selection was made (by the user): whole objects only.
void applyLassoRule() {
    if (adjusting() || putting() || g_edit.box || !selectionToolActive()) return;
    QQuickItem *dv = documentView();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    if (!sc || sc->property("selectionItemCount").toInt() <= 0) return;
    const QString pageId = dv->property("currentPageId").toString();
    const QList<QRect> sel = lineRects(sc);
    if (sel.isEmpty()) return;
    upgradeLegacy(sc, pageId, sel);
    const PageObjects po = findObjects(sc, pageId, true);
    QHash<QString, int> selCount;
    for (const QRect &r : sel) selCount[rectKey(r)]++;
    QSet<QString> selected(selCount.keyBegin(), selCount.keyEnd());
    QSet<int> selIdx;
    for (int i = 0; i < po.rects.size(); ++i) if (selected.contains(rectKey(po.rects[i]))) selIdx.insert(i);
    QList<int> add, drop;
    for (const Placement &p : po.found) {
        int in = 0;
        for (int m : p.members) if (selIdx.contains(m)) ++in;
        if (in == 0 || in == p.matched) continue;
        if (2 * in >= p.matched) { for (int m : p.members) if (!selIdx.contains(m)) add << m; }
        else for (int m : p.members) if (selIdx.contains(m)) drop << m;
    }
    if (add.isEmpty() && drop.isEmpty()) return;
    TLOG("lasso took part of text objects: %lld stroke(s) added, %lld taken out", (long long)add.size(), (long long)drop.size());
    if (add.isEmpty()) { dropFromSelection(sc, po.rects, drop, selected); return; }
    QSet<int> want = selIdx;
    for (int m : add) want.insert(m);
    for (int m : drop) want.remove(m);
    setSelection(sc, po.rects, QList<int>(want.begin(), want.end()));
}

QTimer *g_lassoTimer = nullptr;

// The open page's selection count changes: rule 2 once it has settled (a lasso selection updates in steps).
void watchSelection() {
    QQuickItem *dv = documentView();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    if (!sc || sc->property("_rmppTextobjWatch").toBool()) return;
    const int sig = sc->metaObject()->indexOfSignal("selectionItemCountChanged()");
    if (sig < 0) return;
    sc->setProperty("_rmppTextobjWatch", true);
    if (!g_lassoTimer) {
        g_lassoTimer = new QTimer(qApp);
        g_lassoTimer->setSingleShot(true);
        g_lassoTimer->setInterval(400);
        QObject::connect(g_lassoTimer, &QTimer::timeout, qApp, [] { applyLassoRule(); });
    }
    auto *hook = new SignalHook;
    hook->setParent(sc);
    hook->fn = [] { if (!adjusting() && !putting()) g_lassoTimer->start(); };
    QMetaObject::connect(sc, sig, hook, QObject::staticMetaObject.methodCount(), Qt::DirectConnection, nullptr);
}

// ---- rule 1: double tap ----
QElapsedTimer g_lastDoubleTap;

// The selection is exactly whole text objects: those objects (else empty).
// A mark on object p (highlight, underline): not one of its strokes, at least 1.5 em long, wider than tall, and
// mostly (80%) on the object.
// Also a recorded highlight (src/highlight.h: a band, or a highlighter stroke not snapped yet) of any length.
bool isMark(const PageObjects &po, const Placement &p, int i) {
    if (p.members.contains(i)) return false;
    if (isBand(po, p, i)) return true;
    const QRect &r = po.rects[i];
    const double em = po.recs[p.rec].em * p.k;
    if (r.width() < 1.5 * em || r.width() < 1.5 * r.height()) return false;
    const QRectF on = QRectF(r).intersected(p.box.adjusted(-0.25 * em, -0.25 * em, 0.25 * em, 0.25 * em));
    return on.width() * on.height() >= 0.8 * r.width() * r.height();
}

// The whole text objects in the selection. onlyObjects: the selection is those objects and marks on them, nothing
// else. other: the page strokes selected that are neither.
QList<Placement> selectedObjects(const PageObjects &po, const QList<QRect> &sel, bool *onlyObjects = nullptr,
                                 QList<int> *other = nullptr, QList<int> *marks = nullptr) {
    QSet<QString> keys;
    for (const QRect &r : sel) keys.insert(rectKey(r));
    QSet<int> selIdx;
    for (int i = 0; i < po.rects.size(); ++i) if (keys.contains(rectKey(po.rects[i]))) selIdx.insert(i);
    QList<Placement> out;
    QSet<int> covered;
    for (const Placement &p : po.found) {
        int in = 0;
        for (int m : p.members) if (selIdx.contains(m)) ++in;
        if (in == p.matched) { out << p; for (int m : p.members) covered.insert(m); }
    }
    QList<int> rest, mk;
    for (int i : selIdx) {
        if (covered.contains(i)) continue;
        bool mark = false;
        for (const Placement &p : out) if (isMark(po, p, i)) mark = true;
        (mark ? mk : rest) << i;
    }
    if (onlyObjects) *onlyObjects = !out.isEmpty() && rest.isEmpty();
    if (other) *other = rest;
    if (marks) *marks = mk;
    return out;
}

// A double tap on the selection (xochitl's selection handler): edit the selected object(s).
void editSelectedObjects(bool now = false) {
    if (!now && g_lastDoubleTap.isValid() && g_lastDoubleTap.elapsed() < 600) return;   // the tap overlay saw it too
    g_lastDoubleTap.start();
    if (g_edit.box || putting()) return;
    QQuickItem *dv = documentView();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    QObject *ph = dv ? dv->property("penHandler").value<QObject *>() : nullptr;
    if (!sc || !ph) return;
    const QList<QRect> sel = lineRects(sc);
    if (sel.isEmpty()) return;
    upgradeLegacy(sc, dv->property("currentPageId").toString(), sel);
    const PageObjects po = findObjects(sc, dv->property("currentPageId").toString(), true);
    bool only = false;
    QList<int> marks;
    const QList<Placement> objs = selectedObjects(po, sel, &only, nullptr, &marks);
    if (!only) { TLOG("double tap: the selection is not (only) text objects"); return; }
    openObjectEdit(dv, sc, ph, po, objs, marks);
}

// xochitl's selection handler areas emit doubleClicked on a double tap on the selection (pen and finger).
void watchDoubleTap() {
    QQuickItem *h = findAnywhere([](QQuickItem *i) { return classStarts(i, "SceneSelectionHandler"); });
    if (!h) return;
    for (QQuickItem *k : h->childItems()) {
        if (!k->inherits("QQuickMouseArea") || k->property("_rmppTextobjTap").toBool()) continue;
        const int sig = k->metaObject()->indexOfSignal("doubleClicked(QQuickMouseEvent*)");
        if (sig < 0) continue;
        k->setProperty("_rmppTextobjTap", true);
        auto *hook = new SignalHook;
        hook->setParent(k);
        hook->fn = [] { QTimer::singleShot(0, qApp, [] { editSelectedObjects(); }); };
        QMetaObject::connect(k, sig, hook, QObject::staticMetaObject.methodCount(), Qt::DirectConnection, nullptr);
    }
}

// Double taps anywhere on the page, seen without taking them from xochitl: a TapHandler only watches (passive grab),
// with any tool. Finger only.
const char *kTapQml = R"QML(
import QtQuick
Item {
    id: tapArea
    property bool active: false
    anchors.fill: parent
    // above the pen tool's own input (with the pen tool it saw no taps at z 5); it only watches (passive grab)
    z: 9000
    // finger only: with the pen it kept the selection tool's own pen input (menu taps, moving the selection) from
    // xochitl, seen on the device (OS 3.29). Pen double taps on a selection still reach xochitl's handler.
    TapHandler {
        enabled: tapArea.active
        acceptedDevices: PointerDevice.TouchScreen
        gesturePolicy: TapHandler.DragThreshold
        grabPermissions: PointerHandler.ApprovesTakeOverByAnything
        // two taps within 450 ms and 60 px, counted here: the TapHandler's own double tap missed some with the
        // pen tool (developer, 8798ea08)
        property real lastX: -1000
        property real lastY: -1000
        property double lastT: 0
        onTapped: (eventPoint, button) => {
            // a palm (large contact) is not a tap
            const d = eventPoint.ellipseDiameters
            if (d && (d.width > 45 || d.height > 45)) { lastT = 0; return }
            const p = eventPoint.position, now = Date.now()
            if (now - lastT < 450 && Math.abs(p.x - lastX) < 60 && Math.abs(p.y - lastY) < 60) {
                lastT = 0
                tobj.command = "dtap:" + p.x + ":" + p.y + "#" + now
            } else {
                lastT = now; lastX = p.x; lastY = p.y
                tobj.command = "stap:" + p.x + ":" + p.y + "#" + now   // a single tap (object mode only)
            }
        }
    }
}
)QML";
QPointer<QObject> g_tapOverlay;

void installTapOverlay() {
    QQuickItem *dv = documentView();
    if (!dv) return;
    if (!g_tapOverlay || g_tapOverlay->parent() != dv) {
        if (g_tapOverlay) g_tapOverlay->deleteLater();
        g_tapOverlay = createWithState(dv, kTapQml, "TapArea");
    }
    if (g_tapOverlay) {
        const bool on = !g_edit.box;   // any tool (developer, #3): a finger double tap edits the object there
        if (g_tapOverlay->property("active").toBool() != on) g_tapOverlay->setProperty("active", on);
    }
}

// A point in DocumentView coordinates -> page coordinates (addtext.h viewToPage).
bool viewToPage(QQuickItem *dv, const QPointF &p, QPointF *out);

// Double tap at x, y (DocumentView coordinates): select the text object there and edit it. Not found on the current
// layer but on one of the Text objects layer's known boxes (layer.h): that layer is made current (one undo group with
// the step before it), and the object is selected and edited there (onText). Where two or more objects overlap: a
// short message, nothing else.
int textObjectsAt(const QString &page, const QPointF &at, QRectF *box);   // layer.h
int textLayer(QObject *sc);                                              // layer.h
void switchLayer(QObject *sc, int to);                                   // layer.h
void waitFor(QPointer<QObject> scp, std::function<bool()> cond, std::function<void(bool)> next, int tries = 0);   // layer.h

void backToWriting(QObject *sc, const char *why);   // layer.h
void toolChanged();                                 // layer.h
void requestBoxes();                                // layer.h

// Our own tap sequence (a double or single tap: tool, layer, selection, edit box) is under way: a tool change seen
// meanwhile is ours, not the user's (the tool flickered off while a double tap turned it on, device c67d0d58). The
// sequence ends by its own steps (TapSeqEnd when it returns with nothing pending, endTapSeq in its last callback);
// the tool is then looked at once. The 5 s cap is only a fail-safe against a sequence that never ended.
bool g_tapSeq = false, g_tapSeqAsync = false;
QElapsedTimer g_tapSeqStarted;
bool tapSequence() { return g_tapSeq && g_tapSeqStarted.isValid() && g_tapSeqStarted.elapsed() < 5000; }
void endTapSeq() {
    if (!g_tapSeq) return;
    g_tapSeq = false;
    g_tapSeqAsync = false;
    QTimer::singleShot(0, qApp, [] { toolChanged(); requestBoxes(); });
}
struct TapSeqEnd {
    ~TapSeqEnd() { if (!g_tapSeqAsync) endTapSeq(); }
};
// a continuation of the sequence (a callback): it ends the sequence when it returns, unless it goes on again
struct TapSeqStep {
    TapSeqStep() { g_tapSeqAsync = false; }
    ~TapSeqStep() { if (!g_tapSeqAsync) endTapSeq(); }
};

// edit false: a single tap on the Text objects layer: the object there selected (no edit box), nothing there: back
// to the writing layer.
void doubleTapNow(double x, double y, bool onText = false, bool edit = true) {
    if (g_edit.box || putting() || adjusting()) {
        TLOG("double tap: busy (edit box %d, putting %d, adjusting %d)", g_edit.box ? 1 : 0, putting() ? 1 : 0, adjusting() ? 1 : 0);
        return;
    }
    if (!g_tapSeq) g_tapSeqStarted.start();
    g_tapSeq = true;
    g_tapSeqAsync = false;
    TapSeqEnd seqEnd;
    QQuickItem *dv = documentView();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    QPointF at;
    if (!sc || !viewToPage(dv, QPointF(x, y), &at)) { TLOG("double tap: no page point"); return; }
    const QString pageId = dv->property("currentPageId").toString();
    const QList<QRect> sel = lineRects(sc);
    if (!sel.isEmpty()) upgradeLegacy(sc, pageId, sel);
    PageObjects po = findObjects(sc, pageId, true);
    auto hitAt = [&]() -> const Placement * {
        const Placement *h = nullptr;
        for (const Placement &p : po.found)
            if (p.box.adjusted(-12, -12, 12, 12).contains(at) && (!h || p.box.width() * p.box.height() < h->box.width() * h->box.height()))
                h = &p;
        return h;
    };
    const Placement *hit = hitAt();
    if (!hit && upgradeLegacyAt(sc, pageId, po.rects, at)) {   // an object from rmpp-hwr 0.2-0.4: it has its rects now
        po = findObjects(sc, pageId, true);
        hit = hitAt();
    }
    // Nothing changes (tool or layer) unless the tap is on a text object: a palm or finger resting while writing
    // was taken for a double tap, and the tool switched to the selection tool under the pen (developer, 6ad906ef).
    if (!hit) {
        const int t = textLayer(sc), cur = sc->property("currentLayer").toInt();
        if (!edit) { backToWriting(sc, "a tap on no object"); return; }
        if (onText) { TLOG("double tap: no text object here on the text layer either"); return; }
        if (t < 0 || t == cur) { TLOG("double tap: not on a text object"); return; }
        const int n = textObjectsAt(pageId, at, nullptr);
        if (n == 0) { TLOG("double tap: not on a text object"); return; }
        if (n > 1) { showHint(dv, QStringLiteral("Text objects overlap here.")); return; }
        g_lastDoubleTap.start();
        TLOG("double tap: to the text layer for the object there");
        useSelectionTool();
        switchLayer(sc, t);
        QPointer<QObject> scp(sc);
        g_tapSeqAsync = true;
        waitFor(scp, [scp, t] { return scp->property("currentLayer").toInt() == t; }, [x, y](bool ok) {
            if (!ok) { endTapSeq(); return; }
            QTimer::singleShot(150, qApp, [x, y] { g_tapSeqAsync = false; doubleTapNow(x, y, true); });
        });
        return;
    }
    {   // two or more objects right here: which one is meant is not clear (the 12 px margin alone is not an overlap:
        // two objects side by side both have it at the gap, device 2351692b)
        // (placements sharing strokes are one object: an undone conversion's record matching the same strokes)
        QList<QSet<int>> here;
        for (const Placement &p : po.found) {
            if (!p.box.contains(at)) continue;
            QSet<int> ms;
            for (int m : p.members) if (m >= 0) ms.insert(m);
            bool same = false;
            for (const QSet<int> &h : here) if (h.intersects(ms)) same = true;
            if (!same) here << ms;
        }
        if (here.size() > 1) { showHint(dv, QStringLiteral("Text objects overlap here.")); return; }
    }
    if (!selectionToolActive()) {   // on an object, another tool: the selection tool first (selecting needs it)
        const bool switched = useSelectionTool();
        TLOG("double tap: on a text object, the selection tool %s", switched ? "turned on" : "could not be turned on");
        if (switched) {
            g_tapSeqAsync = true;
            QTimer::singleShot(500, qApp, [x, y, onText, edit] { g_tapSeqAsync = false; doubleTapNow(x, y, onText, edit); });
        }
        return;
    }
    if (!sel.isEmpty()) {
        bool only = false;
        const QList<Placement> objs = selectedObjects(po, sel, &only);
        for (const Placement &p : objs)   // selected with nothing but objects and their marks: edit
            if (p.rec == hit->rec && p.d == hit->d && only) { if (edit) editSelectedObjects(); return; }
    }
    // not selected, or selected with other handwriting: select this object alone
    g_lastDoubleTap.start();
    TLOG("double tap: selecting a text object (%d strokes)", hit->matched);
    // another object's strokes within this one's box: one rect would take them too. A stroke counts by its centre
    // (boxes of objects side by side touch or overlap a little: every tap said "overlap", device 2351692b)
    // Strokes of the object itself do not count, also when another record matches them (the same text converted
    // again where an undone conversion was: every tap said "overlap", device effa3783).
    bool crossed = false;
    const QRectF inner = hit->box.adjusted(2, 2, -2, -2);
    const QSet<int> own(hit->members.cbegin(), hit->members.cend());
    for (const Placement &p : po.found) {
        if (&p == hit) continue;
        for (int m : p.members)
            if (m >= 0 && m < po.rects.size() && !own.contains(m) && inner.contains(QRectF(po.rects[m]).center())) {
                crossed = true;
                const QRect r = po.rects[m];
                TLOG("double tap: crossed: record %d's stroke %d,%d %dx%d in record %d's box %.0f,%.0f %.0fx%.0f",
                     p.rec, r.x(), r.y(), r.width(), r.height(), hit->rec, hit->box.x(), hit->box.y(),
                     hit->box.width(), hit->box.height());
                break;
            }
        if (crossed) break;
    }
    if (onText && !crossed) {   // nothing but objects on the Text objects layer: one rect selects it, then the edit box
        QPointer<QObject> scp(sc);
        selectRect(sc, hit->box.toAlignedRect().adjusted(-2, -2, 2, 2), 0);
        if (edit) {
            g_tapSeqAsync = true;
            QTimer::singleShot(250, sc, [scp] {
                TapSeqStep step;
                if (scp && scp->property("selectionItemCount").toInt() > 0) editSelectedObjects(true);
            });
        }
        return;
    }
    endSelection(sc, dv);
    QPointer<QObject> scp(sc);
    QPointer<QQuickItem> dvp(dv);
    const QList<QRect> rects = po.rects;
    const QList<int> members = hit->members;
    g_tapSeqAsync = true;
    QTimer::singleShot(250, sc, [scp, dvp, rects, members, pageId, edit] {
        if (!scp) { endTapSeq(); return; }
        setSelection(scp, rects, members, [scp, dvp, pageId, edit] {
            TapSeqStep step;
            // a stroke drawn across the object encloses some of its strokes and cannot be left out: editing would
            // delete it too, so say so (the edit needs the object alone, or with its marks)
            if (!scp || !dvp) return;
            const PageObjects po2 = findObjects(scp, pageId, true);
            QList<int> other;
            bool only = false;
            selectedObjects(po2, lineRects(scp), &only, &other);
            if (!other.isEmpty()) {
                // not exactly this object: the mixed selection is not left behind (moving it mixed two objects'
                // strokes, device f1b714c2)
                const bool onT = scp->property("currentLayer").toInt() == textLayer(scp);
                if (onT) QMetaObject::invokeMethod(scp, "clearSelectedItems");
                showHint(dvp, onT ? QStringLiteral("Text objects overlap here.")
                                  : QStringLiteral("Handwriting over this text object is selected with it. Move it off to edit the text."));
            }
            else if (edit) editSelectedObjects(true);   // a double tap edits the object right away (developer, #3)
        });
    });
}

// A finger double tap from the overlay. The two-finger (undo) and three-finger (redo) tap gestures are seen as a
// double tap too, and starting a selection then cleared the redo history (device, 0d009cd3): wait a moment, and it
// is not a double tap if an undo or redo came with it.
void doubleTapAt(double x, double y) {
    // taps on the toolbar (its undo and redo buttons, pressed quickly) are not on the page
    if (x < 130 && findAnywhere([](QQuickItem *i) { return i->objectName() == QLatin1String("undoButton") && i->isVisible(); }))
        return;
    TLOG("double tap seen at %.0f, %.0f", x, y);
    QTimer::singleShot(350, qApp, [x, y] {
        if (g_anyUserStep.isValid() && g_anyUserStep.elapsed() < 800) { TLOG("double tap: an undo/redo gesture, left alone"); return; }
        auto go = std::make_shared<std::function<void(int)>>();
        *go = [x, y, go](int tries) {   // the first tap's selection may still be settling (single tap, below)
            if (adjusting() && tries < 6) { QTimer::singleShot(300, qApp, [go, tries] { (*go)(tries + 1); }); return; }
            doubleTapNow(x, y);
        };
        (*go)(0);
    });
}

// A single finger tap (developer, #3). Object mode only (the Text objects layer current): the object there is
// selected, a tap on no object goes back to the writing layer. On other layers nothing (a palm or finger while
// writing). The first finger of an undo/redo gesture also arrives as a tap: left alone when an undo or redo came with
// it (selecting would make xochitl spend that undo on the selection, probed).
void singleTapAt(double x, double y) {
    if (x < 130 && findAnywhere([](QQuickItem *i) { return i->objectName() == QLatin1String("undoButton") && i->isVisible(); }))
        return;
    QQuickItem *dv = documentView();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    if (!sc || g_edit.box || putting() || adjusting()) return;
    const int t = textLayer(sc);
    if (t < 0 || sc->property("currentLayer").toInt() != t) return;
    QTimer::singleShot(250, qApp, [x, y] {
        if (g_anyUserStep.isValid() && g_anyUserStep.elapsed() < 600) return;
        if (g_lastDoubleTap.isValid() && g_lastDoubleTap.elapsed() < 600) return;
        QQuickItem *dv2 = documentView();
        QObject *sc2 = dv2 ? dv2->property("sceneController").value<QObject *>() : nullptr;
        if (!sc2 || g_edit.box || putting() || adjusting() || sc2->property("currentLayer").toInt() != textLayer(sc2)) return;
        TLOG("single tap at %.0f, %.0f on the text layer", x, y);
        doubleTapNow(x, y, true, false);
    });
}

// ---- highlights drawn on objects (src/highlight.h) ----
// A highlighter stroke found on an object: the record, where the object is, the stroke, the highlight (its index in
// the record's list) and its bands (page units).
struct HlCapture {
    QString recId;
    QPointF d;
    double k = 1;
    QString strokeKey;
    int index = 0;
    Hl hl;
    QList<QRectF> bands;
};

// Replaces the freehand highlighter strokes of caps by bands: the selection tool for a moment (to select and delete
// the strokes), the bands drawn, the tool in use before tapped again, and the bands' rects recorded.
void snapHighlights(QQuickItem *dv, QObject *sc, const QList<HlCapture> &caps) {
    if (caps.isEmpty() || !g_state->value(QStringLiteral("snapHighlights")).toBool()) return;
    QQuickItem *prev = activeToolButton();
    if (!prev) { TLOG("highlight recorded; no tool button to come back to: not snapped"); return; }
    TLOG("snapping %lld highlight(s); back to %s after", (long long)caps.size(), qPrintable(prev->objectName()));
    g_putting = true;
    g_putStarted.start();
    QPointer<QObject> scp(sc);
    QPointer<QQuickItem> dvp(dv), prevp(prev);
    const int layer = sc->property("currentLayer").toInt();
    QSet<QString> keys;
    for (const HlCapture &c : caps) keys.insert(c.strokeKey);
    auto giveUp = [scp, dvp, prevp](const char *why) {
        TLOG("highlight not snapped: %s", why);
        if (scp) endSelection(scp, dvp);
        QTimer::singleShot(300, qApp, [prevp] { if (prevp) tapItem(prevp); g_putting = false; });
    };
    useSelectionTool();
    QTimer::singleShot(600, sc, [=] {
        if (!scp) { g_putting = false; return; }
        const QList<QRect> rects = pageLineRects(scp);
        QList<int> want;
        for (int i = 0; i < rects.size(); ++i) if (keys.contains(rectKey(rects[i]))) want << i;
        if (want.size() != keys.size()) { giveUp("the stroke is gone"); return; }
        setSelection(scp, rects, want, [=] {
            if (!scp) { g_putting = false; return; }
            QSet<QString> sel;
            for (const QRect &r : lineRects(scp)) sel.insert(rectKey(r));
            if (sel != keys) { giveUp("the stroke could not be selected alone"); return; }
            QMetaObject::invokeMethod(scp, "deleteSelectedItems", Q_ARG(int, layer));
            QTimer::singleShot(400, scp, [=] {
                if (!scp) { g_putting = false; return; }
                QList<Hl> draw;
                for (const HlCapture &c : caps) { Hl h = c.hl; h.rects = c.bands; draw << h; }
                const int drawn = addBands(scp, draw);
                QTimer::singleShot(300, qApp, [prevp, dvp] {
                    if (prevp) tapItem(prevp);
                    if (dvp) QMetaObject::invokeMethod(dvp, "updateAfterStroke");
                });
                QTimer::singleShot(1200, qApp, [=] {
                    // the bands' rects into the records (a highlight whose band did not come keeps the deleted
                    // stroke's rect: the next check drops it)
                    if (scp && drawn > 0) {
                        const QList<QRect> now = pageLineRects(scp);
                        const QList<Rec> recs = readRecs();
                        QHash<QString, QList<Hl>> changed;
                        for (const HlCapture &c : caps) {
                            QList<QRectF> found;
                            for (const QRectF &e : c.bands)
                                for (const QRect &q : now)
                                    if (sameBand(q, e, 1.0)) { found << QRectF((QPointF(q.topLeft()) - c.d) / c.k, QSizeF(q.size()) / c.k); break; }
                            if (found.isEmpty()) continue;
                            if (!changed.contains(c.recId))
                                for (const Rec &r : recs) if (r.id == c.recId) changed[c.recId] = hlOf(r);
                            QList<Hl> &hl = changed[c.recId];
                            if (c.index < hl.size()) hl[c.index].rects = found;
                        }
                        for (auto it = changed.cbegin(); it != changed.cend(); ++it) setRecordHl(it.key(), hlJson(it.value()));
                        TLOG("highlight snapped: %d band(s)", drawn);
                    } else if (drawn < 0) {
                        TLOG("highlight bands: this xochitl version is not supported");
                    }
                    g_putting = false;
                });
            });
        });
    });
}

// After the page's strokes changed (settled): highlights whose strokes are gone are dropped from their records; new
// highlighter strokes on objects (the highlighter is in use) are recorded and snapped.
void checkHighlights(QQuickItem *dv, QObject *sc, const QString &pageId, const QSet<QString> &newKeys) {
    const PageObjects po = findObjects(sc, pageId, true);
    if (po.found.isEmpty()) return;
    // gone: none of a highlight's strokes on any copy of the object
    QHash<int, QSet<int>> shown;   // record index -> highlights seen
    QSet<int> seen;
    for (const Placement &p : po.found) {
        const QList<Hl> hls = hlOf(po.recs[p.rec], &p);
        if (hls.isEmpty()) continue;
        seen.insert(p.rec);
        for (int j = 0; j < hls.size(); ++j)
            for (const QRectF &e : hls[j].rects)
                for (const QRect &q : po.rects)
                    if (sameBand(q, e, p.k)) { shown[p.rec].insert(j); break; }
    }
    for (int ri : seen) {
        const QList<Hl> hls = hlOf(po.recs[ri]);
        QList<Hl> keep;
        for (int j = 0; j < hls.size(); ++j) if (hls[j].rects.isEmpty() || shown[ri].contains(j)) keep << hls[j];
        if (keep.size() == hls.size()) continue;
        setRecordHl(po.recs[ri].id, hlJson(keep));
        TLOG("%lld highlight(s) erased", (long long)(hls.size() - keep.size()));
    }
    // new highlighter strokes
    int color = kHighlighterYellow;
    uint argb = 0;
    if (newKeys.isEmpty() || !highlighterInUse(dv->property("penHandler").value<QObject *>(), &color, &argb)) return;
    QList<HlCapture> caps;
    QHash<QString, QList<Hl>> recHl;
    for (int i = 0; i < po.rects.size(); ++i) {
        if (!newKeys.contains(rectKey(po.rects[i]))) continue;
        for (const Placement &p : po.found) {
            if (isBand(po, p, i)) break;
            const Rec &r = po.recs[p.rec];
            HlCapture c;
            if (!isHighlightStroke(po, p, i) || !rangeFromRect(r.text, pageLines(r, p), QRectF(po.rects[i]), &c.hl.a, &c.hl.b))
                continue;
            if (!recHl.contains(r.id)) recHl[r.id] = hlOf(r);
            c.recId = r.id; c.d = p.d; c.k = p.k;
            c.strokeKey = rectKey(po.rects[i]);
            c.hl.color = color; c.hl.argb = argb;
            c.bands = bandRects(r.text, pageLines(r, p), c.hl.a, c.hl.b);
            Hl stored = c.hl;   // shown by the freehand stroke until it is snapped
            stored.rects = {QRectF((QPointF(po.rects[i].topLeft()) - p.d) / p.k, QSizeF(po.rects[i].size()) / p.k)};
            c.index = int(recHl[r.id].size());
            recHl[r.id].append(stored);
            caps << c;
            break;
        }
    }
    if (caps.isEmpty()) return;
    for (auto it = recHl.cbegin(); it != recHl.cend(); ++it) setRecordHl(it.key(), hlJson(it.value()));
    TLOG("%lld highlight(s) drawn on text objects (colour %d)", (long long)caps.size(), color);
    snapHighlights(dv, sc, caps);
}

// ---- rule 3: erased objects become plain strokes ----
// From the 1.5 s timer: once the open page's strokes have changed and stayed the same for a tick, the page's
// objects are looked for; a record whose strokes are found only in part (at least half, not all) was erased in
// part: its record is dropped. An object not found at all keeps its record (deleted, or not loaded yet: undo
// brings it back). Then the highlights (checkHighlights): the strokes new since the last check, none on a page
// just opened.
struct PageCheck {
    QString page;
    QSet<QString> seen, checked;
    bool once = false;
};
PageCheck g_check;

// Not while a page is being opened: xochitl crashed (SEGV right after "Lookup of layer idx failed") when the
// periodic page query ran during the first second of opening a document, 5 times on 2026-11-22. A page counts as
// settled 3 s after the scene controller or the page last changed.
bool pageSettled(QObject *sc, const QString &pageId) {
    static QPointer<QObject> lastSc;
    static QString lastPage;
    static QElapsedTimer since;
    if (lastSc != sc || lastPage != pageId || !since.isValid()) {
        lastSc = sc;
        lastPage = pageId;
        since.start();
        return false;
    }
    return since.elapsed() > 3000;
}

void checkErasedObjects() {
    if (g_edit.box || putting() || adjusting()) return;
    QQuickItem *dv = documentView();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    if (!sc || !dv->isVisible()) return;
    const QString pageId = dv->property("currentPageId").toString();
    if (pageId.isEmpty() || !pageSettled(sc, pageId)) return;
    if (pageId != g_check.page) { g_check = PageCheck{}; g_check.page = pageId; }
    const QList<QRect> rects = pageLineRects(sc);
    QSet<QString> keys;
    for (const QRect &r : rects) keys.insert(rectKey(r));
    const bool settled = keys == g_check.seen;
    g_check.seen = keys;
    if (!settled || (g_check.once && keys == g_check.checked) || rects.isEmpty()) return;
    QSet<QString> added;
    if (g_check.once) for (const QString &k : keys) if (!g_check.checked.contains(k)) added.insert(k);
    g_check.checked = keys;
    g_check.once = true;
    const QList<Rec> recs = readRecs();
    const RectIndex idx(rects);
    // Strokes of an object found whole do not count for another record: the same text converted again at the same
    // place (the first conversion undone) shares glyph strokes with it, which looked like a partly erased object.
    QList<Placement> partials;
    QList<int> partialRec;
    QSet<int> whole;   // page rect indexes of objects found whole
    for (int i = 0; i < recs.size(); ++i) {
        const Rec &r = recs[i];
        if (r.page != pageId || r.rects.isEmpty()) continue;
        QList<Placement> full;
        Placement partial;
        findRec(r, i, rects, idx, &full, &partial, scaleAllowed(r, pageId));
        for (const Placement &f : full) for (int m : f.members) if (m >= 0) whole.insert(m);
        if (full.isEmpty() && partial.matched > 0) { partials << partial; partialRec << i; }
    }
    QStringList drop;
    for (int j = 0; j < partials.size(); ++j) {
        const Rec &r = recs[partialRec[j]];
        int own = 0;
        for (int m : partials[j].members) if (m >= 0 && !whole.contains(m)) ++own;
        if (own >= qMax(2, (r.n + 1) / 2) && partials[j].matched < r.n) drop << r.id;
    }
    if (!drop.isEmpty()) {
        updateRecords({}, drop);
        TLOG("%lld text object(s) erased in part: plain strokes now", (long long)drop.size());
    }
    checkHighlights(dv, sc, pageId, added);
}
