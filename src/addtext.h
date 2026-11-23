// "Add text object": a text object (ink in the glyph font) from typed text,
// without handwriting first. An entry in xochitl's typing (T) tool menu, under "Add text"; then a tap on the page
// marks where the text goes, the edit box opens empty, and on OK the text is drawn there (ink.h applyInkEdit,
// create mode). As xochitl does when the pen taps outside a selection, a selection on the page is cleared first.

const char *kAddTextItemQml = R"QML(
import QtQuick
import QtQuick.Layouts
import ark.controls as ArkControls
ArkControls.FoldoutItem {
    objectName: "rmppTextobjAddText"
    Layout.fillWidth: true
    label: "Add text object"
    focusPolicy: Qt.NoFocus   // keeps the edit box's text field the focus item (else typing goes nowhere)
    iconSource: tobjIcon
    onClicked: tobj.command = "addtext#" + Date.now()
}
)QML";

const char *kPlaceQml = R"QML(
import QtQuick
import ark.tokens as ArkTokens
import ark.controls as ArkControls
import xofm.libs.peninput
Item {
    id: place
    property var penManager: null
    anchors.fill: parent
    z: 10000
    // the pen draws nothing while placing; a tap (pen or finger) anywhere on the page places the text
    PenInputBlocker { anchors.fill: parent; manager: place.penManager }
    MouseArea {
        anchors.fill: parent
        onClicked: (m) => tobj.command = "addtextat:" + m.x + ":" + m.y + "#" + Date.now()
    }
    Rectangle {
        width: row.width + 60; height: 110
        x: (parent.width - width) / 2; y: 40
        color: "white"; border.color: "black"; border.width: 3; radius: 8
        MouseArea { anchors.fill: parent }   // taps on the banner do not place
        Row {
            id: row
            anchors.centerIn: parent; spacing: 30
            Text { text: "Tap where the text object goes"; font.pixelSize: 34; anchors.verticalCenter: parent.verticalCenter }
            ArkControls.Button { text: "Cancel"; type: ArkTokens.Button.secondary; onClicked: tobj.command = "addtextcancel#" + Date.now() }
        }
    }
}
)QML";

struct AddPlace {
    QPointer<QObject> overlay;
    QPointer<QQuickItem> dv;
    QString page;
};
AddPlace g_place;

void closeAddPlace() {
    if (g_place.overlay) g_place.overlay->deleteLater();
    g_place = AddPlace{};
}

// Called from the 1.5 s timer: the notebook was closed or the page turned while placing.
void closeStaleAddPlace() {
    if (!g_place.overlay) return;
    if (!g_place.dv || !g_place.dv->isVisible() || g_place.dv->property("currentPageId").toString() != g_place.page)
        closeAddPlace();
}

// A point in DocumentView coordinates -> page coordinates (scene units: x centred on 0, -810..810; y from the top),
// through the view's own viewToScene (follows zoom and scrolling).
bool viewToPage(QQuickItem *dv, const QPointF &p, QPointF *out) {
    QQuickItem *sv = sceneViewOf(dv);
    if (!sv) return false;
    QVariant r;
    if (!QMetaObject::invokeMethod(sv, "viewToScene", Q_RETURN_ARG(QVariant, r), Q_ARG(QVariant, QVariant(sv->mapFromItem(dv, p)))))
        return false;
    if (r.canConvert<QJSValue>()) r = r.value<QJSValue>().toVariant();
    if (!r.canConvert<QPointF>()) return false;
    *out = r.toPointF();
    return true;
}

// The em size for a new object: the size the last new object was made at (its size in the edit box included), else
// about the size of xochitl's body text. Not converted handwriting's: that follows the handwriting and is big.
double defaultTextObjectEm() {
    const double em = g_state->value(QStringLiteral("addTextEm")).toDouble();
    return em > 0 ? qBound(20.0, em, 200.0) : 40.0;
}

// Closes the typing tool's menu (foldout), as picking one of xochitl's own entries does.
void closeTypingMenu(QQuickItem *dv) {
    QQuickItem *sv = dv ? sceneViewOf(dv) : nullptr;
    if (sv && QMetaObject::invokeMethod(sv, "closeToolbarFoldout")) return;
    QQuickItem *menu = findAnywhere([](QQuickItem *i) { return i->objectName() == QLatin1String("typingMenu"); });
    QQuickItem *fold = menu ? findItem(menu, [](QQuickItem *i) { return classStarts(i, "ToolbarFoldout"); }) : nullptr;
    if (fold && fold->metaObject()->indexOfMethod("close()") >= 0 && QMetaObject::invokeMethod(fold, "close")) return;
    QQuickItem *btn = fold ? fold->parentItem() : nullptr;   // the T button; its foldout is open while it is checked
    if (btn && btn->property("checked").toBool()) btn->setProperty("checked", false);
}

// T menu > Add text object: close the menu, then wait for the tap that places the text.
void startAddTextObject() {
    if (g_edit.box) return;
    QQuickItem *dv = documentView();
    if (!glyphsAvailable()) { showHint(dv, QStringLiteral("No ink font installed")); return; }
    closeTypingMenu(dv);
    if (!dv) return;
    closeAddPlace();
    QObject *pin = dv->property("penInput").value<QObject *>();
    QObject *mgr = pin ? pin->property("surfaceManager").value<QObject *>() : nullptr;
    QObject *o = createWithState(dv, kPlaceQml, "AddTextPlace", {{QStringLiteral("penManager"), QVariant::fromValue(mgr)}});
    if (!o) return;
    g_place = AddPlace{o, dv, dv->property("currentPageId").toString()};
}

// The tap: x, y in DocumentView coordinates.
void placeTextObject(double x, double y) {
    QPointer<QQuickItem> dv = g_place.dv;
    closeAddPlace();
    QObject *sc = dv ? dv->property("sceneController").value<QObject *>() : nullptr;
    QObject *ph = dv ? dv->property("penHandler").value<QObject *>() : nullptr;
    QPointF at;
    if (!sc || !ph || !viewToPage(dv, QPointF(x, y), &at)) return;
    TLOG("add text object at view %.0f,%.0f = page %.1f,%.1f", x, y, at.x(), at.y());
    InkEdit e;
    e.sc = sc; e.ph = ph; e.layer = sc->property("currentLayer").toInt();
    e.create = true;
    e.em = defaultTextObjectEm();
    e.origin = at - QPointF(0, e.em * 0.5);   // the tap marks the middle of the first line
    e.maxW = qMax(810.0 - 40.0 - e.origin.x(), e.em * 8);
    g_langHint.clear();
    showInkEditBox(dv, e, QStringLiteral("Add text object"), QString(), 1);
}

// The text-object icon: xochitl's own convert icon ("aa") inside a dashed selection box, composed at run time from
// the icon in xochitl's resources (no reMarkable artwork shipped). The same composition as rmpp-hwr's lasso convert
// icon (ui/hwrui.cpp inkIconUrl), so the two look alike. Written to our data folder, created first: it does not exist
// on a fresh install (rmpp-hwr#15).
QString textObjectIconUrl() {
    static QString url;
    if (!url.isEmpty()) return url;
    QFile in(QStringLiteral(":/ark/icons/hwc"));
    QDir().mkpath(QLatin1String(kDataDir));
    const QString out = QLatin1String(kDataDir) + QStringLiteral("/textobj_icon.svg");
    QString svg;
    if (in.open(QIODevice::ReadOnly)) {
        const QString src = QString::fromUtf8(in.readAll());
        const int a = src.indexOf(QLatin1Char('>'), src.indexOf(QLatin1String("<svg"))) + 1;
        const int b = src.lastIndexOf(QLatin1String("</svg>"));
        if (a > 0 && b > a)
            svg = QStringLiteral("<svg width=\"48\" height=\"48\" viewBox=\"0 0 48 48\" fill=\"none\" xmlns=\"http://www.w3.org/2000/svg\">"
                                 "<rect x=\"2\" y=\"4\" width=\"44\" height=\"40\" stroke=\"black\" stroke-width=\"2.5\" "
                                 "stroke-dasharray=\"5 3.5\" fill=\"none\"/><g transform=\"translate(8.5 7.5) scale(0.64)\">")
                  + src.mid(a, b - a) + QStringLiteral("</g></svg>");
    }
    QFile f(out);
    if (!svg.isEmpty() && f.open(QIODevice::WriteOnly) && f.write(svg.toUtf8()) > 0) {
        f.close();
        url = out;   // xochitl's Icon takes a plain path (a file:// URL is "not found")
    } else {
        url = QStringLiteral("qrc:/ark/icons/ballpoint_tip");
        TLOG("text object icon: could not write %s", qPrintable(out));
    }
    return url;
}

// The typing (T) tool menu: our entry right under xochitl's "Add text" (labelled "Edit text" on a page with text).
void tryInjectAddText() {
    QQuickItem *menu = findAnywhere([](QQuickItem *i) { return i->objectName() == QLatin1String("typingMenu"); });
    QQuickItem *add = menu ? findItem(menu, [](QQuickItem *i) { const QString l = i->property("label").toString();
        return l == QLatin1String("Add text") || l == QLatin1String("Edit text"); }) : nullptr;
    QQuickItem *col = add ? add->parentItem() : nullptr;
    if (!col || col->property("_rmppTextobj").toBool()) return;
    QQmlEngine *eng = qmlEngine(add);
    QQmlContext *base = qmlContext(add);
    if (!eng || !base) return;
    col->setProperty("_rmppTextobj", true);
    auto *ctx = new QQmlContext(base, col);
    ctx->setContextProperty(QStringLiteral("tobj"), g_state);
    ctx->setContextProperty(QStringLiteral("tobjIcon"), textObjectIconUrl());
    auto *item = qobject_cast<QQuickItem *>(createInline(eng, ctx, kAddTextItemQml, "AddTextObject", col));
    if (item) item->stackAfter(add);
    TLOG("typing menu: add text object %s", item ? "added" : "FAILED");
}
