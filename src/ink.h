// Drawing text as pen strokes: layout in the ink font, adding strokes to the open page, selecting them.
//
// xochitl 3.29 internals used (no headers; found by walking meta-objects):
//   SceneController::addDrawingLine(Line)                   adds one finished pen stroke to the open page
//   SceneController::cloneSelectedItems(int layer, double)  + getItemBoundingRect(items): the selection's bounds
//   SceneController::selectWithLine(Line)                   lasso selection (selection tool active)
//   SceneController::addSelectionRect(QRect, mode)          mode 0: select the strokes inside; mode 1: take them out
//   SceneController::getLineBoundingRectsToBeSelected([QRect]) bounds of each stroke in a rect (no rect: selected)
//   SceneController::deleteSelectedItems(int layer)         removes the selection (one undo step)
//   Line (88 bytes): 0x00 int tool, 0x04 int color, 0x08 uint argb, 0x10 QList<Point>, 0x28 double thickness
//   Point (14 bytes, packed): float x, y; u16 speed, width; u8 direction, pressure  (same as .rm v6 points)
// The Line layout is checked (size 88) before use; anything else and the feature reports an error instead.

#pragma pack(push, 1)
struct InkPt {
    float x, y;
    quint16 speed, width;
    quint8 direction, pressure;
};
#pragma pack(pop)
static_assert(sizeof(InkPt) == 14, "Line point layout");

// Text with nothing to draw: only spaces, tabs, line breaks, the full-width space (U+3000) or zero-width marks.
bool blankText(const QString &t) {
    for (const uint c : t.toUcs4())
        if (!QChar::isSpace(c) && c != 0x200b && c != 0x200c && c != 0x200d && c != 0x2060 && c != 0xfeff) return false;
    return true;
}

// One line of text in em units (x from 0, y 0 = top of the em box). Returns the width in em.
double layoutLine(const QString &text, QList<QList<QPointF>> *out) {
    double x = 0;
    for (const uint c : text.toUcs4()) {
        const Glyph *it = glyphOf(char32_t(c));
        if (!it) { x += 0.5; continue; }
        for (const auto &s : it->strokes) {
            QList<QPointF> p;
            for (const QPointF &q : s) p.append(QPointF(q.x() + x, q.y()));
            out->append(p);
        }
        x += it->adv + 0.04;
    }
    return x;
}

QList<InkPt> inkPoints(const QList<QPointF> &xy, quint16 width) {
    QList<InkPt> out;
    const int n = int(xy.size());
    for (int i = 0; i < n; ++i) {
        const int j = i + 1 < n ? i + 1 : i - 1;
        double a = 0;
        if (j >= 0 && j != i) {
            const QPointF d = j > i ? xy[j] - xy[i] : xy[i] - xy[j];
            a = std::atan2(d.y(), d.x());
        }
        if (a < 0) a += 2 * M_PI;
        out.append(InkPt{float(xy[i].x()), float(xy[i].y()), 4, width, quint8(a / (2 * M_PI) * 255), 110});
    }
    return out;
}

// Add strokes (page coordinates) with the current pen. Returns the number added, -1 if Line is not as expected.
// Converted text is drawn like a font: Fineliner (even width), black, thickness from the text size (em, page units).
int addInkStrokes(QObject *sc, QObject *ph, const QList<QList<QPointF>> &strokes, double em, int weight = 1) {
    Q_UNUSED(ph);
    const int add = sc->metaObject()->indexOfMethod("addDrawingLine(Line)");
    const QMetaType type = add >= 0 ? sc->metaObject()->method(add).parameterMetaType(0) : QMetaType();
    if (add < 0 || type.sizeOf() != 88) return -1;
    const int tool = 17;   // Finelinerv2
    const int color = 0;   // black
    const uint argb = 0xff000000u;
    static const double divisor[] = {32.0, 20.0, 13.0};   // thin, medium, thick
    const double thick = qBound(1.5, em / divisor[qBound(0, weight, 2)], 9.0);
    for (const auto &s : strokes) {
        void *line = type.create();
        auto *b = static_cast<char *>(line);
        *reinterpret_cast<int *>(b + 0x00) = tool;
        *reinterpret_cast<int *>(b + 0x04) = color;
        *reinterpret_cast<uint *>(b + 0x08) = argb;
        *reinterpret_cast<QList<InkPt> *>(b + 0x10) = inkPoints(s, quint16(std::lround(thick * 6)));
        *reinterpret_cast<double *>(b + 0x28) = thick;
        void *argv[] = {nullptr, line};
        sc->qt_metacall(QMetaObject::InvokeMetaMethod, add, argv);
        type.destroy(line);
    }
    return int(strokes.size());
}

// Bounds of the current selection (page coordinates); empty when nothing is selected. The item list is passed
// through opaquely: QList<std::shared_ptr<SceneItem>> has the layout of QList<std::shared_ptr<void>>.
QRectF selectionRect(QObject *sc, int layer) {
    const QMetaObject *mo = sc->metaObject();
    const int clone = mo->indexOfMethod("cloneSelectedItems(int,double)");
    const int bound = mo->indexOfMethod("getItemBoundingRect(QList<std::shared_ptr<SceneItem>>)");
    if (clone < 0 || bound < 0 || sc->property("selectionItemCount").toInt() <= 0) return {};
    QList<std::shared_ptr<void>> items;
    double scale = 1.0;   // 0 yields an empty rect
    void *a1[] = {&items, &layer, &scale};
    sc->qt_metacall(QMetaObject::InvokeMetaMethod, clone, a1);
    QRectF r;
    void *a2[] = {&r, &items};
    sc->qt_metacall(QMetaObject::InvokeMetaMethod, bound, a2);
    return r;
}

// ---- placing text ----
// One drawn line: its text and em box (top-left, size; page units). Highlights are placed on these.
struct LineBox {
    QString t;
    QPointF o;
    double em = 0;
};

struct Placed {
    QList<QList<QPointF>> strokes;
    QPointF origin;   // top-left of the first line's em box
    double em = 0;    // em height of the first line
    int weight = 1;   // 0 thin, 1 medium, 2 thick
    QList<LineBox> lines;   // every visual line, blank ones too
};

void appendLine(Placed *out, const QString &text, const QPointF &origin, double h) {
    out->lines.append(LineBox{text, origin, h});
    QList<QList<QPointF>> em;
    layoutLine(text, &em);
    for (const auto &s : em) {
        QList<QPointF> p;
        for (const QPointF &q : s) p.append(origin + q * h);
        out->strokes.append(p);
    }
}

// The sizes a converted object can have (em, scene units; 40 is about xochitl's body text). Handwriting is never the
// same size twice, so a size taken from it straight made every object a little different (issue #7).
double lineWidth(const QString &text);

const double kEmSteps[] = {28, 40, 52, 68, 88, 116};

double snapEm(double h) {
    double best = kEmSteps[0];
    for (double s : kEmSteps)
        if (std::abs(std::log(h / s)) < std::abs(std::log(h / best))) best = s;
    return best;
}

// Recognized lines ("x0 y0 x1 y1<TAB>text") as glyph strokes, either over the original lines (replace) or stacked
// below the selection. One size for the whole object: the size of its longest line's handwriting, snapped to
// kEmSteps.
Placed placeLines(const QStringList &lines, const QRectF &sel, bool replace) {
    Placed out;
    QString all;
    for (const QString &l : lines) all += l.section(QLatin1Char('	'), 1);
    useGlyphLanguage(all);
    struct In { QRectF b; QString text; };
    QList<In> ins;
    double h0 = 0;
    int longest = -1;
    for (const QString &l : lines) {
        const QStringList box = l.section(QLatin1Char('	'), 0, 0).split(QLatin1Char(' '));
        const QString text = l.section(QLatin1Char('	'), 1);
        if (box.size() != 4 || blankText(text)) continue;
        const QRectF b(QPointF(box[0].toDouble(), box[1].toDouble()), QPointF(box[2].toDouble(), box[3].toDouble()));
        const double w = lineWidth(text);
        if (w <= 0) continue;
        ins.append({b, text});
        // the handwriting's size: its ink height (Hangul ink is ~0.85 em), kept within +-15% of its width
        double h = b.height() / 0.85;
        if (w * h > b.width() * 1.15) h = b.width() * 1.15 / w;
        if (w * h < b.width() * 0.85) h = qMin(b.width() * 0.85 / w, b.height() / 0.6);
        const int n = int(text.trimmed().size());
        if (n > longest || (n == longest && h > h0)) { longest = n; h0 = h; }
    }
    if (ins.isEmpty()) return out;
    const double em = snapEm(h0);
    double yNext = replace ? -1e9 : sel.bottom() + 20;
    for (const In &in : ins) {
        // over the line it came from, but never into the previous line
        const double y = replace ? qMax(in.b.center().y() - 0.5 * em, yNext) : yNext;
        const QPointF origin(in.b.left(), y);
        if (out.em == 0) { out.origin = origin; out.em = em; }
        appendLine(&out, in.text, origin, em);
        yNext = y + em * 1.15;
    }
    return out;
}

double lineWidth(const QString &text) {
    QList<QList<QPointF>> tmp;
    return layoutLine(text, &tmp);
}

// Break one paragraph into lines no wider than maxEm (em units): at spaces, or inside a word that is too long.
QStringList wrapParagraph(const QString &para, double maxEm) {
    QStringList out;
    QString cur;
    for (const QString &word : para.split(QLatin1Char(' '))) {
        const QString tryLine = cur.isEmpty() ? word : cur + QLatin1Char(' ') + word;
        if (cur.isEmpty() || lineWidth(tryLine) <= maxEm) {
            cur = tryLine;
        } else {
            out << cur;
            cur = word;
        }
        while (cur.size() > 1 && lineWidth(cur) > maxEm) {   // a single word longer than the line
            int k = int(cur.size()) - 1;
            while (k > 1 && lineWidth(cur.left(k)) > maxEm) --k;
            out << cur.left(k);
            cur = cur.mid(k);
        }
    }
    if (!cur.isEmpty() || out.isEmpty()) out << cur;
    return out;
}

// Text (paragraphs separated by '\n') at one em size, wrapped at maxWidth (page units, 0 = no wrap), lines 1.15 em
// apart.
Placed placeText(const QString &text, const QPointF &origin, double em, double maxWidth) {
    useGlyphLanguage(text);
    Placed out;
    out.origin = origin;
    out.em = em;
    double y = origin.y();
    for (const QString &para : text.split(QLatin1Char('\n'))) {
        const QStringList lines = maxWidth > 0 ? wrapParagraph(para, maxWidth / em) : QStringList{para};
        for (const QString &line : lines) {
            if (!blankText(line)) appendLine(&out, line, QPointF(origin.x(), y), em);
            else out.lines.append(LineBox{line, QPointF(origin.x(), y), em});
            y += em * 1.15;
        }
    }
    return out;
}

// Select strokes with a rectangular lasso around them (SceneController::selectWithLine(Line), what the lasso tool
// calls). Selecting the new ink lets the user move it at once and also makes xochitl redraw that area.
void selectInk(QObject *sc, const QList<QList<QPointF>> &strokes) {
    const int sel = sc->metaObject()->indexOfMethod("selectWithLine(Line)");
    const QMetaType type = sel >= 0 ? sc->metaObject()->method(sel).parameterMetaType(0) : QMetaType();
    if (sel < 0 || type.sizeOf() != 88 || strokes.isEmpty()) return;
    QRectF r;
    for (const auto &s : strokes)
        for (const QPointF &p : s) r = r.isNull() ? QRectF(p, QSizeF(0.1, 0.1)) : r.united(QRectF(p, QSizeF(0.1, 0.1)));
    r.adjust(-15, -15, 15, 15);
    const QList<QPointF> loop{r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft(), r.topLeft()};
    void *line = type.create();
    auto *b = static_cast<char *>(line);
    *reinterpret_cast<int *>(b + 0x00) = 11;   // selection tool
    *reinterpret_cast<uint *>(b + 0x08) = 0xff000000u;
    *reinterpret_cast<QList<InkPt> *>(b + 0x10) = inkPoints(loop, 12);
    *reinterpret_cast<double *>(b + 0x28) = 2.0;
    void *argv[] = {nullptr, line};
    sc->qt_metacall(QMetaObject::InvokeMetaMethod, sel, argv);
    type.destroy(line);
}

// One full e-paper refresh (xochitl's own ghost buster, as rmpp-refresh does) after ms: the edit box and the
// keyboard leave ghosting behind when they close.
void fullRefreshLater(QPointer<QQuickItem> dvp, int ms) {
    QTimer::singleShot(ms, qApp, [dvp] {
        QObject *gb = dvp && dvp->isVisible() ? dvp->property("ghostBuster").value<QObject *>() : nullptr;
        if (!gb || !QMetaObject::invokeMethod(gb, "userRequestedClear")) TLOG("no ghost buster");
        else TLOG("full refresh");
    });
}

// Ends the selection the way a pen tap outside it does (DeviceSceneView.endItemSelection): a move or resize made
// to the selection is kept. clearSelectedItems drops a pending move (the ink jumped back, 2026-11-22).
void endSelection(QObject *sc, QQuickItem *dv) {
    QQuickItem *sv = dv ? findItem(dv, [](QQuickItem *i) { return classStarts(i, "DeviceSceneView_QMLTYPE") && i->isVisible(); }) : nullptr;
    if (!sv || !QMetaObject::invokeMethod(sv, "endItemSelection")) QMetaObject::invokeMethod(sc, "clearSelectedItems");
}

// Selection by rect (SceneController::addSelectionRect): mode 0 selects the strokes inside r (replacing the
// selection), mode 1 takes the strokes inside r out of it. Only while the selection tool is active.
bool selectRect(QObject *sc, const QRect &r, int mode = 0) {
    const int m = sc->metaObject()->indexOfMethod("addSelectionRect(QRect,SceneController::LineSelectionMode)");
    if (m < 0) return false;
    QRect rect = r;
    void *a[] = {nullptr, &rect, &mode};
    sc->qt_metacall(QMetaObject::InvokeMetaMethod, m, a);
    return true;
}

// Bounds of each stroke inside area (page units), or of each selected stroke (area null).
QList<QRect> lineRects(QObject *sc, const QRect *area = nullptr) {
    QList<QRect> out;
    if (!sc) return out;
    if (area) QMetaObject::invokeMethod(sc, "getLineBoundingRectsToBeSelected", Qt::DirectConnection,
                                        Q_RETURN_ARG(QList<QRect>, out), Q_ARG(QRect, *area));
    else QMetaObject::invokeMethod(sc, "getLineBoundingRectsToBeSelected", Qt::DirectConnection, Q_RETURN_ARG(QList<QRect>, out));
    return out;
}

// Every stroke of the open page. An area query leaves out the selected strokes (seen on the device, OS 3.29), so
// the selection's own rects are added.
QList<QRect> pageLineRects(QObject *sc) {
    const QRect all(-20000, -20000, 40000, 80000);
    QList<QRect> out = lineRects(sc, &all);
    // only with a selection: asking for the selection's rects while a page is still loading crashed xochitl twice
    // (SEGV right after "Lookup of layer idx failed")
    if (sc->property("selectionItemCount").toInt() <= 0) return out;
    const QList<QRect> sel = lineRects(sc);
    if (sel.isEmpty()) return out;
    QSet<QString> have;
    auto key = [](const QRect &r) { return QStringLiteral("%1,%2,%3,%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()); };
    for (const QRect &r : out) have.insert(key(r));
    for (const QRect &r : sel) if (!have.contains(key(r))) out << r;
    return out;
}
