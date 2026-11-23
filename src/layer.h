// ---- the Text objects layer (#3) ----
// Text objects live on a layer of their own ("Text objects"), so writing and erasing on the user's layer never touch
// them. The user does not see the layer change: a new object is added to that layer while the user's layer stays
// current, then the Text objects layer is made current only while the object is selected (a selection on another
// layer is dropped by xochitl), and the user's layer comes back when the selection ends.
//
// Every layer change is an undo step in xochitl (addLayer, setLayerName and setCurrentLayer alike, OS 3.29.1), so:
// - the switch to the Text objects layer (S1) is made after the object is added (B): when the selection ends with
//   the object untouched, undoing S1 takes back only the switch, and the history is as on one layer;
// - when the object was moved or resized, the switch back is a step of its own (S2);
// - after an undo or redo, while the Text objects layer is current (and the user did not pick it in the layers
//   menu), one more step the same way (settleLayer): undo = S2, the moves and S1 together; redo likewise.
// The conversion's own chain (handwriting back in one undo) is in edit.h (layerConvStep).

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

QElapsedTimer g_ownLayerChange;   // our own layer changes (not the user picking a layer)

void setLayer(QObject *sc, int i) {
    g_ownLayerChange.start();
    QMetaObject::invokeMethod(sc, "setCurrentLayer", Q_ARG(int, i));
}

// The Text objects layer of the page, made if there is none: addLayer (which makes it current), its name, and the
// user's layer current again; three undo steps, undone with the conversion (LastConv::steps). done(layer, steps).
void ensureTextLayer(QPointer<QObject> scp, int home, std::function<void(int, int)> done) {
    const int t = textLayer(scp);
    if (t >= 0) { done(t, 0); return; }
    if (!layerApi(scp)) { done(-1, 0); return; }
    g_ownLayerChange.start();
    QMetaObject::invokeMethod(scp, "addLayer");
    QTimer::singleShot(400, scp, [scp, home, done] {
        if (!scp) return;
        const int t = scp->property("layerCount").toInt() - 1;
        if (t <= home || scp->property("currentLayer").toInt() != t) {
            TLOG("text layer: addLayer did not add a current layer (%d layers, current %d)", t + 1,
                 scp->property("currentLayer").toInt());
            done(-1, 1);
            return;
        }
        QMetaObject::invokeMethod(scp, "setLayerName", Q_ARG(int, t), Q_ARG(QString, QString::fromLatin1(kTextLayerName)));
        QTimer::singleShot(300, scp, [scp, home, t, done] {
            if (!scp) return;
            setLayer(scp, home);
            QTimer::singleShot(400, scp, [scp, home, t, done] {
                if (!scp) return;
                const bool ok = textLayer(scp) == t && scp->property("currentLayer").toInt() == home;
                TLOG("text layer: %s (layer %d)", ok ? "added" : "NOT added as expected", t + 1);
                done(ok ? t : -1, 3);
            });
        });
    });
}

// The time the Text objects layer is current for a selection there.
struct LayerSession {
    QPointer<QObject> sc;
    QString page;
    int home = -1, text = -1;
    QSet<QString> start;   // the layer's strokes once the selection is made (untouched: the same at the end)
    bool active = false, selecting = false;
    int pending = 0;
    int objects = -1;      // text objects found on the layer at the start (strays are moved only if none is lost)       // an undo (1) or redo (2) asked for while the object was selected
    QElapsedTimer since;
};
LayerSession g_ls;

QSet<QString> layerKeys(QObject *sc) {
    QSet<QString> keys;
    for (const QRect &r : pageLineRects(sc)) keys.insert(rectKey(r));
    return keys;
}

// S1: the Text objects layer current (after the object was added there), then `select` (on that layer).
void startTextSession(QPointer<QObject> scp, const QString &page, int home, int text, std::function<void()> select) {
    g_ls = LayerSession{};
    g_ls.sc = scp;
    g_ls.page = page;
    g_ls.home = home;
    g_ls.text = text;
    g_ls.active = g_ls.selecting = true;
    g_ls.since.start();
    setLayer(scp, text);
    QTimer::singleShot(250, scp, [scp, select] {
        if (!scp || !g_ls.active) return;
        if (select) select();
    });
}

int objectCount(QObject *sc);

// Once the selection on the Text objects layer is made: what is there now (to tell a move at the end).
void textSessionSelected() {
    if (!g_ls.active || !g_ls.sc) return;
    g_ls.start = layerKeys(g_ls.sc);
    g_ls.objects = objectCount(g_ls.sc);
    g_ls.selecting = false;
    TLOG("text layer: object selected (%lld strokes on the layer)", (long long)g_ls.start.size());
}

void afterPendingStep(bool undo);   // edit.h

// The selection ended: the user's layer back (S2). A move is applied a moment after the selection ends, so the
// layer is compared 600 ms later.
void moveStraysThenSwitch(QPointer<QObject> scp, int text, int home);
int strayCount(QObject *sc);

// Every selection on the Text objects layer leaves two steps in the history: the switch there (S1) and the switch
// back (S2), with whatever was done in between. Untouched, the pair changes nothing the user sees: an undo or redo
// that meets it goes on through it to the user's own step (settleLayer -> leftTextLayer). Undoing S1 instead (the
// earlier way) left it in the redo history: the redo button came on after a mere tap outside (developer, cb57b732).
// The pairs in history order; g_pairIdx of them are done (not undone).
QList<bool> g_pairs;   // true: untouched (the user sees nothing)
int g_pairIdx = 0;

void pushPair(bool untouched) {
    while (g_pairs.size() > g_pairIdx) g_pairs.removeLast();   // a new step cut the redo history
    g_pairs << untouched;
    ++g_pairIdx;
    if (g_pairs.size() > 200) { g_pairs.removeFirst(); --g_pairIdx; }
}

void endTextSession() {
    QObject *sc = g_ls.sc;
    g_ls.active = false;
    if (!sc) return;
    const int home = g_ls.home, text = g_ls.text, pending = g_ls.pending, objects = g_ls.objects;
    const QSet<QString> start = g_ls.start;
    QPointer<QObject> scp(sc);
    QTimer::singleShot(600, qApp, [scp, home, text, pending, start, objects] {
        QObject *sc = scp;
        if (!sc) return;
        if (sc->property("currentLayer").toInt() != text) { TLOG("text layer: ended on another layer"); return; }
        // a stroke written on it (a pen right after a tool change): to the user's layer; only if every object is
        // still found (an object not recognized after an edit would look like strays)
        if (strayCount(sc) > 0 && objects >= 0 && objectCount(sc) >= objects) {
            pushPair(false);
            moveStraysThenSwitch(scp, text, home);
            if (pending) QTimer::singleShot(2500, qApp, [pending] { afterPendingStep(pending == 1); });
            return;
        }
        const bool untouched = layerKeys(sc) == start && !start.isEmpty();
        pushPair(untouched);
        setLayer(sc, home);
        TLOG("text layer: back to layer %d (%s)", home + 1, untouched ? "untouched: an undo or redo passes through" : "object changed");
        if (pending) QTimer::singleShot(500, qApp, [pending] { afterPendingStep(pending == 1); });
    });
}

bool textSessionActive() { return g_ls.active; }

// An undo or redo while the object is selected there: xochitl only ends the selection; it is applied once the
// user's layer is back (endTextSession).
bool textSessionPending(bool undo) {
    if (!g_ls.active) return false;
    g_ls.pending = undo ? 1 : 2;
    TLOG("%s while selected on the text layer: after the selection ends", undo ? "undo" : "redo");
    return true;
}

void checkTextSession() {
    if (!g_ls.active) return;
    QObject *sc = g_ls.sc;
    if (!sc) { g_ls.active = false; return; }
    if (g_ls.selecting) {
        if (g_ls.since.elapsed() > 4000) { TLOG("text layer: nothing selected"); g_ls.start = layerKeys(sc); g_ls.selecting = false; }
        else return;
    }
    if (sc->property("selectionItemCount").toInt() > 0 || g_edit.box || putting() || adjusting()) return;
    endTextSession();
}

// After an undo or redo: while the Text objects layer is current outside a selection there (and not picked by the
// user), one more step the same way, until the user's layer is current.
bool g_userTextLayer = false;   // the user picked the Text objects layer (layers menu)

void settleLayer(bool undo, int n) {
    QString page;
    QObject *sc = pageScene(&page);
    if (!sc || g_ls.active || g_userTextLayer) return;
    const int t = textLayer(sc);
    if (t < 0 || sc->property("currentLayer").toInt() != t) {
        if (n > 0) {
            TLOG("text layer: %d more %s step(s) to leave it", n, undo ? "undo" : "redo");
            bool untouched = false;   // the pair just passed
            if (undo && g_pairIdx > 0) untouched = g_pairs.value(--g_pairIdx);
            else if (!undo && g_pairIdx < g_pairs.size()) untouched = g_pairs.value(g_pairIdx++);
            if (untouched) {
                TLOG("text layer: an untouched selection passed: the %s goes on", undo ? "undo" : "redo");
                QTimer::singleShot(300, qApp, [undo] { userStep(undo, true); });
            }
        }
        return;
    }
    if (n >= 20) { TLOG("text layer: still current after 20 steps"); return; }
    if (!undo && !sc->property("redoAvailable").toBool()) {
        // a redo of a switch we had undone (it stays in the redo history) with nothing after it: switch back as a
        // new step. Undoing it again left it on top of the redo history, so every redo redid it (stuck, device
        // 0d009cd3); a new step clears that history (empty but for it), and an undo later skips both (settleLayer).
        int home = -1;
        for (int i = sc->property("layerCount").toInt() - 1; i >= 0; --i) if (i != t) { home = i; break; }
        if (home >= 0) setLayer(sc, home);
        TLOG("text layer: redo ended on it: back to layer %d (a step of its own)", home + 1);
        return;
    }
    ownUndoRedo(sc, undo);
    QTimer::singleShot(400, qApp, [undo, n] { settleLayer(undo, n + 1); });
}

// The user changed the current layer (layers menu): picking the Text objects layer is the user's own choice.

void layerChanged() {
    QString page;
    QObject *sc = pageScene(&page);
    if (!sc) return;
    if ((g_ownLayerChange.isValid() && g_ownLayerChange.elapsed() < 1500) || (g_ownUndo.isValid() && g_ownUndo.elapsed() < 2500)
        || (g_userStepAt.isValid() && g_userStepAt.elapsed() < 3000) || g_ls.active)
        return;
    const bool on = sc->property("currentLayer").toInt() == textLayer(sc);
    if (on != g_userTextLayer) TLOG("text layer: %s by the user", on ? "picked" : "left");
    g_userTextLayer = on;
}

// Strokes on the Text objects layer that are not part of a text object (nor a mark on one): written there while
// that layer was current by mistake (a page opened on it, developer's "테" on 0e3040b5). Moved to the user's layer
// (copied there in one step, deleted here in one), then that layer is made current.
QList<int> strayIndexes(QObject *sc, PageObjects *out) {
    QQuickItem *dv = documentView();
    const QString pageId = dv ? dv->property("currentPageId").toString() : QString();
    *out = findObjects(sc, pageId, false);
    QSet<int> keep;
    for (const Placement &p : out->found) {
        for (int m : p.members) if (m >= 0) keep.insert(m);
        for (int i = 0; i < out->rects.size(); ++i) if (isMark(*out, p, i)) keep.insert(i);
    }
    QList<int> strays;
    for (int i = 0; i < out->rects.size(); ++i) if (!keep.contains(i)) strays << i;
    return strays;
}

int objectCount(QObject *sc) {
    QQuickItem *dv = documentView();
    return int(findObjects(sc, dv ? dv->property("currentPageId").toString() : QString(), false).found.size());
}

int strayCount(QObject *sc) {
    PageObjects po;
    return int(strayIndexes(sc, &po).size());
}

void moveStraysThenSwitch(QPointer<QObject> scp, int text, int home) {
    QObject *sc = scp;
    if (!sc) return;
    PageObjects po;
    const QList<int> strays = strayIndexes(sc, &po);
    if (strays.isEmpty()) { setLayer(sc, home); return; }
    TLOG("text layer: %lld stroke(s) on it are not text objects: moved to layer %d", (long long)strays.size(), home + 1);
    const bool switched = useSelectionTool();
    QTimer::singleShot(switched ? 500 : 0, sc, [scp, po, strays, text, home] {
        if (!scp) return;
        setSelection(scp, po.rects, strays, [scp, text, home] {
            QObject *sc = scp;
            if (!sc) return;
            const QMetaObject *mo = sc->metaObject();
            const int clone = mo->indexOfMethod("cloneSelectedItems(int,double)");
            const int bound = mo->indexOfMethod("getItemBoundingRect(QList<std::shared_ptr<SceneItem>>)");
            const int add = mo->indexOfMethod("cloneAddAndSelectItems(int,QList<std::shared_ptr<SceneItem>>,double,QPointF)");
            if (clone < 0 || bound < 0 || add < 0 || sc->property("selectionItemCount").toInt() <= 0) { setLayer(sc, home); return; }
            int lay = text;
            double scale = 1.0;
            QList<std::shared_ptr<void>> items;
            void *a1[] = {&items, &lay, &scale};
            sc->qt_metacall(QMetaObject::InvokeMetaMethod, clone, a1);
            QRectF box;
            void *a2[] = {&box, &items};
            sc->qt_metacall(QMetaObject::InvokeMetaMethod, bound, a2);
            QMetaObject::invokeMethod(sc, "deleteSelectedItems", Q_ARG(int, text));
            QMetaObject::invokeMethod(sc, "clearSelectedItems");
            int to = home;
            QPointF centre = box.center();
            void *a3[] = {nullptr, &to, &items, &scale, &centre};
            sc->qt_metacall(QMetaObject::InvokeMetaMethod, add, a3);
            QTimer::singleShot(300, sc, [scp, home] { if (scp) setLayer(scp, home); });
        });
    });
}

void watchLayers() {
    QQuickItem *dv = documentView();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    if (!sc || sc->property("_rmppTextobjLayers").toBool()) return;
    const int sig = sc->metaObject()->indexOfSignal("currentLayerChanged()");
    const int sel = sc->metaObject()->indexOfSignal("selectionItemCountChanged()");
    if (sig < 0 || sel < 0) return;
    sc->setProperty("_rmppTextobjLayers", true);
    g_userTextLayer = false;
    g_pairs.clear();   // a new document: a new history
    g_pairIdx = 0;
    // a page opened with the Text objects layer current (seen on the device, cause not found): the user's layer
    // again, once the page has settled (no scene calls while a page loads: xochitl crashed then)
    QPointer<QObject> scp(sc);
    QTimer::singleShot(3000, qApp, [scp] {
        if (!scp || g_ls.active || g_userTextLayer) return;
        const int t = textLayer(scp), cur = scp->property("currentLayer").toInt(), n = scp->property("layerCount").toInt();
        if (t < 0 || cur != t) return;
        int home = -1;
        for (int i = n - 1; i >= 0; --i) if (i != t) { home = i; break; }
        if (home < 0) return;
        TLOG("text layer: the page opened on it: back to layer %d", home + 1);
        moveStraysThenSwitch(scp, t, home);
    });
    auto *hook = new SignalHook;
    hook->setParent(sc);
    hook->fn = [] { QTimer::singleShot(0, qApp, [] { layerChanged(); }); };
    QMetaObject::connect(sc, sig, hook, QObject::staticMetaObject.methodCount(), Qt::DirectConnection, nullptr);
    auto *hook2 = new SignalHook;
    hook2->setParent(sc);
    hook2->fn = [] { if (g_ls.active) QTimer::singleShot(150, qApp, [] { checkTextSession(); }); };
    QMetaObject::connect(sc, sel, hook2, QObject::staticMetaObject.methodCount(), Qt::DirectConnection, nullptr);
}

// The Text objects layer in the layers menu is fixed, as xochitl's own Text layer is (developer, #3): no move up or
// down, no merge, no delete, no rename (its buttons hidden, its name field read-only); the layer below it cannot
// move up past it, and the layer above it cannot move down or merge into it.
// The row's own switches (layerMoveUpEnabled...) are not changed: xochitl rebuilds a row's buttons when they change,
// and Layer 1's arrows vanished and came back (developer, cb57b732). Only the buttons are touched: hidden on the
// Text objects row, disabled in place (they look the same as xochitl's disabled ones) on its neighbours.
void forEachAction(QQuickItem *row, const std::function<void(QQuickItem *, const QString &)> &fn) {
    QList<QQuickItem *> stack{row};
    while (!stack.isEmpty()) {
        QQuickItem *x = stack.takeLast();
        if (x != row && x->metaObject()->indexOfProperty("actionKey") >= 0) { fn(x, x->property("actionKey").toString()); continue; }
        stack << x->childItems();
    }
}

void lockTextLayerRows() {
    QList<QQuickItem *> rows;
    findAnywhere([&rows](QQuickItem *i) {
        if (classStarts(i, "LayerComponent") && i->isVisible()) rows << i;
        return false;
    });
    if (rows.isEmpty()) return;
    int t = -1;
    for (QQuickItem *r : rows) if (r->property("layerName").toString() == QLatin1String(kTextLayerName)) t = r->property("layerIndex").toInt();
    if (t < 0) return;
    for (QQuickItem *r : rows) {
        const int i = r->property("layerIndex").toInt();
        if (i == t) {
            forEachAction(r, [](QQuickItem *b, const QString &) { if (b->isVisible()) b->setVisible(false); });
            if (QObject *in = r->property("textInput").value<QObject *>()) if (!in->property("readOnly").toBool()) in->setProperty("readOnly", true);
        } else if (i == t - 1) {   // just below: moving it up would move the Text objects layer down
            forEachAction(r, [](QQuickItem *b, const QString &k) { if (k == QLatin1String("up") && b->isEnabled()) b->setEnabled(false); });
        } else if (i == t + 1) {   // just above: moving down or merging would go into the Text objects layer
            forEachAction(r, [](QQuickItem *b, const QString &k) {
                if ((k == QLatin1String("down") || k == QLatin1String("merge")) && b->isEnabled()) b->setEnabled(false);
            });
        }
    }
}

// Applied before every frame (QQuickWindow::afterAnimating, GUI thread, before the scene is synced) for a while
// after the layers button is pressed, so the rows never show their buttons (applied after creation, they showed for a
// moment: developer, e10a6f86).
QElapsedTimer g_layersMenuPressed;

void watchLayersMenu() {
    QQuickItem *menu = findAnywhere([](QQuickItem *i) { return i->objectName() == QLatin1String("layersMenu"); });
    QQuickItem *btn = menu ? findItem(menu, [](QQuickItem *i) { return i->metaObject()->indexOfSignal("pressed()") >= 0; }) : nullptr;
    lockTextLayerRows();
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
            if (g_layersMenuPressed.isValid() && g_layersMenuPressed.elapsed() < 4000) lockTextLayerRows();
        }, Qt::DirectConnection);
    }
}

// The document closing (its close button pressed) while the Text objects layer is current and not picked by the
// user (an object selected there): the user's layer current first, so the page is not saved on that layer (then it
// opened there next time: the cause of the developer's "테" on 0e3040b5).
void closeGuard() {
    QString page;
    QObject *sc = pageScene(&page);
    if (!sc || g_userTextLayer) return;
    const int t = textLayer(sc);
    if (t < 0 || sc->property("currentLayer").toInt() != t) return;
    int home = g_ls.active ? g_ls.home : -1;
    if (home < 0) for (int i = sc->property("layerCount").toInt() - 1; i >= 0; --i) if (i != t) { home = i; break; }
    if (home < 0) return;
    g_ls.active = false;
    QMetaObject::invokeMethod(sc, "clearSelectedItems");
    setLayer(sc, home);
    TLOG("text layer: the document closes: layer %d current first", home + 1);
}

void watchCloseButton() {
    QQuickItem *b = findAnywhere([](QQuickItem *i) { return i->objectName() == QLatin1String("close-documentview-button") && i->isVisible(); });
    if (!b || b->property("_rmppTextobjClose").toBool()) return;
    const int sig = b->metaObject()->indexOfSignal("pressed()");
    if (sig < 0) return;
    b->setProperty("_rmppTextobjClose", true);
    auto *hook = new SignalHook;
    hook->setParent(b);
    hook->fn = [] { closeGuard(); };
    QMetaObject::connect(b, sig, hook, QObject::staticMetaObject.methodCount(), Qt::DirectConnection, nullptr);
}
