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

// How many of the Text objects layer's known objects are at `at` (0: none; 2 or more: they overlap there).
int textObjectsAt(const QString &page, const QPointF &at, QRectF *box = nullptr) {
    loadTextBoxes();
    QList<QRectF> hits;   // distinct boxes there (duplicates of one object counted once: a false overlap, 10cf412e)
    for (const QRectF &b : g_textBoxes.value(page)) {
        if (!b.adjusted(-15, -15, 15, 15).contains(at)) continue;
        bool dup = false;
        for (const QRectF &h : hits) if (sameBox(h, b)) dup = true;
        if (!dup) { hits << b; if (box) *box = b; }
    }
    return int(hits.size());
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

// The current layer changed: the Text objects layer current (by any cause) turns on the selection tool (not a step);
// another layer current is remembered as the writing layer.
void layerChanged() {
    QString page;
    QObject *sc = pageScene(&page);
    if (!sc) return;
    if (g_ownLayerSwitch > 0) --g_ownLayerSwitch;
    const int t = textLayer(sc), cur = sc->property("currentLayer").toInt();
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
    if (!sc || g_edit.box || putting() || selectionToolActive()) return;
    const int t = textLayer(sc);
    if (t < 0 || sc->property("currentLayer").toInt() != t) return;
    const int w = writingLayer(sc);
    if (w < 0) return;
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
    hook(sc, "pendingEditChanged()", [] {   // also emitted, unchanged, during undo: only a real transform counts
        QString p;
        QObject *s = pageScene(&p);
        if (!s || putting() || adjusting() || g_own > 0) return;
        if (!s->property("pendingEdit").value<QTransform>().isIdentity()) groupsUnknown("a move or resize");
    });
    hook(sc, "rootDocumentLengthChanged()", [] { if (!putting() && g_own == 0) groupsUnknown("typed text"); });
    hook(sc, "selectionCleared()", [] {
        g_selectionEnded.start();
        QTimer::singleShot(700, qApp, [] { QString p; if (QObject *s = pageScene(&p)) readTextBoxes(s); });
    });
    if (QObject *ph = dv->property("penHandler").value<QObject *>())
        hook(ph, "strokeCompleted()", [] { if (!selectionToolActive() && !putting()) groupsUnknown("a pen stroke"); });
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
