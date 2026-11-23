// ---- the Text objects layer (#3) ----
// Text objects live on a layer of their own ("Text objects", at the bottom), so writing and erasing on the user's
// layer never touch them. docs/text-layer.md has the why. In short (design S', after hidden layer switching kept
// leaking into undo and redo): every layer change is an undo step in xochitl and cannot be hidden, so the layer is
// only made current by the user's own action:
// - a conversion adds the object to the Text objects layer while the user's layer stays current (unselected);
// - a finger double tap on an object makes that layer current (a step the user sees), selects and edits it;
// - on that layer, choosing a pen, eraser or text tool makes the user's writing layer current again;
// - an undo or redo that only changes the current layer is followed by one more (edit.h layerChangeCheck).
// Nothing is done when a page opens.

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

// The user's writing layer: the topmost layer that is not the Text objects layer.
int writingLayer(QObject *sc) {
    const int t = textLayer(sc);
    for (int i = sc->property("layerCount").toInt() - 1; i >= 0; --i) if (i != t) return i;
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

QSet<QString> layerKeys(QObject *sc) {
    QSet<QString> keys;
    for (const QRect &r : pageLineRects(sc)) keys.insert(rectKey(r));
    return keys;
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

// The Text objects layer of the page, made if there is none: addLayer (on top, made current), its name, moved to
// the bottom (below the user's layers, as xochitl's Text row is), the user's layer current again; four undo steps,
// undone with the conversion (LastConv::steps). The move is checked: not where expected, the layer is taken as found.
// done(layer, steps, the user's layer now).
void ensureTextLayer(QPointer<QObject> scp, int home, std::function<void(int, int, int)> done) {
    const int t = textLayer(scp);
    if (t >= 0) { done(t, 0, home); return; }
    if (!layerApi(scp)) { done(-1, 0, home); return; }
    g_ownLayerChange.start();
    QMetaObject::invokeMethod(scp, "addLayer");
    QTimer::singleShot(400, scp, [scp, home, done] {
        if (!scp) return;
        const int t = scp->property("layerCount").toInt() - 1;
        if (t <= home || scp->property("currentLayer").toInt() != t) {
            TLOG("text layer: addLayer did not add a current layer (%d layers, current %d)", t + 1,
                 scp->property("currentLayer").toInt());
            done(-1, 1, home);
            return;
        }
        QMetaObject::invokeMethod(scp, "setLayerName", Q_ARG(int, t), Q_ARG(QString, QString::fromLatin1(kTextLayerName)));
        QTimer::singleShot(300, scp, [scp, home, t, done] {
            if (!scp) return;
            const int mv = moveLayerMethod(scp);
            int steps = 3, text = t, h = home;
            if (mv >= 0 && t > 0) {
                int from = t, to = 0;
                void *a[] = {nullptr, &from, &to};
                g_ownLayerChange.start();
                scp->qt_metacall(QMetaObject::InvokeMetaMethod, mv, a);
                steps = 4;
                text = 0;
                h = home + 1;   // the user's layers moved up by one
            }
            QTimer::singleShot(300, scp, [scp, text, h, steps, done] {
                if (!scp) return;
                setLayer(scp, h);
                QTimer::singleShot(400, scp, [scp, text, h, steps, done] {
                    if (!scp) return;
                    const int t2 = textLayer(scp), cur = scp->property("currentLayer").toInt();
                    TLOG("text layer: %s (layer %d, user's layer %d)", t2 == text && cur == h ? "added" : "NOT where expected",
                         t2 + 1, cur + 1);
                    if (t2 < 0 || cur == t2) { done(-1, steps, h); return; }
                    done(t2, steps, cur);
                });
            });
        });
    });
}

// Where the objects on the Text objects layer are, per page: from conversions, and read whenever that layer is
// current. A double tap far from all of them does not make that layer current (a step) just to look.
// Kept in textlayer.json ({page: [[x, y, w, h], ...]}) so they hold after a restart (records have no page position).
QHash<QString, QList<QRectF>> g_textBoxes;
bool g_textBoxesLoaded = false;

QString textBoxesPath() { return QLatin1String(kDataDir) + QStringLiteral("/textlayer.json"); }

void loadTextBoxes() {
    if (g_textBoxesLoaded) return;
    g_textBoxesLoaded = true;
    QFile f(textBoxesPath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = o.begin(); it != o.end(); ++it) {
        QList<QRectF> boxes;
        for (const QJsonValue &v : it.value().toArray()) {
            const QJsonArray a = v.toArray();
            if (a.size() == 4) boxes << QRectF(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble());
        }
        g_textBoxes.insert(it.key(), boxes);
    }
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

void rememberTextBox(const QString &page, const QRectF &box) {
    loadTextBoxes();
    g_textBoxes[page] << box;
    saveTextBoxes();
}

void textLayerSeen() {
    QQuickItem *dv = documentView();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    const QString page = dv ? dv->property("currentPageId").toString() : QString();
    if (!sc || page.isEmpty() || sc->property("currentLayer").toInt() != textLayer(sc)) return;
    loadTextBoxes();
    QList<QRectF> boxes;
    for (const Placement &p : findObjects(sc, page, false).found) boxes << p.box;
    if (g_textBoxes.value(page) == boxes) return;
    g_textBoxes[page] = boxes;
    saveTextBoxes();
}

// Is `at` on a text object of the Text objects layer (one of the boxes known for the page)? A tap anywhere else does
// nothing.
bool textObjectAt(const QString &page, const QPointF &at) {
    loadTextBoxes();
    for (const QRectF &b : g_textBoxes.value(page)) if (b.adjusted(-20, -20, 20, 20).contains(at)) return true;
    return false;
}

// The Text objects layer current: object mode. The selection tool is turned on when the user picks it (not when a
// page opens on it); choosing another tool (pen, eraser, text) makes the writing layer current, a step the user
// started by choosing the tool, so ink never goes onto the Text objects layer.
QPointer<QTimer> g_objectMode;
QPointer<QQuickItem> g_objectModeTool;   // the tool when the watch started: only a change of tool counts

void watchObjectMode() {
    g_objectModeTool = activeToolButton();
    if (!g_objectMode) {
        g_objectMode = new QTimer(qApp);
        g_objectMode->setInterval(150);
        QObject::connect(g_objectMode, &QTimer::timeout, qApp, [] {
            QString page;
            QObject *sc = pageScene(&page);
            const int t = textLayer(sc);
            if (!sc || t < 0 || sc->property("currentLayer").toInt() != t) { g_objectMode->stop(); return; }
            QQuickItem *tool = activeToolButton();
            if (tool == g_objectModeTool) return;
            g_objectModeTool = tool;
            if (selectionToolActive() || !tool || g_edit.box) return;
            const int home = writingLayer(sc);
            g_objectMode->stop();
            if (home < 0) return;
            textLayerSeen();
            setLayer(sc, home);
            TLOG("text layer: another tool chosen on it: layer %d current for writing", home + 1);
        });
    }
    g_objectMode->start();
}

void layerChanged() {
    QString page;
    QObject *sc = pageScene(&page);
    if (!sc) return;
    const int t = textLayer(sc);
    if (t < 0 || sc->property("currentLayer").toInt() != t) return;
    textLayerSeen();
    const bool ours = g_ownLayerChange.isValid() && g_ownLayerChange.elapsed() < 1500;
    const bool step = g_ownUndo.isValid() && g_ownUndo.elapsed() < 1500;
    if (!ours && !step && !(g_anyUserStep.isValid() && g_anyUserStep.elapsed() < 1500)) {
        TLOG("text layer: picked by the user");
        useSelectionTool();
    }
    QTimer::singleShot(400, qApp, [] { watchObjectMode(); });   // after a tool switch, so it is not taken for a change
}

void watchLayers() {
    QQuickItem *dv = documentView();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    if (!sc || sc->property("_rmppTextobjLayers").toBool()) return;
    const int sig = sc->metaObject()->indexOfSignal("currentLayerChanged()");
    if (sig < 0) return;
    sc->setProperty("_rmppTextobjLayers", true);
    // nothing on open: a page that opens on the Text objects layer gets only the watch for another tool
    QPointer<QObject> scp(sc);
    QTimer::singleShot(3000, qApp, [scp] {
        if (!scp) return;
        const int t = textLayer(scp);
        if (t >= 0 && scp->property("currentLayer").toInt() == t) {
            TLOG("text layer: the page opened on it: left as it is");
            watchObjectMode();
        }
    });
    auto *hook = new SignalHook;
    hook->setParent(sc);
    hook->fn = [] { QTimer::singleShot(0, qApp, [] { layerChanged(); }); };
    QMetaObject::connect(sc, sig, hook, QObject::staticMetaObject.methodCount(), Qt::DirectConnection, nullptr);
}

// ---- the layers menu ----
// The Text objects row: no move up or down, no merge, no rename; delete stays (developer: it must be deletable). The
// layer below it cannot move up past it, the one above it cannot move down or merge into it. Only the button items are
// touched (hidden on the row, disabled in place on its neighbours): changing a row's layer*Enabled switches made
// xochitl rebuild its buttons (they flickered). Applied before every frame for 4 s after the layers button is
// pressed, so nothing shows for a frame.
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
            forEachAction(r, [](QQuickItem *b, const QString &k) {
                if ((k == QLatin1String("up") || k == QLatin1String("down") || k == QLatin1String("merge")) && b->isVisible())
                    b->setVisible(false);
            });
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
