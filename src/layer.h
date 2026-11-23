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
    int pending = 0;       // an undo (1) or redo (2) asked for while the object was selected
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
    QTimer::singleShot(400, scp, [scp, select] {
        if (!scp || !g_ls.active) return;
        if (select) select();
    });
}

// Once the selection on the Text objects layer is made: what is there now (to tell a move at the end).
void textSessionSelected() {
    if (!g_ls.active || !g_ls.sc) return;
    g_ls.start = layerKeys(g_ls.sc);
    g_ls.selecting = false;
    TLOG("text layer: object selected (%lld strokes on the layer)", (long long)g_ls.start.size());
}

void afterPendingStep(bool undo);   // edit.h

// The selection ended: the user's layer back. Untouched: undo S1; moved or resized: S2. A move is applied a moment
// after the selection ends, so the layer is compared 600 ms later; and the undo is checked: if it changed the
// layer's strokes, it took a move (not seen in time), which is redone, and S2 follows. If it changed nothing (an undo
// right after a selection ends is ignored), it is sent again.
void switchBack(QPointer<QObject> scp, int home, int text, int pending, const QSet<QString> &keys, int tries = 0) {
    QObject *sc = scp;
    if (!sc) return;
    if (sc->property("currentLayer").toInt() == home) {
        TLOG("text layer: back to layer %d (switch undone)", home + 1);
        if (pending) QTimer::singleShot(500, qApp, [pending] { afterPendingStep(pending == 1); });
        return;
    }
    if (tries > 0 && layerKeys(sc) != keys) {   // the undo took a move
        ownUndoRedo(sc, false);
        QTimer::singleShot(500, qApp, [scp, home, pending] {
            if (!scp) return;
            setLayer(scp, home);
            TLOG("text layer: the object was moved: back to layer %d (a step of its own)", home + 1);
            if (pending) QTimer::singleShot(500, qApp, [pending] { afterPendingStep(pending == 1); });
        });
        return;
    }
    if (tries >= 3) { TLOG("text layer: the switch back was not taken: left on the text layer"); return; }
    if (tries > 0) TLOG("text layer: switch back sent again");
    ownUndoRedo(sc, true);
    QTimer::singleShot(500, qApp, [scp, home, text, pending, keys, tries] { switchBack(scp, home, text, pending, keys, tries + 1); });
}

void endTextSession() {
    QObject *sc = g_ls.sc;
    g_ls.active = false;
    if (!sc) return;
    const int home = g_ls.home, text = g_ls.text, pending = g_ls.pending;
    const QSet<QString> start = g_ls.start;
    QPointer<QObject> scp(sc);
    QTimer::singleShot(600, qApp, [scp, home, text, pending, start] {
        QObject *sc = scp;
        if (!sc) return;
        if (sc->property("currentLayer").toInt() != text) { TLOG("text layer: ended on another layer"); return; }
        const QSet<QString> keys = layerKeys(sc);
        if (keys != start || start.isEmpty()) {
            setLayer(sc, home);
            TLOG("text layer: object changed, back to layer %d (a step of its own)", home + 1);
            if (pending) QTimer::singleShot(500, qApp, [pending] { afterPendingStep(pending == 1); });
            return;
        }
        switchBack(scp, home, text, pending, keys);
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
        if (n > 0) TLOG("text layer: %d more %s step(s) to leave it", n, undo ? "undo" : "redo");
        return;
    }
    if (n >= 20) { TLOG("text layer: still current after 20 steps"); return; }
    if (!undo && !sc->property("redoAvailable").toBool()) {
        // a redo of a switch we had undone (it stays in the redo history): nothing after it, so take it back
        ownUndoRedo(sc, true);
        TLOG("text layer: redo ended on it: switch undone again");
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

void watchLayers() {
    QQuickItem *dv = documentView();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    if (!sc || sc->property("_rmppTextobjLayers").toBool()) return;
    const int sig = sc->metaObject()->indexOfSignal("currentLayerChanged()");
    const int sel = sc->metaObject()->indexOfSignal("selectionItemCountChanged()");
    if (sig < 0 || sel < 0) return;
    sc->setProperty("_rmppTextobjLayers", true);
    g_userTextLayer = false;
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
        setLayer(scp, home);
        TLOG("text layer: the page opened on it: back to layer %d", home + 1);
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
