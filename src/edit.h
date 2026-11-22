// ---- drawing a text object and editing one ----

bool g_putting = false;   // our own ink is being added and selected (the selection rules leave it alone)
QElapsedTimer g_putStarted;

bool putting() { return g_putting && g_putStarted.isValid() && g_putStarted.elapsed() < 8000; }

// The strokes to lasso for new ink: the glyph strokes, and the corners of its highlight bands.
QList<QList<QPointF>> inkAndBands(const Placed &pl, const QList<Hl> &hls) {
    QList<QList<QPointF>> out = pl.strokes;
    for (const Hl &h : hls)
        for (const QRectF &r : h.rects) out.append({r.topLeft(), r.bottomRight()});
    return out;
}

// After new ink is added and selected: record it from the selection's stroke rects (removing the records in
// `removeIds`, the object it replaces). The selection settles after a while (longer for big ink), so measure again
// until it holds all n new strokes and the highlight bands; select again if it is still empty (slow tool switch).
// Bands that did not come (not drawn, not selected) are left out after a few tries.
void recordNew(QPointer<QObject> scp, const QString &page, const QString &text, const Placed &pl, int n,
               const QStringList &removeIds, const QList<Hl> &hls = {}, int attempt = 0) {
    QTimer::singleShot(attempt == 0 ? 300 : 400, qApp, [=] {
        if (!scp) { g_putting = false; return; }
        const int nb = bandCount(hls);
        const int count = scp->property("selectionItemCount").toInt();
        const bool full = count == n + nb, glyphsOnly = nb > 0 && count == n && attempt >= 4;
        const QList<QRect> rects = full || glyphsOnly ? lineRects(scp) : QList<QRect>();
        if (rects.size() != n + nb && !(glyphsOnly && rects.size() == n)) {
            if (count == 0 && (attempt == 1 || attempt == 3)) {
                const bool switched = useSelectionTool();
                TLOG("new ink not selected yet, selecting again%s", switched ? " (selection tool)" : "");
                QTimer::singleShot(switched ? 500 : 0, scp, [scp, pl, hls] { if (scp) selectInk(scp, inkAndBands(pl, hls)); });
            }
            if (attempt < 6) recordNew(scp, page, text, pl, n, removeIds, hls, attempt + 1);
            else {
                TLOG("new ink not selected (%d of %d), not recorded: plain strokes", count, n + nb);
                g_putting = false;
            }
            return;
        }
        QList<Hl> found = hls;
        const QList<QRect> glyphs = nb > 0 ? splitBands(rects, &found) : rects;
        if (glyphs.size() != n) {
            TLOG("new ink: %lld strokes are not %d glyphs and %d bands: not recorded", (long long)rects.size(), n, nb);
            g_putting = false;
            return;
        }
        updateRecords(makeRecord(page, text, glyphs, pl.origin, pl.em, pl.weight, QString(), pl.lines, hlJson(found)), removeIds);
        TLOG("recorded: %d strokes, %lld highlight(s)", n, (long long)found.size());
        QTimer::singleShot(600, qApp, [] { g_putting = false; });   // after the selection count settled
    });
}

// Adds the strokes of pl as a text object: ends (or deletes) the selection, adds the strokes once the scene has
// settled, selects them (the user moves the new object right away; double tap edits it) and records them.
// The page is redrawn once: fullRefresh (after the edit box: it and the keyboard left ghosting) = one ghost-buster
// refresh; otherwise one updateAfterStroke.
// Highlights (hls, band rects in page units) are drawn first, under the glyphs.
void putInk(QPointer<QObject> scp, QPointer<QObject> php, QPointer<QQuickItem> dvp, int layer, bool deleteSelection,
            const Placed &pl, const QString &text, const QStringList &removeIds, bool fullRefresh = false,
            const QList<Hl> &hls = {}) {
    g_putting = true;
    g_putStarted.start();
    // addDrawingLine is ignored while a selection is active
    if (deleteSelection) QMetaObject::invokeMethod(scp, "deleteSelectedItems", Q_ARG(int, layer));
    else endSelection(scp, dvp);
    QTimer::singleShot(400, scp, [=] {
        if (!scp || !php) { g_putting = false; return; }
        const int bands = hls.isEmpty() ? 0 : addBands(scp, hls);
        const int added = addInkStrokes(scp, php, pl.strokes, pl.em, pl.weight);
        if (added > 0) {
            selectInk(scp, inkAndBands(pl, bands > 0 ? hls : QList<Hl>()));
            recordNew(scp, dvp ? dvp->property("currentPageId").toString() : scp->property("pageId").toString(), text, pl,
                      added, removeIds, bands > 0 ? hls : QList<Hl>());
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
    property int sizePct: 100     // size of the ink, relative to now
    property int rev: 0           // bumps the preview image
    function refresh() { tobj.inkText = input.text; tobj.inkWeight = weight; tobj.inkSize = sizePct; rev++; }
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
                onTextChanged: typing.restart()
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
        ArkControls.Button { text: "+"; type: ArkTokens.Button.secondary; onClicked: shade.sizePct = Math.min(200, shade.sizePct + 10) }
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
};
InkEdit g_edit;

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
        const double scale = qBound(50, g_state->value(QStringLiteral("inkSize")).toInt(), 200) / 100.0;
        const int weight = qBound(0, g_state->value(QStringLiteral("inkWeight")).toInt(), 2);
        const double em = g_edit.em * scale;
        const Placed pl = placeText(g_state->value(QStringLiteral("inkText")).toString().trimmed(), QPointF(12, 8), em,
                                    g_edit.maxW * scale);
        static const double divisor[] = {32.0, 20.0, 13.0};
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
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
    g_edit = e;   // before the box: its preview reads em and maxW
    g_state->insert(QStringLiteral("inkMissing"), missingGlyphs(text));
    QObject *o = createWithState(dv, kEditQml, "InkEdit",
                                 {{QStringLiteral("initial"), text}, {QStringLiteral("penManager"), QVariant::fromValue(mgr)},
                                  {QStringLiteral("weight"), weight}, {QStringLiteral("title"), title}});
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
                e.hls.append(h);   // drawn before this build: colour unknown, yellow
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
    const QString text = g_state->value(QStringLiteral("inkText")).toString().trimmed();
    const int weight = qBound(0, g_state->value(QStringLiteral("inkWeight")).toInt(), 2);
    const double size = qBound(50, g_state->value(QStringLiteral("inkSize")).toInt(), 200) / 100.0;
    if (!e.create && !e.text0.isEmpty() && text == e.text0 && weight == e.weight0 && qFuzzyCompare(size, 1.0))
        return false;   // nothing changed
    if (blankText(text)) {
        if (!e.create) {   // empty (or only spaces): the object is deleted, strokes and record
            QMetaObject::invokeMethod(e.sc, "deleteSelectedItems", Q_ARG(int, e.layer));
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
        saveSetting(QStringLiteral("addTextEm"), em);   // the next new object starts at this size
        // selected and recorded like any new object: needs the selection tool (with the pen, selecting does nothing)
        const bool switchTool = useSelectionTool();
        QTimer::singleShot(switchTool ? 600 : 0, qApp, [e, pl, text] {
            putInk(e.sc, e.ph, e.dv, e.layer, false, pl, text, {}, true);
        });
        return true;
    }
    const QList<Hl> hls = placeHighlights(e.hls, e.joined, text, pl.lines);
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
