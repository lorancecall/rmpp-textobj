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

// Makes the selection hold exactly the page strokes `want` (indices into rects). Asynchronous (the selection
// updates after a moment); done() runs after it.
void setSelection(QPointer<QObject> scp, const QList<QRect> &rects, const QList<int> &want, std::function<void()> done = {}) {
    if (!scp || want.isEmpty()) return;
    g_adjusting = true;
    g_adjustStarted.start();
    QList<QRect> wr;
    QSet<QString> wantKeys;
    for (int i : want) { wr << rects[i]; wantKeys.insert(rectKey(rects[i])); }
    const QRect box = unite(wr).adjusted(-1, -1, 1, 1);
    selectRect(scp, box, 0);
    QTimer::singleShot(350, scp, [scp, wantKeys, done] {
        if (!scp) { g_adjusting = false; return; }
        int out = 0, kept = 0;
        for (const QRect &r : lineRects(scp)) {
            if (wantKeys.contains(rectKey(r))) continue;
            // take it out only if nothing wanted lies inside its rect (that would go out with it)
            bool encloses = false;
            for (const QRect &in : lineRects(scp, &r)) if (wantKeys.contains(rectKey(in))) encloses = true;
            if (encloses) { ++kept; continue; }
            selectRect(scp, r, 1);
            ++out;
        }
        QTimer::singleShot(350, scp, [scp, out, kept, done] {
            if (scp) TLOG("selection set: %d strokes (%d taken out, %d kept)", scp->property("selectionItemCount").toInt(), out, kept);
            if (done) done();
            QTimer::singleShot(300, qApp, [] { g_adjusting = false; });
        });
    });
}

// Takes the page strokes `drop` out of the selection.
void dropFromSelection(QPointer<QObject> scp, const QList<QRect> &rects, const QList<int> &drop, const QSet<QString> &selected) {
    if (!scp || drop.isEmpty()) return;
    g_adjusting = true;
    g_adjustStarted.start();
    QSet<QString> dropKeys;
    for (int i : drop) dropKeys.insert(rectKey(rects[i]));
    int out = 0;
    for (int i : drop) {
        // inside its rect only strokes that go out too, or that are not selected (mode 1 leaves those alone)
        bool other = false;
        for (const QRect &in : lineRects(scp, &rects[i]))
            if (!dropKeys.contains(rectKey(in)) && selected.contains(rectKey(in))) other = true;
        if (other) continue;
        selectRect(scp, rects[i], 1);
        ++out;
    }
    TLOG("lasso: %d stroke(s) of text objects taken out of the selection", out);
    QTimer::singleShot(600, qApp, [] { g_adjusting = false; });
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
bool isMark(const PageObjects &po, const Placement &p, int i) {
    if (p.members.contains(i)) return false;
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
void editSelectedObjects() {
    if (g_lastDoubleTap.isValid() && g_lastDoubleTap.elapsed() < 600) return;   // the tap overlay saw it too
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
    openObjectEdit(dv, sc, ph, po, objs, int(marks.size()));
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
// and only while the selection tool is active.
const char *kTapQml = R"QML(
import QtQuick
Item {
    id: tapArea
    property bool active: false
    anchors.fill: parent
    z: 5
    TapHandler {
        enabled: tapArea.active
        gesturePolicy: TapHandler.DragThreshold
        grabPermissions: PointerHandler.ApprovesTakeOverByAnything
        onDoubleTapped: (eventPoint, button) => tobj.command = "dtap:" + eventPoint.position.x + ":" + eventPoint.position.y + "#" + Date.now()
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
        const bool on = selectionToolActive() && !g_edit.box;
        if (g_tapOverlay->property("active").toBool() != on) g_tapOverlay->setProperty("active", on);
    }
}

// A point in DocumentView coordinates -> page coordinates (addtext.h viewToPage).
bool viewToPage(QQuickItem *dv, const QPointF &p, QPointF *out);

// Double tap at x, y (DocumentView coordinates) with the selection tool: select the text object there; if it is
// selected already, edit it.
void doubleTapAt(double x, double y) {
    if (g_edit.box || putting() || adjusting() || !selectionToolActive()) return;
    QQuickItem *dv = documentView();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    QPointF at;
    if (!sc || !viewToPage(dv, QPointF(x, y), &at)) return;
    const QString pageId = dv->property("currentPageId").toString();
    const QList<QRect> sel = lineRects(sc);
    if (!sel.isEmpty()) upgradeLegacy(sc, pageId, sel);
    const PageObjects po = findObjects(sc, pageId, true);
    const Placement *hit = nullptr;
    for (const Placement &p : po.found)
        if (p.box.adjusted(-12, -12, 12, 12).contains(at) && (!hit || p.box.width() * p.box.height() < hit->box.width() * hit->box.height()))
            hit = &p;
    if (!hit) return;
    if (!sel.isEmpty()) {
        bool only = false;
        const QList<Placement> objs = selectedObjects(po, sel, &only);
        for (const Placement &p : objs)   // selected with nothing but objects and their marks: edit
            if (p.rec == hit->rec && p.d == hit->d && only) { editSelectedObjects(); return; }
    }
    // not selected, or selected with other handwriting: select this object alone
    g_lastDoubleTap.start();
    TLOG("double tap: selecting a text object (%d strokes)", hit->matched);
    endSelection(sc, dv);
    QPointer<QObject> scp(sc);
    QPointer<QQuickItem> dvp(dv);
    const QList<QRect> rects = po.rects;
    const QList<int> members = hit->members;
    QTimer::singleShot(250, sc, [scp, dvp, rects, members, pageId] {
        setSelection(scp, rects, members, [scp, dvp, pageId] {
            // a stroke drawn across the object encloses some of its strokes and cannot be left out: editing would
            // delete it too, so say so (the edit needs the object alone, or with its marks)
            if (!scp || !dvp) return;
            const PageObjects po2 = findObjects(scp, pageId, true);
            QList<int> other;
            bool only = false;
            selectedObjects(po2, lineRects(scp), &only, &other);
            if (!other.isEmpty())
                showHint(dvp, QStringLiteral("Handwriting over this text object is selected with it. Move it off to edit the text."));
        });
    });
}

// ---- rule 3: erased objects become plain strokes ----
// From the 1.5 s timer: once the open page's strokes have changed and stayed the same for a tick, the page's
// objects are looked for; a record whose strokes are found only in part (at least half, not all) was erased in
// part: its record is dropped. An object not found at all keeps its record (deleted, or not loaded yet: undo
// brings it back).
struct PageCheck { QString page; int seen = -1, checked = -1; };
PageCheck g_check;

void checkErasedObjects() {
    if (g_edit.box || putting() || adjusting()) return;
    QQuickItem *dv = documentView();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    if (!sc) return;
    const QString pageId = dv->property("currentPageId").toString();
    if (pageId != g_check.page) g_check = PageCheck{pageId};
    const QList<QRect> rects = pageLineRects(sc);
    const int n = int(rects.size());
    const bool settled = n == g_check.seen;
    g_check.seen = n;
    if (!settled || n == g_check.checked || n == 0) return;
    g_check.checked = n;
    const QList<Rec> recs = readRecs();
    const RectIndex idx(rects);
    QStringList drop;
    for (int i = 0; i < recs.size(); ++i) {
        const Rec &r = recs[i];
        if (r.page != pageId || r.rects.isEmpty()) continue;
        QList<Placement> full;
        Placement partial;
        findRec(r, i, rects, idx, &full, &partial, scaleAllowed(r, pageId));
        if (full.isEmpty() && partial.matched >= qMax(2, (r.n + 1) / 2) && partial.matched < r.n) drop << r.id;
    }
    if (drop.isEmpty()) return;
    updateRecords({}, drop);
    TLOG("%lld text object(s) erased in part: plain strokes now", (long long)drop.size());
}
