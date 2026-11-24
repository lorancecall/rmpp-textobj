// ---- the Text objects layer (#3) ----
// Text objects live on a layer of their own ("Text objects", at the bottom), so writing and erasing on the user's
// layer never touch them. docs/text-layer.md has the design and its history. Rules:
// - layers are found by scanning, never by a kept index: the Text objects layer by its name, the writing layer by the
//   name of the last other layer the user had current;
// - a conversion makes the Text objects layer first if the page has none, then deletes the handwriting and adds the
//   object there while the writing layer stays current (edit.h putInk);
// - the Text objects layer is made current only by the user's own action: a double tap on one of its objects (select.h)
//   or a pick in the layers menu; while it is current the selection tool is on, and choosing another tool makes the
//   writing layer current again;
// - each of our layer switches is one undo group with the step next to it (edit.h pushGroup);
// - nothing else happens when a page opens.

const char *kTextLayerName = "Text objects";

QString layerName(QObject *sc, int i) {
    QString n;
    QMetaObject::invokeMethod(sc, "layerName", Qt::DirectConnection, Q_RETURN_ARG(QString, n), Q_ARG(int, i));
    return n;
}

int textLayer(QObject *sc) {
    if (!sc) return -1;
    const int n = sc->property("layerCount").toInt();
    for (int i = 0; i < n; ++i) if (layerName(sc, i) == QLatin1String(kTextLayerName)) return i;
    return -1;
}

bool layerApi(QObject *sc) {
    const QMetaObject *mo = sc ? sc->metaObject() : nullptr;
    return mo && mo->indexOfMethod("addLayer()") >= 0 && mo->indexOfMethod("setLayerName(int,QString)") >= 0
           && mo->indexOfMethod("setCurrentLayer(int)") >= 0 && mo->indexOfMethod("layerName(int)") >= 0;
}

// The writing layer: the last layer other than the Text objects layer the user had current, by name; else the
// topmost other layer.
QString g_writingName;

int writingLayer(QObject *sc) {
    const int t = textLayer(sc), n = sc->property("layerCount").toInt();
    if (!g_writingName.isEmpty())
        for (int i = 0; i < n; ++i) if (i != t && layerName(sc, i) == g_writingName) return i;
    for (int i = n - 1; i >= 0; --i) if (i != t) return i;
    return -1;
}

int g_ownLayerSwitch = 0;   // our own setCurrentLayer calls not yet seen applied
void followBands(QObject *sc, const QString &pageId);   // below: bands follow their moved objects
// The Text objects layer was made current by the user (the layers panel), not by the add-on (a conversion putting its
// ink there, a double tap going to an object). The dashed boxes show only then (developer: a box showed on a new
// object and not on the others for a moment; the boxes are for working on that layer).
bool g_userOnText = false;

void setLayer(QObject *sc, int i) {
    if (sc->property("currentLayer").toInt() == i) return;
    ++g_ownLayerSwitch;
    QMetaObject::invokeMethod(sc, "setCurrentLayer", Q_ARG(int, i));
}

QSet<QString> layerKeys(QObject *sc) {
    QSet<QString> keys;
    for (const QRect &r : pageLineRects(sc)) keys.insert(rectKey(r));
    return keys;
}

// A fingerprint of what can be seen of the page: its id, the layers (names, order, current), the current layer's
// strokes and the typed text's length (edit.h: a group is still on top only if the page is as it was after it).
QString pageSig(QObject *sc) {
    QQuickItem *dv = documentView();
    QStringList parts{dv ? dv->property("currentPageId").toString() : QString()};
    const int n = sc->property("layerCount").toInt();
    for (int i = 0; i < n; ++i) parts << layerName(sc, i);
    parts << QString::number(sc->property("currentLayer").toInt()) << QString::number(sc->property("rootDocumentLength").toInt());
    QStringList keys = layerKeys(sc).values();
    std::sort(keys.begin(), keys.end());
    parts << QString::number(qHash(keys.join(QLatin1Char(';'))));
    return parts.join(QLatin1Char('|'));
}

// moveLayer's signature is not known from QML; found here (logged once): (int, int) is taken as (from, to).
int moveLayerMethod(QObject *sc) {
    static int idx = -2;
    if (idx != -2) return idx;
    idx = -1;
    const QMetaObject *mo = sc->metaObject();
    for (int i = 0; i < mo->methodCount(); ++i) {
        const QMetaMethod m = mo->method(i);
        if (m.name() != "moveLayer") continue;
        TLOG("text layer: %s", m.methodSignature().constData());
        if (m.methodSignature() == "moveLayer(int,int)") idx = i;
    }
    return idx;
}

// Waits (checking every 50 ms, up to 3 s) until cond holds, then next(true); on time out next(false). Layer calls
// are applied later, not inside the call (probed).
void waitFor(QPointer<QObject> scp, std::function<bool()> cond, std::function<void(bool)> next, int tries) {
    if (!scp) return;
    if (cond()) { next(true); return; }
    if (tries >= 60) { next(false); return; }
    QTimer::singleShot(50, scp, [scp, cond, next, tries] { waitFor(scp, cond, next, tries + 1); });
}

// The Text objects layer made: addLayer (on top, made current), its name, moved to the bottom (below the user's
// layers, as xochitl's Text row is), the writing layer current again. Each step is waited for. done(ok, steps).
void ensureTextLayer(QPointer<QObject> scp, std::function<void(bool, int)> done) {
    if (textLayer(scp) >= 0) { done(true, 0); return; }
    const int n0 = scp->property("layerCount").toInt();
    const QString writing = layerName(scp, scp->property("currentLayer").toInt());
    ++g_ownLayerSwitch;
    QMetaObject::invokeMethod(scp, "addLayer");
    waitFor(scp, [scp, n0] { return scp->property("layerCount").toInt() == n0 + 1; }, [scp, n0, writing, done](bool added) {
        if (!added) { TLOG("text layer: addLayer not applied"); done(false, 0); return; }
        const int t = n0;   // added on top
        QMetaObject::invokeMethod(scp, "setLayerName", Q_ARG(int, t), Q_ARG(QString, QString::fromLatin1(kTextLayerName)));
        waitFor(scp, [scp, t] { return layerName(scp, t) == QLatin1String(kTextLayerName); }, [scp, t, writing, done](bool named) {
            if (!named) { TLOG("text layer: name not applied"); done(false, 1); return; }
            const int mv = moveLayerMethod(scp);
            int steps = 2;
            if (mv >= 0 && t > 0) {
                int from = t, to = 0;
                void *a[] = {nullptr, &from, &to};
                scp->qt_metacall(QMetaObject::InvokeMetaMethod, mv, a);
                steps = 3;
            }
            const bool moved = steps == 3;
            waitFor(scp, [scp, moved] { return !moved || textLayer(scp) == 0; }, [scp, writing, steps, done](bool) {
                int w = -1;
                for (int i = 0; i < scp->property("layerCount").toInt(); ++i) if (layerName(scp, i) == writing) w = i;
                if (w < 0) { done(false, steps); return; }
                setLayer(scp, w);
                waitFor(scp, [scp, w] { return scp->property("currentLayer").toInt() == w; }, [scp, steps, done](bool back) {
                    TLOG("text layer: %s (layer %d)", back ? "added" : "added, but the writing layer is not current", textLayer(scp) + 1);
                    done(back, steps + 1);
                });
            });
        });
    });
}

// ---- where the objects on the Text objects layer are ----
// Per object, per page: added at a conversion, read again whenever that layer is current and a selection there ends
// (moves, edits, deletes). Kept in textlayer.json ({page: [[x, y, w, h], ...]}) so they hold after a restart;
// pages that no longer exist are dropped on load; at most 200 boxes per page and 500 pages; the package's purge
// removes the folder. Only a hint: a double tap goes to the layer only when it is on one of these boxes.
QHash<QString, QList<QRectF>> g_textBoxes;
bool g_textBoxesLoaded = false;

QString textBoxesPath() { return QLatin1String(kDataDir) + QStringLiteral("/textlayer.json"); }

QSet<QString> existingPages() {
    QSet<QString> pages;
    QDirIterator it(QStringLiteral("/home/root/.local/share/remarkable/xochitl"), {QStringLiteral("*.rm")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) pages.insert(QFileInfo(it.next()).completeBaseName());
    return pages;
}

void saveTextBoxes() {
    QJsonObject o;
    for (auto it = g_textBoxes.begin(); it != g_textBoxes.end(); ++it) {
        QJsonArray boxes;
        for (const QRectF &b : it.value()) boxes.append(QJsonArray{qRound(b.x()), qRound(b.y()), qRound(b.width()), qRound(b.height())});
        o.insert(it.key(), boxes);
    }
    QDir().mkpath(QLatin1String(kDataDir));
    QFile f(textBoxesPath() + QStringLiteral(".new"));
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    f.close();
    QFile::remove(textBoxesPath());
    QFile::rename(textBoxesPath() + QStringLiteral(".new"), textBoxesPath());
}

void loadTextBoxes() {
    if (g_textBoxesLoaded) return;
    g_textBoxesLoaded = true;
    QFile f(textBoxesPath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    const QSet<QString> pages = existingPages();
    int pruned = 0;
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (!pages.contains(it.key())) { ++pruned; continue; }
        QList<QRectF> boxes;
        for (const QJsonValue &v : it.value().toArray()) {
            const QJsonArray a = v.toArray();
            if (a.size() == 4) boxes << QRectF(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble());
        }
        g_textBoxes.insert(it.key(), boxes);
    }
    if (pruned) { TLOG("text layer: %d page(s) gone, their object boxes dropped", pruned); saveTextBoxes(); }
}

// Mostly the same rect (an object added again by a redo, or read again): one box.
bool sameBox(const QRectF &a, const QRectF &b) {
    const QRectF i = a.intersected(b);
    const double ia = i.width() * i.height(), u = a.width() * a.height() + b.width() * b.height() - ia;
    return u > 0 && ia / u > 0.6;
}

void rememberTextBox(const QString &page, const QRectF &box) {
    loadTextBoxes();
    QList<QRectF> &boxes = g_textBoxes[page];
    for (int i = int(boxes.size()) - 1; i >= 0; --i) if (sameBox(boxes[i], box)) boxes.removeAt(i);
    boxes << box;
    while (boxes.size() > 200) boxes.removeFirst();
    while (g_textBoxes.size() > 500) g_textBoxes.erase(g_textBoxes.begin());
    saveTextBoxes();
}

void forgetTextBox(const QString &page, const QRectF &box) {
    loadTextBoxes();
    QList<QRectF> &boxes = g_textBoxes[page];
    const qsizetype before = boxes.size();
    for (int i = int(boxes.size()) - 1; i >= 0; --i) if (sameBox(boxes[i], box)) boxes.removeAt(i);
    if (boxes.size() != before) saveTextBoxes();
}

// The Text objects layer current and settled: its objects' boxes read again.
void readTextBoxes(QObject *sc) {
    QQuickItem *dv = documentView();
    const QString page = dv ? dv->property("currentPageId").toString() : QString();
    if (page.isEmpty() || putting() || sc->property("currentLayer").toInt() != textLayer(sc)) return;
    loadTextBoxes();
    QList<QRectF> boxes;
    for (const Placement &p : findObjects(sc, page, false).found) boxes << p.box;
    if (g_textBoxes.value(page) == boxes) return;
    g_textBoxes[page] = boxes;
    saveTextBoxes();
}

QList<QRectF> textBoxesOf(const QString &page) { loadTextBoxes(); return g_textBoxes.value(page); }

// How many of the Text objects layer's known objects are at `at` (0: none; 2 or more: they overlap there).
int textObjectsAt(const QString &page, const QPointF &at, QRectF *box = nullptr) {
    loadTextBoxes();
    // distinct boxes there (duplicates of one object counted once: a false overlap, 10cf412e); boxes that hold the
    // point itself first, the 15 px margin only when none does (objects side by side, device 2351692b)
    bool strict = false;
    for (const QRectF &b : g_textBoxes.value(page)) if (b.contains(at)) strict = true;
    QList<QRectF> hits;
    for (const QRectF &b : g_textBoxes.value(page)) {
        if (!(strict ? b.contains(at) : b.adjusted(-15, -15, 15, 15).contains(at))) continue;
        bool dup = false;
        for (const QRectF &h : hits) if (sameBox(h, b)) dup = true;
        if (!dup) { hits << b; if (box) *box = b; }
    }
    return int(hits.size());
}

bool g_moved = false;   // a selection was moved or resized: one step when it ends

// ---- objects kept apart (developer, #3) ----
// The nearest place for the boxes in `moving` (one object, or a selected group moved as one block) where none of them
// overlaps any of `others` (each kept 6 px away): the offset 0 when they are free, else the smallest shift up, down,
// left or right, at most the largest member's size. False: no room near (all or nothing for a group: developer).
bool freePlace(const QList<QRectF> &moving, const QList<QRectF> &others, QPointF *offset) {
    auto hit = [&](const QPointF &d, QRectF *m, QRectF *o) {
        for (const QRectF &mv : moving)
            for (const QRectF &ot : others)
                if (mv.translated(d).intersects(ot.adjusted(-6, -6, 6, 6))) { *m = mv.translated(d); *o = ot; return true; }
        return false;
    };
    QRectF m, o;
    if (!hit(QPointF(), &m, &o)) { *offset = QPointF(); return true; }
    double reach = 0;
    for (const QRectF &mv : moving) reach = qMax(reach, qMax(mv.width(), mv.height()));
    bool found = false;
    QPointF best;
    for (int dir = 0; dir < 4; ++dir) {
        QPointF d;
        for (int step = 0; step < 40; ++step) {   // push past whatever is in the way, again until all is free
            if (!hit(d, &m, &o)) {
                if (!found || QLineF(QPointF(), d).length() < QLineF(QPointF(), best).length()) best = d;
                found = true;
                break;
            }
            const QRectF r = o.adjusted(-7, -7, 7, 7);
            if (dir == 0) d.rx() += r.right() - m.left();
            else if (dir == 1) d.rx() -= m.right() - r.left();
            else if (dir == 2) d.ry() += r.bottom() - m.top();
            else d.ry() -= m.bottom() - r.top();
            if (QLineF(QPointF(), d).length() > reach) break;
        }
    }
    *offset = best;
    return found;
}
bool freePlace(const QRectF &box, const QList<QRectF> &others, QPointF *offset) {
    return freePlace(QList<QRectF>{box}, others, offset);
}

// Strokes of no recorded object, grouped into boxes by nearness (one word of glyphs stays one box): an object whose
// record was lost still gets a box and is still kept apart (device ab6e7ac9: objects with no box).
QList<QRectF> strayClusters(const QList<QRect> &rects, const QSet<int> &covered, const QSet<QString> &only = {}) {
    QList<QRectF> boxes;
    for (int i = 0; i < rects.size(); ++i) {
        if (covered.contains(i) || (!only.isEmpty() && !only.contains(rectKey(rects[i])))) continue;
        QRectF b(rects[i]);
        for (int j = int(boxes.size()) - 1; j >= 0; --j)
            if (boxes[j].adjusted(-12, -12, 12, 12).intersects(b)) { b |= boxes[j]; boxes.removeAt(j); }
        boxes << b;
    }
    for (bool merged = true; merged;) {   // merging can make two earlier boxes touch: again until stable
        merged = false;
        for (int a = 0; a < boxes.size() && !merged; ++a)
            for (int c = a + 1; c < boxes.size(); ++c)
                if (boxes[a].adjusted(-12, -12, 12, 12).intersects(boxes[c])) {
                    boxes[a] |= boxes[c];
                    boxes.removeAt(c);
                    merged = true;
                    break;
                }
    }
    return boxes;
}

// The Text objects layer's objects as boxes, split into the selected ones and the others; strokes of no object count
// one by one. While a move is pending the selected strokes are already where they are being moved (probed).
void objectBoxes(QObject *sc, const QString &page, QList<QRectF> *selected, QList<QRectF> *others) {
    const PageObjects po = findObjects(sc, page, false);
    QSet<QString> selKeys;
    if (sc->property("selectionItemCount").toInt() > 0)
        for (const QRect &r : lineRects(sc)) selKeys.insert(rectKey(r));
    QSet<int> covered;
    for (const Placement &p : po.found) {
        int in = 0;
        for (int m : p.members) if (m >= 0) { covered.insert(m); if (selKeys.contains(rectKey(po.rects[m]))) ++in; }
        if (in > 0 && selected) *selected << p.box;
        else if (in == 0 && others) *others << p.box;
    }
    QSet<QString> straySel, strayOthers;
    for (int i = 0; i < po.rects.size(); ++i) {
        if (covered.contains(i)) continue;
        (selKeys.contains(rectKey(po.rects[i])) ? straySel : strayOthers).insert(rectKey(po.rects[i]));
    }
    if (selected && !straySel.isEmpty()) *selected << strayClusters(po.rects, covered, straySel);
    if (others && !strayOthers.isEmpty()) *others << strayClusters(po.rects, covered, strayOthers);
}

// A selection on the Text objects layer moved or resized (on lift): if it would overlap another object there, it is
// pushed to the nearest free place in the same pending edit (one step when the selection ends: probed); with no room
// near, the move is cancelled (cancelPendingEdit: no step, probed).
bool g_pushing = false;
// Positions are read only while no edit is pending (review of f028522: during a pending drag the page reports
// positions that cannot be trusted, and the push saw no overlap while the one box left showing jumped). The last
// read at rest is kept: the selected objects' boxes and the others'; a move maps the selected ones by the pending
// transform.
QList<QRectF> g_restSelected, g_restOthers;
bool g_restValid = false;
QString g_restPage;

void keepApartAfterMove(QObject *sc) {
    const int t = textLayer(sc);
    if (t < 0 || sc->property("currentLayer").toInt() != t || g_edit.box || putting()) return;
    if (sc->property("pendingEdit").value<QTransform>().isIdentity()) return;
    QQuickItem *dv = documentView();
    const QString page = dv ? dv->property("currentPageId").toString() : QString();
    if (!g_restValid || g_restPage != page || g_restSelected.isEmpty()) {
        TLOG("move on the text layer: no boxes read at rest for this selection: not checked");
        return;
    }
    const QTransform pe = sc->property("pendingEdit").value<QTransform>();
    QList<QRectF> moving;
    for (const QRectF &b : g_restSelected) moving << pe.mapRect(b);
    const QList<QRectF> others = g_restOthers;
    QRectF mu, ou, reported, area;
    for (const QRectF &b : moving) mu |= b;
    for (const QRectF &b : others) ou |= b;
    for (const QRect &r : lineRects(sc)) reported |= QRectF(r);
    const QRect all(-20000, -20000, 40000, 80000);
    const QList<QRect> areaRects = lineRects(sc, &all);
    for (const QRect &r : areaRects) area |= QRectF(r);
    TLOG("move on the text layer: pe %.2f,%.2f %.0f,%.0f; %lld moved box(es) %.0f,%.0f %.0fx%.0f (selection reported "
         "%.0f,%.0f %.0fx%.0f); %lld other(s) %.0f,%.0f %.0fx%.0f (area query %lld in %.0f,%.0f %.0fx%.0f)",
         pe.m11(), pe.m22(), pe.dx(), pe.dy(), (long long)moving.size(), mu.x(), mu.y(), mu.width(), mu.height(),
         reported.x(), reported.y(), reported.width(), reported.height(), (long long)others.size(), ou.x(), ou.y(),
         ou.width(), ou.height(), (long long)areaRects.size(), area.x(), area.y(), area.width(), area.height());
    QPointF off;
    g_pushing = true;
    if (!freePlace(moving, others, &off)) {
        // put back with no step; the selection is ended (a further move of it after a cancel is not committed:
        // probed), the user selects again
        QMetaObject::invokeMethod(sc, "cancelPendingEdit", Qt::DirectConnection);
        g_moved = false;
        QMetaObject::invokeMethod(sc, "clearSelectedItems", Qt::DirectConnection);
        if (dv) showHint(dv, QStringLiteral("No room for the text object there: put back."));
        TLOG("move on the text layer: no room near (%lld object(s)): put back", (long long)moving.size());
    } else if (!off.isNull()) {
        QMetaObject::invokeMethod(sc, "moveSelectedItems", Qt::DirectConnection, Q_ARG(int, t), Q_ARG(QPointF, off));
        TLOG("move on the text layer: %lld object(s) pushed %.0f, %.0f off others", (long long)moving.size(), off.x(), off.y());
    }
    QTimer::singleShot(0, qApp, [] { g_pushing = false; requestBoxes(); });
}

// The push runs when the selection is let go: one of SceneSelectionHandler's MouseAreas is pressed while it is
// dragged, its pressed turns false at the lift and pendingEditChanged follows (probed, 3 ms apart). penHandler's
// timeSincePenUp does not see a selection drag (probed): waiting on it held the push for 8 s, past the selection.
bool g_pushWait = false;
bool selectionPressed() {
    static QPointer<QQuickItem> cached;
    if (!cached) cached = findAnywhere([](QQuickItem *i) { return classStarts(i, "SceneSelectionHandler"); });
    QQuickItem *h = cached;
    if (!h) return false;
    for (QQuickItem *k : h->childItems())
        if (k->inherits("QQuickMouseArea") && k->property("pressed").toBool()) return true;
    return false;
}
void pushWhenReleased() {
    g_pushWait = false;
    QString pg;
    if (QObject *s = pageScene(&pg)) keepApartAfterMove(s);
    requestBoxes();
}
void watchSelectionRelease() {
    QQuickItem *h = findAnywhere([](QQuickItem *i) { return classStarts(i, "SceneSelectionHandler"); });
    if (!h) return;
    for (QQuickItem *k : h->childItems()) {
        if (!k->inherits("QQuickMouseArea") || k->property("_rmppTextobjPress").toBool()) continue;
        const int sig = k->metaObject()->indexOfSignal("pressedChanged()");
        if (sig < 0) continue;
        k->setProperty("_rmppTextobjPress", true);
        auto *hook = new SignalHook;
        hook->setParent(k);
        QPointer<QQuickItem> kp(k);
        hook->fn = [kp] {
            if (kp && !kp->property("pressed").toBool() && g_pushWait)
                QTimer::singleShot(0, qApp, [] { if (g_pushWait) pushWhenReleased(); });
        };
        QMetaObject::connect(k, sig, hook, QObject::staticMetaObject.methodCount(), Qt::DirectConnection, nullptr);
    }
}

// ---- switching ----
// To the Text objects layer for a double tap (select.h), or back to the writing layer when another tool is chosen
// there: one undo group with the step next to it.
void switchLayer(QObject *sc, int to) {
    QQuickItem *dv = documentView();
    const QString page = dv ? dv->property("currentPageId").toString() : QString();
    const bool before = sc->property("undoAvailable").toBool();
    setLayer(sc, to);
    QPointer<QObject> scp(sc);
    waitFor(scp, [scp, to] { return scp->property("currentLayer").toInt() == to; }, [scp, page, before](bool ok) {
        if (ok) pushGroup(scp, page, before ? 2 : 1, true);
        else groupsUnknown("a layer switch not applied");
    });
}

// Object mode ended by the user with nothing selected (a tap on no object, a lasso or pen tap that selected nothing):
// the writing layer current again, one undo group with the step before it (developer, #3: writing and the lasso on the
// handwriting did not work while the Text objects layer stayed current). Never from a selection merely ending: a new
// lasso and our own flows end selections too.
void backToWriting(QObject *sc, const char *why) {
    const int t = textLayer(sc), w = writingLayer(sc);
    if (t < 0 || w < 0 || sc->property("currentLayer").toInt() != t) return;
    if (sc->property("selectionItemCount").toInt() > 0) {
        // a pending move is committed first (clearing the selection would drop it: probed); it is then a step of
        // its own (g_moved: a group of 1 at selectionCleared), and the switch merges with it
        if (!sc->property("pendingEdit").value<QTransform>().isIdentity())
            QMetaObject::invokeMethod(sc, "applyPendingEdit", Qt::DirectConnection, Q_ARG(int, t));
        QMetaObject::invokeMethod(sc, "clearSelectedItems", Qt::DirectConnection);
    }
    readTextBoxes(sc);
    TLOG("text layer: %s: layer %d current for writing", why, w + 1);
    switchLayer(sc, w);
}

// The current layer changed: the Text objects layer current (by any cause) turns on the selection tool (not a step);
// another layer current is remembered as the writing layer.
void layerChanged() {
    QString page;
    QObject *sc = pageScene(&page);
    if (!sc) return;
    const bool own = g_ownLayerSwitch > 0 || putting() || tapSequence() || g_edit.box;
    if (g_ownLayerSwitch > 0) --g_ownLayerSwitch;
    const int t = textLayer(sc), cur = sc->property("currentLayer").toInt();
    if (!own) g_userOnText = t >= 0 && cur == t;   // our own switches (a move's bands followed) keep the user's choice
    requestBoxes();
    if (t >= 0 && cur == t) {
        if (!putting() && !g_edit.box && !selectionToolActive()) useSelectionTool();
        return;
    }
    g_writingName = layerName(sc, cur);
}

// Choosing another tool while the Text objects layer is current: the writing layer current first, so ink never goes
// there (the selection tool's selected property turning false).
void toolChanged() {
    QString page;
    QObject *sc = pageScene(&page);
    if (!sc || g_edit.box || putting() || tapSequence() || selectionToolActive()) return;
    const int t = textLayer(sc);
    if (t < 0 || sc->property("currentLayer").toInt() != t) return;
    const int w = writingLayer(sc);
    if (w < 0) return;
    // (our own tap sequence turning the tool on is not the user's choice: tapSequence(), select.h)
    if (sc->property("selectionItemCount").toInt() > 0) {   // ended here, a pending move kept (probed: a clear drops it)
        if (!sc->property("pendingEdit").value<QTransform>().isIdentity())
            QMetaObject::invokeMethod(sc, "applyPendingEdit", Qt::DirectConnection, Q_ARG(int, t));
        QMetaObject::invokeMethod(sc, "clearSelectedItems", Qt::DirectConnection);
    }
    readTextBoxes(sc);
    TLOG("text layer: another tool chosen on it: layer %d current for writing", w + 1);
    switchLayer(sc, w);
}


// Activity that may be a step of its own (edit.h groupsUnknown): a pen stroke (not a lasso), a move or resize, typed
// text. A selection ending on the Text objects layer reads its boxes again.
void watchLayers() {
    QQuickItem *dv = documentView();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    if (!sc || sc->property("_rmppTextobjLayers").toBool()) return;
    sc->setProperty("_rmppTextobjLayers", true);
    g_moved = false;
    g_undoGroups.clear();   // a document opened: a new history (closing one clears xochitl's: probed)
    g_redoGroups.clear();
    g_groupsKnown = false;
    auto hook = [](QObject *o, const char *sig, std::function<void()> fn) {
        const int i = o ? o->metaObject()->indexOfSignal(sig) : -1;
        if (i < 0) return;
        auto *h = new SignalHook;
        h->setParent(o);
        h->fn = fn;
        QMetaObject::connect(o, i, h, QObject::staticMetaObject.methodCount(), Qt::DirectConnection, nullptr);
    };
    hook(sc, "currentLayerChanged()", [] { QTimer::singleShot(0, qApp, [] { layerChanged(); }); });
    // A move or resize is one step of its own, committed when the selection ends (probed): it becomes a group of 1
    // on top, so the groups under it stay (device, 2351692b: a move after a conversion dropped them, and the
    // conversion then took one press per step).
    hook(sc, "pendingEditChanged()", [] {   // also emitted, unchanged, during undo: only a real transform counts
        QString p;
        QObject *s = pageScene(&p);
        // not gated on putting(): that stays on for seconds after a conversion, and a move right after one was
        // missed (device, effa3783); our own steps never transform a selection
        if (!s || adjusting() || g_own > 0 || g_pushing) return;
        // a move committed or put back (the pending edit back to identity): the boxes are read again then, not at
        // the selection's end, which came before the commit (boxes left at the old place, device ab6e7ac9)
        if (s->property("pendingEdit").value<QTransform>().isIdentity()) { requestBoxes(); return; }
        g_moved = true;
        watchSelectionRelease();
        g_pushWait = true;
        // already let go (the change came with the release): now; else at the release
        if (!selectionPressed()) QTimer::singleShot(0, qApp, [] { if (g_pushWait) pushWhenReleased(); });
    });
    hook(sc, "rootDocumentLengthChanged()", [] { if (!putting() && g_own == 0) groupsUnknown("typed text"); });
    hook(sc, "selectionItemCountChanged()", [] { requestBoxes(); });
    hook(sc, "selectionCleared()", [] {
        requestBoxes();
        if (g_moved) {
            g_moved = false;
            QString p;
            QObject *s = pageScene(&p);
            if (s && g_own == 0) {
                TLOG("a move or resize: one step");
                pushGroup(s, p, 1, false);
                QTimer::singleShot(900, qApp, [] { QString pg; if (QObject *s2 = pageScene(&pg)) followBands(s2, pg); });
            }
        }
        QTimer::singleShot(700, qApp, [] { QString p; if (QObject *s = pageScene(&p)) readTextBoxes(s); });
    });
    if (QObject *ph = dv->property("penHandler").value<QObject *>())
        hook(ph, "strokeCompleted()", [] {
            if (!selectionToolActive()) { if (!putting()) groupsUnknown("a pen stroke"); return; }
            // a lasso or pen tap on the Text objects layer that ends with nothing selected: back to writing (the
            // selection settles a moment after the stroke)
            QTimer::singleShot(400, qApp, [] {
                QString p;
                QObject *s = pageScene(&p);
                if (!s || putting() || adjusting() || g_edit.box || !selectionToolActive()) return;
                const int t = textLayer(s);
                if (t < 0 || s->property("currentLayer").toInt() != t || s->property("selectionItemCount").toInt() > 0) return;
                backToWriting(s, "a lasso that selected nothing");
            });
        });
    hook(dv, "currentPageIdChanged()", [] { groupsUnknown("another page"); });
    if (QQuickItem *tool = selectionToolButton())
        if (!tool->property("_rmppTextobjTool").toBool()) {
            tool->setProperty("_rmppTextobjTool", true);
            hook(tool, "selectedChanged()", [] { QTimer::singleShot(0, qApp, [] { toolChanged(); }); });
        }
    // a page that opens on the Text objects layer: the selection tool (decided), nothing else
    QPointer<QObject> scp(sc);
    QTimer::singleShot(2000, qApp, [scp] {
        if (!scp) return;
        const int t = textLayer(scp);
        if (t >= 0 && scp->property("currentLayer").toInt() == t && !selectionToolActive()) {
            TLOG("text layer: the page opened on it: the selection tool");
            useSelectionTool();
        }
    });
}

// ---- object mode: a dashed box on every object of the Text objects layer (developer, #3) ----
// Only while that layer is current. The boxes are read from the page (findObjects) when its content may have changed,
// kept in page units, and mapped to the view (DeviceSceneView.sceneToView) whenever the view moves; hidden while a
// gesture moves it. Plain Rectangles: a Canvas painted nothing on the device (probed).
const char *kBoxesQml = R"QML(
import QtQuick
Item {
    id: layer
    property var boxes: []
    anchors.fill: parent
    z: 40
    enabled: false
    Repeater {
        model: layer.boxes
        Item {
            x: modelData.x - 4; y: modelData.y - 4; width: modelData.width + 8; height: modelData.height + 8
            Repeater {   // top and bottom edges
                model: Math.max(1, Math.floor(parent.width / 16)) * 2
                Rectangle {
                    readonly property int n: Math.max(1, Math.floor(parent.width / 16))
                    x: (index % n) * 16; y: index < n ? 0 : parent.height - 2
                    width: 9; height: 2; color: "black"
                }
            }
            Repeater {   // left and right edges
                model: Math.max(1, Math.floor(parent.height / 16)) * 2
                Rectangle {
                    readonly property int n: Math.max(1, Math.floor(parent.height / 16))
                    y: (index % n) * 16; x: index < n ? 0 : parent.width - 2
                    width: 2; height: 9; color: "black"
                }
            }
        }
    }
}
)QML";
QPointer<QObject> g_boxesItem;
QList<QRectF> g_pageBoxes;   // page units

void mapBoxes() {
    QQuickItem *dv = documentView();
    QQuickItem *sv = dv ? sceneViewOf(dv) : nullptr;
    if (!g_boxesItem || !sv) return;
    QVariantList out;
    if (!sv->property("gesturing").toBool())
        for (const QRectF &b : g_pageBoxes) {
            QVariant a, c;
            QMetaObject::invokeMethod(sv, "sceneToView", Q_RETURN_ARG(QVariant, a), Q_ARG(QVariant, QVariant(b.topLeft())));
            QMetaObject::invokeMethod(sv, "sceneToView", Q_RETURN_ARG(QVariant, c), Q_ARG(QVariant, QVariant(b.bottomRight())));
            if (a.canConvert<QJSValue>()) a = a.value<QJSValue>().toVariant();
            if (c.canConvert<QJSValue>()) c = c.value<QJSValue>().toVariant();
            if (!a.canConvert<QPointF>() || !c.canConvert<QPointF>()) continue;
            out << QVariant(QRectF(a.toPointF(), c.toPointF()).normalized());
        }
    g_boxesItem->setProperty("boxes", out);
}

void readBoxesNow() {
    QString page;
    QObject *sc = pageScene(&page);
    if (sc && !sc->property("pendingEdit").value<QTransform>().isIdentity()) { mapBoxes(); return; }   // not now
    g_pageBoxes.clear();
    g_restValid = false;
    const int t = sc ? textLayer(sc) : -1;
    if (sc && t >= 0 && sc->property("currentLayer").toInt() == t && !g_edit.box && !putting()) {
        // a selected object shows xochitl's selection frame instead, which moves with it; its box comes back at the
        // new place when the selection ends (developer: the box should move with the object)
        QList<QRectF> sel, others;
        objectBoxes(sc, page, &sel, &others);
        g_restSelected = sel;
        g_restOthers = others;
        g_restPage = page;
        g_restValid = true;
        if (g_userOnText) g_pageBoxes = others;   // the snapshot is kept either way (pushing apart uses it)
        QRectF ou;
        for (const QRectF &o : others) ou |= o;
        TLOG("boxes: %lld %s in %.0f,%.0f %.0fx%.0f, %lld selected left out", (long long)others.size(),
             g_userOnText ? "shown" : "read (not shown: layer not chosen by the user)", ou.x(), ou.y(), ou.width(),
             ou.height(), (long long)sel.size());
    }
    mapBoxes();
}

// Coalesced: many changes in one turn of the event loop read the page once.
void requestBoxes() {
    static bool queued = false;
    if (queued) return;
    queued = true;
    QTimer::singleShot(0, qApp, [] { queued = false; readBoxesNow(); });
}

// The view's mapping, as two mapped points: the boxes are mapped again when it changes.
QString viewKey(QQuickItem *sv) {
    QVariant a, c;
    QMetaObject::invokeMethod(sv, "sceneToView", Q_RETURN_ARG(QVariant, a), Q_ARG(QVariant, QVariant(QPointF(0, 0))));
    QMetaObject::invokeMethod(sv, "sceneToView", Q_RETURN_ARG(QVariant, c), Q_ARG(QVariant, QVariant(QPointF(1000, 1000))));
    if (a.canConvert<QJSValue>()) a = a.value<QJSValue>().toVariant();
    if (c.canConvert<QJSValue>()) c = c.value<QJSValue>().toVariant();
    const QPointF p = a.toPointF(), q = c.toPointF();
    return QStringLiteral("%1,%2,%3,%4,%5").arg(p.x()).arg(p.y()).arg(q.x()).arg(q.y()).arg(sv->property("gesturing").toBool());
}

void installBoxOverlay() {
    QQuickItem *dv = documentView();
    QQuickItem *sv = dv ? sceneViewOf(dv) : nullptr;
    if (!sv) return;
    if (!g_boxesItem || g_boxesItem->parent() != sv) {
        if (g_boxesItem) g_boxesItem->deleteLater();
        g_boxesItem = createWithState(sv, kBoxesQml, "TextBoxes");
        requestBoxes();
    }
    if (sv->property("_rmppTextobjBoxes").toBool()) return;
    sv->setProperty("_rmppTextobjBoxes", true);
    if (QQuickWindow *win = sv->window()) {   // the view signals alone left boxes behind on the device
        QPointer<QQuickItem> svp(sv);
        QObject::connect(win, &QQuickWindow::afterAnimating, sv, [svp] {
            static QString last;
            if (!svp || g_pageBoxes.isEmpty() || selectionPressed()) return;   // not while a selection is dragged
            const QString k = viewKey(svp);
            if (k != last) { last = k; mapBoxes(); }
        });
    }
    for (const char *sig : {"viewportChanged()", "viewScaleChanged()", "scrollPositionChanged()", "gesturingChanged()",
                            "zoomModeChanged()"}) {
        const int i = sv->metaObject()->indexOfSignal(sig);
        if (i < 0) continue;
        auto *h = new SignalHook;
        h->setParent(sv);
        h->fn = [] { mapBoxes(); };
        QMetaObject::connect(sv, i, h, QObject::staticMetaObject.methodCount(), Qt::DirectConnection, nullptr);
    }
}

// ---- the layers menu ----
// The Text objects row: up, down and merge hidden (picking it and delete stay; renaming is not blocked). The row below
// it cannot move up past it, the row above it cannot move down or merge into it, and the last other layer cannot be
// deleted (ink would have nowhere to go but the Text objects layer). Only button items are touched (changing a row's
// layer*Enabled switches made xochitl rebuild its buttons); what we hid or disabled on a row that is no longer in one
// of those places is restored. Applied before every frame while the menu was opened in the last 30 s.
void forEachAction(QQuickItem *row, const std::function<void(QQuickItem *, const QString &)> &fn) {
    QList<QQuickItem *> stack{row};
    while (!stack.isEmpty()) {
        QQuickItem *x = stack.takeLast();
        if (x != row && x->metaObject()->indexOfProperty("actionKey") >= 0) { fn(x, x->property("actionKey").toString()); continue; }
        stack << x->childItems();
    }
}

void setOurs(QQuickItem *b, bool visible, bool enabled) {
    if (b->isVisible() != visible) { b->setVisible(visible); b->setProperty("_rmppTextobjLock", true); }
    if (b->isEnabled() != enabled) { b->setEnabled(enabled); b->setProperty("_rmppTextobjLock", true); }
}

void lockTextLayerRows() {
    QList<QQuickItem *> rows;
    findAnywhere([&rows](QQuickItem *i) {
        if (classStarts(i, "LayerComponent") && i->isVisible()) rows << i;
        return false;
    });
    if (rows.isEmpty()) return;
    int t = -1, others = 0;
    for (QQuickItem *r : rows) {
        const QVariant name = r->property("layerName");
        if (!name.isValid()) continue;
        if (name.toString() == QLatin1String(kTextLayerName)) t = r->property("layerIndex").toInt();
        else ++others;
    }
    for (QQuickItem *r : rows) {
        if (!r->property("layerName").isValid()) continue;
        const int i = r->property("layerIndex").toInt();
        const bool isT = t >= 0 && i == t, below = t >= 0 && i == t - 1, above = t >= 0 && i == t + 1;
        forEachAction(r, [&](QQuickItem *b, const QString &k) {
            const bool move = k == QLatin1String("up") || k == QLatin1String("down") || k == QLatin1String("merge");
            if (isT) { if (move) setOurs(b, false, b->isEnabled()); return; }
            bool enabled = true;
            if (below && k == QLatin1String("up")) enabled = false;
            if (above && (k == QLatin1String("down") || k == QLatin1String("merge"))) enabled = false;
            const bool del = k == QLatin1String("trash") || k == QLatin1String("delete") || k == QLatin1String("remove");
            if (t >= 0 && others <= 1 && del) enabled = false;
            if (!enabled) setOurs(b, true, false);
            else if (b->property("_rmppTextobjLock").toBool()) {   // ours before: as it was
                b->setVisible(true);
                b->setEnabled(true);
                b->setProperty("_rmppTextobjLock", false);
            }
        });
    }
}

QElapsedTimer g_layersMenuPressed;

void watchLayersMenu() {
    QQuickItem *menu = findAnywhere([](QQuickItem *i) { return i->objectName() == QLatin1String("layersMenu"); });
    QQuickItem *btn = menu ? findItem(menu, [](QQuickItem *i) { return i->metaObject()->indexOfSignal("pressed()") >= 0; }) : nullptr;
    if (!btn || btn->property("_rmppTextobjLayersMenu").toBool()) return;
    QQuickWindow *win = btn->window();
    if (!win) return;
    btn->setProperty("_rmppTextobjLayersMenu", true);
    auto *hook = new SignalHook;
    hook->setParent(btn);
    hook->fn = [] { g_layersMenuPressed.start(); };
    QMetaObject::connect(btn, btn->metaObject()->indexOfSignal("pressed()"), hook, QObject::staticMetaObject.methodCount(),
                         Qt::DirectConnection, nullptr);
    if (!win->property("_rmppTextobjFrames").toBool()) {
        win->setProperty("_rmppTextobjFrames", true);
        QObject::connect(win, &QQuickWindow::afterAnimating, btn, [] {
            if (g_layersMenuPressed.isValid() && g_layersMenuPressed.elapsed() < 30000) lockTextLayerRows();
        }, Qt::DirectConnection);
    }
}

// ---- highlights on text objects: bands on the writing layer, bound to their object ----
// The highlighter draws on the writing layer, the objects are on the Text objects layer. With xochitl's "Snap to
// text" on, a highlighter stroke mostly over an object (its hinted box) is replaced by straight bands on the writing
// layer, over the characters it covers, in its colour (developer, #3: the eraser then takes a highlight off as on
// typed text, the glyphs being on another layer). Each band set is bound to its object for the session: when the
// object has moved or was resized, its bands are drawn again over the same characters (followBands); bands erased
// are dropped. Steps: the freehand stroke taken back by an undo (it is the last step; checked, else redone), the
// Text objects layer current for a moment to read the object, the bands drawn on the writing layer. One undo press
// takes the bands back.
bool overTextObjects(const QString &page, const QRect &r) {
    loadTextBoxes();
    const double a = double(r.width()) * r.height();
    if (a <= 0) return false;
    for (const QRectF &b : g_textBoxes.value(page)) {
        const QRectF on = QRectF(r).intersected(b.adjusted(-8, -8, 8, 8));
        if (on.width() * on.height() >= 0.6 * a) return true;
    }
    return false;
}

struct BandSet {
    QString recId, page;
    Hl hl;                 // characters and colour
    QRectF box;            // the object's box when the bands were drawn
    QSet<QString> keys;    // rect keys of the band strokes on the writing layer
};
QList<BandSet> g_bandSets;

// A stroke that is one of the bands drawn here (not a new highlight: it was taken for one, and drawn again).
bool isOwnBand(const QString &key) {
    for (const BandSet &b : g_bandSets) if (b.keys.contains(key)) return true;
    return false;
}

// Rect keys of the strokes on the current layer that were not in before.
QSet<QString> newRectKeys(QObject *sc, const QSet<QString> &before) {
    QSet<QString> out;
    for (const QRect &r : pageLineRects(sc)) if (!before.contains(rectKey(r))) out.insert(rectKey(r));
    return out;
}

void highlightToTextLayer(QQuickItem *dv, QObject *sc, const QString &pageId, const QList<QRect> &strokes, int color,
                          uint argb) {
    const int t = textLayer(sc), w = sc->property("currentLayer").toInt();
    QQuickItem *prev = activeToolButton();
    if (t < 0 || w == t || strokes.isEmpty() || putting() || !prev) return;
    TLOG("highlight over a text object (layer %d, colour %d): bands on this layer", w, color);
    g_putting = true;
    g_putStarted.start();
    QPointer<QObject> scp(sc);
    QPointer<QQuickItem> dvp(dv), prevp(prev);
    QSet<QString> keys;
    for (const QRect &r : strokes) keys.insert(rectKey(r));
    QSet<QString> before;
    for (const QRect &r : pageLineRects(sc)) before.insert(rectKey(r));
    auto done = [scp, dvp, prevp, w](const char *why) {
        if (why) TLOG("highlight: %s", why);
        if (scp) setLayer(scp, w);
        QTimer::singleShot(400, qApp, [prevp, dvp] {
            if (prevp) tapItem(prevp);
            // bands added on the writing layer were not painted until the page was opened again (device): a full
            // refresh shows them
            if (dvp) { QMetaObject::invokeMethod(dvp, "updateAfterStroke"); fullRefreshLater(dvp, 0); }
            g_putting = false;
        });
    };
    if (strokes.size() != 1 || !sc->property("undoAvailable").toBool()) { done("left as drawn (not one stroke)"); return; }
    QMetaObject::invokeMethod(sc, "undo", Qt::DirectConnection);
    QTimer::singleShot(400, sc, [=] {
        if (!scp) { g_putting = false; return; }
        QSet<QString> now;
        for (const QRect &r : pageLineRects(scp)) now.insert(rectKey(r));
        if (now != before - keys) {
            QMetaObject::invokeMethod(scp, "redo", Qt::DirectConnection);
            done("left as drawn (the undo took something else back: redone)");
            return;
        }
        setLayer(scp, t);
        QTimer::singleShot(500, scp, [=] {
            if (!scp) { g_putting = false; return; }
            const PageObjects po = findObjects(scp, pageId, true);
            struct Pend { QString recId; Hl hl; QRectF box; QPointF d; double k; };
            QList<Pend> pend;
            for (const QRect &s : strokes)
                for (const Placement &p : po.found) {
                    const Rec &r = po.recs[p.rec];
                    Hl h;
                    if (!highlightOn(r, p, s) || !rangeFromRect(r.text, pageLines(r, p), QRectF(s), &h.a, &h.b)) continue;
                    h.color = color;
                    h.argb = argb;
                    h.rects = bandRects(r.text, pageLines(r, p), h.a, h.b);
                    if (h.rects.isEmpty()) continue;
                    pend << Pend{r.id, h, p.box, p.d, p.k};
                    break;
                }
            setLayer(scp, w);
            if (pend.isEmpty()) {   // back to the freehand stroke
                QTimer::singleShot(400, scp, [=] {
                    if (scp) QMetaObject::invokeMethod(scp, "redo", Qt::DirectConnection);
                    done("no text object under it: left as drawn");
                });
                return;
            }
            QTimer::singleShot(400, scp, [=] {
                if (!scp) { g_putting = false; return; }
                QSet<QString> was;
                for (const QRect &r : pageLineRects(scp)) was.insert(rectKey(r));
                QList<Hl> draw;
                for (const Pend &pd : pend) draw << pd.hl;
                const int drawn = addBands(scp, draw);
                if (drawn <= 0) {
                    QMetaObject::invokeMethod(scp, "redo", Qt::DirectConnection);
                    done(drawn < 0 ? "this xochitl version is not supported" : "no band drawn: left as drawn");
                    return;
                }
                pushGroup(scp, pageId, drawn, false);   // one undo press takes the bands back
                QTimer::singleShot(600, scp, [=] {
                    if (!scp) { g_putting = false; return; }
                    const QList<QRect> nowRects = pageLineRects(scp);
                    QHash<QString, QList<Hl>> recHl;
                    const QList<Rec> recs = readRecs();
                    for (const Pend &pd : pend) {
                        BandSet b{pd.recId, pageId, pd.hl, pd.box, {}};
                        for (const QRectF &e : pd.hl.rects)
                            for (const QRect &q : nowRects)
                                if (!was.contains(rectKey(q)) && sameBand(q, e, 1.0)) { b.keys.insert(rectKey(q)); break; }
                        g_bandSets << b;
                        if (!recHl.contains(pd.recId))
                            for (const Rec &r : recs) if (r.id == pd.recId) recHl[pd.recId] = hlOf(r);
                        Hl stored = pd.hl;   // the record keeps the characters and colour (rects in record units)
                        stored.rects.clear();
                        for (const QRectF &e : pd.hl.rects) stored.rects << QRectF((e.topLeft() - pd.d) / pd.k, e.size() / pd.k);
                        recHl[pd.recId] << stored;
                    }
                    for (auto it = recHl.cbegin(); it != recHl.cend(); ++it) setRecordHl(it.key(), hlJson(it.value()));
                    TLOG("highlight: %d band(s) on the writing layer, bound to %lld object(s)", drawn, (long long)pend.size());
                    done(nullptr);
                });
            });
        });
    });
}

// After objects on the Text objects layer moved or were resized (their layer current, nothing selected): the bands
// of each are drawn again over the same characters at the new place, on the writing layer. Joins the move's undo
// group. Bands whose strokes are all gone (erased) are dropped.
void followBands(QObject *sc, const QString &pageId) {
    if (g_bandSets.isEmpty() || putting() || g_edit.box) return;
    bool any = false;
    for (const BandSet &b : g_bandSets) if (b.page == pageId) { any = true; break; }
    if (!any) return;
    const int t = textLayer(sc), w = writingLayer(sc), orig = sc->property("currentLayer").toInt();
    if (t < 0 || w < 0 || sc->property("selectionItemCount").toInt() > 0) return;
    g_putting = true;
    g_putStarted.start();
    QPointer<QObject> scp(sc);
    setLayer(sc, t);   // the objects are read on their layer (the move may have ended back on the writing layer)
    QTimer::singleShot(orig == t ? 0 : 500, sc, [scp, pageId, t, w, orig] {
        if (!scp) { g_putting = false; return; }
        const PageObjects po = findObjects(scp, pageId, true);
        struct Job { int set; QList<QRectF> bands; QRectF box; };
        QList<Job> jobs;
        for (int i = 0; i < g_bandSets.size(); ++i) {
            const BandSet &b = g_bandSets[i];
            if (b.page != pageId) continue;
            const Placement *best = nullptr;
            double bd = 1e18;
            for (const Placement &p : po.found) {
                if (po.recs[p.rec].id != b.recId) continue;
                const QPointF d = p.box.center() - b.box.center();
                const double dd = d.x() * d.x() + d.y() * d.y();
                if (dd < bd) { bd = dd; best = &p; }
            }
            if (!best || sameBox(best->box, b.box)) continue;
            const Rec &r = po.recs[best->rec];
            jobs << Job{i, bandRects(r.text, pageLines(r, *best), b.hl.a, b.hl.b), best->box};
        }
        auto finish = [scp, orig, pageId](int steps) {
            if (scp) {
                setLayer(scp, orig);
                if (steps > 0) pushGroup(scp, pageId, steps + 1, true);   // with the move: one press
            }
            QTimer::singleShot(400, qApp, [] {
                g_putting = false;
                requestBoxes();
                if (QQuickItem *dv = documentView()) fullRefreshLater(dv, 0);   // bands on the writing layer shown
            });
        };
        if (jobs.isEmpty()) { finish(0); return; }
        TLOG("bands: %lld object(s) moved: their bands follow", (long long)jobs.size());
        setLayer(scp, w);
        auto steps = std::make_shared<int>(0);
        auto run = std::make_shared<std::function<void(int)>>();
        *run = [scp, jobs, steps, run, w, finish](int j) {
            if (!scp) { g_putting = false; return; }
            if (j >= jobs.size()) { finish(*steps); return; }
            const Job job = jobs[j];
            if (job.set >= g_bandSets.size()) { (*run)(j + 1); return; }
            const QList<QRect> rects = pageLineRects(scp);
            QList<int> want;
            for (int i = 0; i < rects.size(); ++i) if (g_bandSets[job.set].keys.contains(rectKey(rects[i]))) want << i;
            auto draw = [scp, job, steps, run, j](bool deleted) {
                if (!scp) { g_putting = false; return; }
                if (deleted) ++*steps;
                QSet<QString> was;
                for (const QRect &r : pageLineRects(scp)) was.insert(rectKey(r));
                Hl h = g_bandSets[job.set].hl;
                h.rects = job.bands;
                const int n = addBands(scp, {h});
                if (n > 0) *steps += n;
                QTimer::singleShot(500, scp, [scp, job, was, run, j] {
                    if (!scp) { g_putting = false; return; }
                    if (job.set < g_bandSets.size()) {
                        BandSet &b = g_bandSets[job.set];
                        b.keys = newRectKeys(scp, was);
                        b.box = job.box;
                    }
                    (*run)(j + 1);
                });
            };
            if (want.isEmpty()) { TLOG("bands: none left to move (erased): drawn again"); draw(false); return; }
            useSelectionTool();
            QTimer::singleShot(300, scp, [scp, rects, want, draw, w] {
                if (!scp) { g_putting = false; return; }
                setSelection(scp, rects, want, [scp, want, draw, w] {
                    if (!scp) { g_putting = false; return; }
                    if (scp->property("selectionItemCount").toInt() != want.size()) {
                        TLOG("bands: the old bands could not be selected alone: left, new ones drawn");
                        endSelection(scp, nullptr);
                        draw(false);
                        return;
                    }
                    QMetaObject::invokeMethod(scp, "deleteSelectedItems", Q_ARG(int, w));
                    QTimer::singleShot(300, scp, [draw] { draw(true); });
                });
            });
        };
        QTimer::singleShot(500, scp, [run] { (*run)(0); });
    });
}

// Bands whose strokes are all gone from the writing layer (erased there): unbound, and the highlight dropped from the
// record. Called on the writing layer when its strokes changed.
void dropErasedBands(QObject *sc, const QString &pageId) {
    if (g_bandSets.isEmpty()) return;
    const int w = writingLayer(sc);
    if (w < 0 || sc->property("currentLayer").toInt() != w) return;
    QSet<QString> now;
    for (const QRect &r : pageLineRects(sc)) now.insert(rectKey(r));
    for (int i = int(g_bandSets.size()) - 1; i >= 0; --i) {
        BandSet &b = g_bandSets[i];
        if (b.page != pageId || b.keys.isEmpty()) continue;
        bool any = false;
        for (const QString &k : b.keys) if (now.contains(k)) { any = true; break; }
        if (any) continue;
        const QString recId = b.recId;
        const Hl gone = b.hl;
        g_bandSets.removeAt(i);
        for (const Rec &r : readRecs()) {
            if (r.id != recId) continue;
            QList<Hl> keep;
            for (const Hl &h : hlOf(r)) if (!(h.a == gone.a && h.b == gone.b && h.color == gone.color)) keep << h;
            setRecordHl(recId, hlJson(keep));
        }
        TLOG("bands: a highlight erased");
    }
}
