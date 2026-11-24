// rmpp-textobj: text objects for xochitl (reMarkable Paper Pro), loaded with rmpp-preload.
// A text object is typed text drawn as ordinary pen strokes in a stroke font ("ink font"): lasso-selectable, movable,
// erasable and undoable like handwriting, and editable as text (double tap). Glyphs come from rmpp-lang's language
// add-ons (glyphs.rmgl per language) and this package's ASCII glyphs.json.
//
// What it adds to xochitl:
//   - T (typing) tool menu > "Add text object": tap where it goes, type, OK.
//   - Selection tool: double tap on a text object selects it whole; double tap on a selected one opens the edit box.
//   - A lasso that covers part of a text object selects all of it (at least half of its strokes inside) or none of
//     it, so a text object always moves, copies and deletes whole.
//   - A text object that was partly erased becomes plain strokes.
//   - A C API for other add-ons (rmpp-hwr draws recognized handwriting as a text object): see the end of this file.
//
// Records: carried in each object's own strokes (src/carry.h), no record file. Files: /home/root/.vellum/share/rmpp-textobj/glyphs.json (Vellum package; else the data dir).
#include <dlfcn.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QDirIterator>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMetaMethod>
#include <QtCore/QPointer>
#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QWindow>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlPropertyMap>
#include <QtQuick/QQuickImageProvider>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

namespace {

#define TLOG(...) qWarning("[rmpp-textobj] " __VA_ARGS__)

const char *kDataDir = "/home/root/.local/share/rmpp-textobj";        // textlayer.json (hints), icon
const char *kPkgDir = "/home/root/.vellum/share/rmpp-textobj";        // Vellum package: glyphs.json
const char *kConf = "/home/root/.config/rmpp-textobj/settings.conf";

// edit box state (inkText / inkWeight / inkSize / inkMissing), command (written by QML)
QQmlPropertyMap *g_state = nullptr;

// ---- settings ----
bool g_confLoaded = false;
void loadSettings() {
    if (g_confLoaded || !QFileInfo::exists(QStringLiteral("/home/root/.local"))) return;   // /home mounts late
    g_confLoaded = true;
    QSettings s(QLatin1String(kConf), QSettings::IniFormat);
    Q_UNUSED(s);
    // (snapping highlights to the characters follows xochitl's "Snap to text" switch now: snapToTextOn, highlight.h;
    // the old snapHighlights setting is no longer read)
}

void saveSetting(const QString &key, const QVariant &v) {
    g_state->insert(key, v);
    QDir().mkpath(QFileInfo(QLatin1String(kConf)).path());
    QSettings s(QLatin1String(kConf), QSettings::IniFormat);
    s.setValue(key, v);
    s.sync();
}

// ---- item tree helpers ----
QQuickItem *findItem(QQuickItem *root, const std::function<bool(QQuickItem *)> &pred, int depth = 0) {
    if (!root || depth > 250) return nullptr;
    if (pred(root)) return root;
    const auto kids = root->childItems();
    for (QQuickItem *k : kids)
        if (QQuickItem *f = findItem(k, pred, depth + 1)) return f;
    return nullptr;
}

QQuickItem *findAnywhere(const std::function<bool(QQuickItem *)> &pred) {
    for (QWindow *w : QGuiApplication::allWindows())
        if (auto *qw = qobject_cast<QQuickWindow *>(w))
            if (QQuickItem *f = findItem(qw->contentItem(), pred)) return f;
    return nullptr;
}

bool classStarts(QObject *o, const char *prefix) {
    return qstrncmp(o->metaObject()->className(), prefix, qstrlen(prefix)) == 0;
}

QQuickItem *documentView() {
    return findAnywhere([](QQuickItem *i) { return classStarts(i, "DocumentView_QMLTYPE") && i->isVisible(); });
}

// The view that draws the open page (DeviceSceneView, a child of the DocumentView; it also has viewToScene(),
// sceneToView(), endItemSelection() and the signal closeToolbarFoldout()).
QQuickItem *sceneViewOf(QQuickItem *dv) {
    return dv ? findItem(dv, [](QQuickItem *i) { return classStarts(i, "DeviceSceneView_QMLTYPE") && i->isVisible(); }) : nullptr;
}

QObject *createInline(QQmlEngine *eng, QQmlContext *ctx, const char *qml, const char *name, QQuickItem *parent,
                      const QVariantMap &init = {}) {
    QQmlComponent comp(eng);
    comp.setData(QByteArray(qml), QUrl(QStringLiteral("qrc:/rmpptextobj/%1.qml").arg(QLatin1String(name))));
    QObject *o = comp.createWithInitialProperties(init, ctx);
    if (!o) {
        for (const auto &e : comp.errors()) TLOG("%s: %s", name, qPrintable(e.toString()));
        return nullptr;
    }
    o->setParent(parent);
    if (auto *item = qobject_cast<QQuickItem *>(o)) item->setParentItem(parent);
    return o;
}

// A QML component in its own context with "tobj" = g_state, as a child of parent.
QObject *createWithState(QQuickItem *parent, const char *qml, const char *name, const QVariantMap &init = {}) {
    QQmlEngine *eng = parent ? qmlEngine(parent) : nullptr;
    QQmlContext *base = parent ? qmlContext(parent) : nullptr;
    if (!eng || !base) return nullptr;
    auto *ctx = new QQmlContext(base);
    ctx->setContextProperty(QStringLiteral("tobj"), g_state);
    QObject *o = createInline(eng, ctx, qml, name, parent, init);
    if (!o) { delete ctx; return nullptr; }
    ctx->setParent(o);
    return o;
}

// Calls fn on a signal of an object we have no headers for (like QSignalSpy: overrides qt_metacall).
class SignalHook : public QObject {
public:
    std::function<void()> fn;
    int qt_metacall(QMetaObject::Call c, int id, void **a) override {
        id = QObject::qt_metacall(c, id, a);
        if (id < 0 || c != QMetaObject::InvokeMetaMethod) return id;
        if (id == 0 && fn) fn();
        return -1;
    }
};

// A tap on an item through its window (emitting a tool's clicked() does not switch tools; a tap does).
void tapItem(QQuickItem *item) {
    QQuickWindow *win = item ? item->window() : nullptr;
    if (!win) return;
    const QPointF c(item->width() / 2, item->height() / 2);
    const QPointF p = item->mapToScene(c), g = item->mapToGlobal(c);
    QMouseEvent press(QEvent::MouseButtonPress, p, p, g, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, p, p, g, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(win, &press);
    QCoreApplication::sendEvent(win, &release);
}

QQuickItem *selectionToolButton() {
    return findAnywhere([](QQuickItem *i) { return i->objectName() == QLatin1String("selectionButton"); });
}

bool selectionToolActive() {
    QQuickItem *t = selectionToolButton();
    return t && t->property("selected").toBool();
}

// The selection tool, made active if it is not (selection calls select nothing while the pen is active).
// Returns true if it had to be switched.
bool useSelectionTool() {
    QQuickItem *tool = selectionToolButton();
    if (!tool || tool->property("selected").toBool()) return false;
    tapItem(tool);
    return true;
}

// ---- hints: a short message at the top of the page ----
const char *kHintQml = R"QML(
import QtQuick
Rectangle {
    property string message: ""
    width: Math.min(label.implicitWidth + 80, (parent ? parent.width : 1620) - 120); height: label.height + 50
    x: ((parent ? parent.width : 1620) - width) / 2; y: 40
    z: 10001
    color: "white"; border.color: "black"; border.width: 3; radius: 8
    Text { id: label; width: parent.width - 80; anchors.centerIn: parent; text: parent.message; font.pixelSize: 34; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter }
}
)QML";
QPointer<QObject> g_hint;

void showHint(QQuickItem *dv, const QString &text, int ms = 4000) {
    if (g_hint) g_hint->deleteLater();
    TLOG("hint: %s", qPrintable(text));
    if (!dv) return;
    QQmlEngine *eng = qmlEngine(dv);
    QQmlContext *ctx = qmlContext(dv);
    g_hint = (eng && ctx) ? createInline(eng, ctx, kHintQml, "Hint", dv, {{QStringLiteral("message"), text}}) : nullptr;
    if (g_hint) QTimer::singleShot(ms, g_hint, [o = g_hint] { if (o) o->deleteLater(); });
}

// The passcode screen is up (no UI work then: it leaves ghosting there).
bool passcodeScreenUp() {
    for (QWindow *w : QGuiApplication::allWindows())
        if (auto *qw = qobject_cast<QQuickWindow *>(w))
            if (qw->isVisible() && qw->activeFocusItem() && QByteArray(qw->activeFocusItem()->metaObject()->className()).contains("Pin"))
                return true;
    return findAnywhere([](QQuickItem *i) {
        if (!i->isVisible()) return false;
        const QByteArray c = i->metaObject()->className();
        return c.contains("Passcode") || c.contains("Pincode") || c.contains("PinKeyboard");
    }) != nullptr;
}

bool uiReady() {
    static bool ready = false;
    static int clearTicks = 0;
    if (ready) return true;
    if (passcodeScreenUp()) { clearTicks = 0; return false; }
    if (++clearTicks < 6) return false;   // 9 s without the passcode screen before touching the QML engine
    ready = true;
    TLOG("UI ready");
    return true;
}

#include "glyphs.h"
#include "ink.h"
#include "carry.h"
#include "objects.h"
#include "highlight.h"
#include "edit.h"
#include "addtext.h"
#include "select.h"
#include "layer.h"

} // namespace

#include "api.h"   // extern "C": the functions other add-ons call

namespace {

void textobjInit() {
    auto *app = QCoreApplication::instance();
    g_state = new QQmlPropertyMap(app);
    g_state->insert(QStringLiteral("command"), QString());
    g_state->insert(QStringLiteral("inkText"), QString());
    g_state->insert(QStringLiteral("inkWeight"), 1);
    g_state->insert(QStringLiteral("inkSize"), 100);
    g_state->insert(QStringLiteral("inkMissing"), QString());
    QObject::connect(g_state, &QQmlPropertyMap::valueChanged, app, [](const QString &key, const QVariant &v) {
        if (key == QLatin1String("inkText")) { updateInkMissing(v.toString()); return; }
        if (key != QLatin1String("command")) return;
        const QString cmd = v.toString().section(QLatin1Char('#'), 0, 0);
        if (cmd == QLatin1String("addtext")) startAddTextObject();
        else if (cmd == QLatin1String("addtextcancel")) closeAddPlace();
        else if (cmd.startsWith(QLatin1String("addtextat:")))
            placeTextObject(cmd.section(QLatin1Char(':'), 1, 1).toDouble(), cmd.section(QLatin1Char(':'), 2, 2).toDouble());
        else if (cmd == QLatin1String("inkok")) finishInkEdit(true);
        else if (cmd == QLatin1String("inkcancel")) finishInkEdit(false);
        else if (cmd.startsWith(QLatin1String("stap:")))
            singleTapAt(cmd.section(QLatin1Char(':'), 1, 1).toDouble(), cmd.section(QLatin1Char(':'), 2, 2).toDouble());
        else if (cmd.startsWith(QLatin1String("dtap:")))
            doubleTapAt(cmd.section(QLatin1Char(':'), 1, 1).toDouble(), cmd.section(QLatin1Char(':'), 2, 2).toDouble());
    });
    auto *t = new QTimer(app);
    QObject::connect(t, &QTimer::timeout, app, [] {
        if (!uiReady()) return;
        loadSettings();
        tryInjectAddText();
        watchSelection();
        watchDoubleTap();
        watchUndoRedo();
        watchLayers();
        watchLayersMenu();
        installTapOverlay();
        installBoxOverlay();
        closeStaleInkEdit();
        closeStaleAddPlace();
        checkErasedObjects();
        releaseIdleGlyphs();
    });
    t->start(1500);
    TLOG("loaded (API %d)", kApiVersion);
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(textobjInit)
