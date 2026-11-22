// ---- highlights on text objects ----
// xochitl snaps a highlighter stroke on typed text to its characters (a glyph range: a band as high as the line).
// A text object is pen strokes to xochitl, so a highlighter stroke on it stays a freehand stroke over the ink.
// rmpp-textobj does the snapping for its objects:
// - a highlighter stroke drawn on a text object (the highlighter is the tool in use, the new stroke is thick and lies
//   on the object) covers characters: those of the line it overlaps most, at least half of their width inside it;
// - the record keeps the highlight: {"a", "b": characters [a, b) of the text (code points), "c": Line::Color,
//   "argb", "r": [x, y, w, h, ...] the bounds of the strokes that show it (record units, relative to the bounds)};
// - the freehand stroke is replaced by bands: one straight highlighter stroke per line, as high as the em box and as
//   wide as the characters, in the same colour (the selection tool is used for a moment to delete the freehand
//   stroke; the tool in use before is tapped again after);
// - editing the text draws the highlights again on the new text: a highlight follows its characters (text inserted
//   before it moves it, text changed inside it stays highlighted, its text deleted removes it);
// - a highlight whose strokes are all erased is dropped from the record.
// Highlights drawn with builds before this (freehand strokes on an object, not in the record) are taken up when the
// object is edited; their colour is not known then, so they come back yellow.
// tests/highlight_test.py is a 1:1 Python port of the geometry with its unit tests; keep them in step.
//
// Line for a highlighter stroke (rmpp-format NOTES, verified with highlightWithLine): tool 18 (Highlighterv2),
// colour Line::Color (3 yellow, 4 green, 5 pink, 14 blue, 15 orange, 16 gray; 9 = the ARGB code), points with
// width = px * 4, pressure 255. Drawing it with addDrawingLine (not on typed text) is to be checked on the tablet.

constexpr int kHighlighterTool = 18;   // Line::Tool Highlighterv2
constexpr int kOldHighlighterTool = 5; // Highlighter (v1)
constexpr int kHighlighterYellow = 3;  // Line::Color HighlighterYellow
constexpr double kBand = 0.95;         // band height, em

// A highlight: characters [a, b) of a text, colour, and the rects of the strokes that show it.
struct Hl {
    int a = 0, b = 0;
    int color = kHighlighterYellow;
    uint argb = 0;
    QList<QRectF> rects;   // page units (record units in a record's "r")
};

// (x0, x1) in em of each character of one line, as layoutLine places them.
QList<QPair<double, double>> charSpans(const QList<uint> &line) {
    QList<QPair<double, double>> out;
    double x = 0;
    for (const uint c : line) {
        const Glyph *g = glyphOf(char32_t(c));
        if (!g) { out.append({x, x + 0.5}); x += 0.5; continue; }
        out.append({x, x + g->adv});
        x += g->adv + 0.04;
    }
    return out;
}

int indexIn(const QList<uint> &hay, const QList<uint> &needle, int from) {
    if (needle.isEmpty()) return from;
    for (int i = from; i + needle.size() <= hay.size(); ++i) {
        bool eq = true;
        for (int j = 0; j < needle.size() && eq; ++j) eq = hay[i + j] == needle[j];
        if (eq) return i;
    }
    return -1;
}

// Offset (code points) of each visual line in text: wrapping drops a space or splits a word, so each line is found
// after the previous one.
QList<int> lineStarts(const QList<uint> &text, const QList<LineBox> &lines) {
    QList<int> out;
    int pos = 0;
    for (const LineBox &l : lines) {
        const QList<uint> t = l.t.toUcs4();
        int i = indexIn(text, t, pos);
        if (i < 0) i = pos;
        out.append(i);
        pos = i + int(t.size());
    }
    return out;
}

// Record r's line boxes on the page where placement p found it.
QList<LineBox> pageLines(const Rec &r, const Placement &p) {
    QList<LineBox> out;
    for (const LineBox &l : r.lines) out.append(LineBox{l.t, p.d + l.o * p.k, l.em * p.k});
    return out;
}

// The characters [a, b) of text that a highlighter stroke with bounds r (page units) covers, on an object with the
// line boxes lines: the line it overlaps most (at least 40% of its em box height), the characters with at least
// half their width inside it. False if none.
bool rangeFromRect(const QString &text, const QList<LineBox> &lines, const QRectF &r, int *a, int *b) {
    int best = -1;
    double bestOv = 0;
    for (int i = 0; i < lines.size(); ++i) {
        const double ov = qMin(r.bottom(), lines[i].o.y() + lines[i].em) - qMax(r.top(), lines[i].o.y());
        if (ov >= 0.4 * lines[i].em && ov > bestOv) { best = i; bestOv = ov; }
    }
    if (best < 0) return false;
    const LineBox &l = lines[best];
    const QList<uint> t = l.t.toUcs4();
    const QList<QPair<double, double>> sp = charSpans(t);
    int fa = -1, fb = -1;
    for (int j = 0; j < sp.size(); ++j) {
        if (QChar::isSpace(t[j])) continue;
        const double x0 = l.o.x() + sp[j].first * l.em, x1 = l.o.x() + sp[j].second * l.em;
        if (qMin(x1, r.right()) - qMax(x0, r.left()) >= 0.5 * (x1 - x0)) {
            if (fa < 0) fa = j;
            fb = j + 1;
        }
    }
    if (fa < 0) return false;
    const int s = lineStarts(text.toUcs4(), lines)[best];
    *a = s + fa;
    *b = s + fb;
    return true;
}

// The bands of highlight [a, b) of text on the line boxes: one per visual line it touches (leading and trailing
// spaces left out), each the rect of its ink: the characters' width, kBand em high around the middle of the em box.
QList<QRectF> bandRects(const QString &text, const QList<LineBox> &lines, int a, int b) {
    QList<QRectF> out;
    const QList<int> starts = lineStarts(text.toUcs4(), lines);
    for (int i = 0; i < lines.size(); ++i) {
        const LineBox &l = lines[i];
        const QList<uint> t = l.t.toUcs4();
        const int s = starts[i];
        int la = qMax(a, s) - s, lb = qMin(b, s + int(t.size())) - s;
        while (la < lb && QChar::isSpace(t[la])) ++la;
        while (lb > la && QChar::isSpace(t[lb - 1])) --lb;
        if (la >= lb) continue;
        const QList<QPair<double, double>> sp = charSpans(t);
        const double h = kBand * l.em;
        out.append(QRectF(l.o.x() + sp[la].first * l.em, l.o.y() + 0.5 * l.em - h / 2, (sp[lb - 1].second - sp[la].first) * l.em, h));
    }
    return out;
}

// Highlight [a, b) of the old text moved onto the new text: the common start and end of the two keep their
// highlight; the changed middle part is highlighted if the highlight reached into it. False when nothing is left.
bool mapRange(const QList<uint> &old, const QList<uint> &nw, int *a, int *b) {
    int p = 0;
    while (p < old.size() && p < nw.size() && old[p] == nw[p]) ++p;
    int s = 0;
    while (s < old.size() - p && s < nw.size() - p && old[old.size() - 1 - s] == nw[nw.size() - 1 - s]) ++s;
    const int oe = int(old.size()) - s, ne = int(nw.size()) - s;   // changed: old[p, oe) -> new[p, ne)
    auto pos = [&](int x, bool end) { return x <= p ? x : x >= oe ? x - oe + ne : (end ? ne : p); };
    int na = pos(*a, false), nb = pos(*b, true);
    while (na < nb && QChar::isSpace(nw[na])) ++na;
    while (nb > na && QChar::isSpace(nw[nb - 1])) --nb;
    if (na >= nb) return false;
    *a = na;
    *b = nb;
    return true;
}

// ---- records ----
// The highlights of a record; rects placed on the page with placement p (null: record units as stored).
QList<Hl> hlOf(const Rec &r, const Placement *p = nullptr) {
    QList<Hl> out;
    for (const QJsonValue &v : r.hl) {
        const QJsonObject o = v.toObject();
        Hl h;
        h.a = o.value(QStringLiteral("a")).toInt();
        h.b = o.value(QStringLiteral("b")).toInt();
        h.color = o.value(QStringLiteral("c")).toInt(kHighlighterYellow);
        h.argb = uint(o.value(QStringLiteral("argb")).toDouble());
        const QJsonArray rr = o.value(QStringLiteral("r")).toArray();
        for (int i = 0; i + 3 < rr.size(); i += 4) {
            const QRectF q(rr[i].toDouble(), rr[i + 1].toDouble(), rr[i + 2].toDouble(), rr[i + 3].toDouble());
            h.rects.append(p ? QRectF(p->d + q.topLeft() * p->k, q.size() * p->k) : q);
        }
        if (h.b > h.a) out.append(h);
    }
    return out;
}

// JSON of highlights (rects as they are: page units for makeRecord, record units for setRecordHl).
QJsonArray hlJson(const QList<Hl> &hls) {
    QJsonArray out;
    for (const Hl &h : hls) {
        QJsonArray rr;
        for (const QRectF &q : h.rects) rr << q.x() << q.y() << q.width() << q.height();
        out.append(QJsonObject{{QStringLiteral("a"), h.a}, {QStringLiteral("b"), h.b}, {QStringLiteral("c"), h.color},
                               {QStringLiteral("argb"), double(h.argb)}, {QStringLiteral("r"), rr}});
    }
    return out;
}

// Page rect q against a highlight stroke rect e (page units): the same place and width. Its height: the highlighter
// width does not scale with a resized object (as the pen width), so anything between e's and the unscaled height.
bool sameBand(const QRect &q, const QRectF &e, double k) {
    const QPointF c = centre(q), ce = e.center();
    const double tol = qMax(kPosTol + 1, 0.12 * e.height());
    if (qMax(qAbs(c.x() - ce.x()), qAbs(c.y() - ce.y())) > tol) return false;
    if (qAbs(q.width() - e.width()) > qMax(4.0, 0.12 * e.width()) + e.height() * qAbs(1 - 1 / k)) return false;
    const double h0 = qMin(e.height(), e.height() / k), h1 = qMax(e.height(), e.height() / k);
    return q.height() >= 0.7 * h0 - 3 && q.height() <= 1.3 * h1 + 3;
}

// Whether page rect i shows a highlight of the object at p (a stroke recorded for one of its highlights).
bool isBand(const PageObjects &po, const Placement &p, int i) {
    for (const Hl &h : hlOf(po.recs[p.rec], &p))
        for (const QRectF &e : h.rects)
            if (sameBand(po.rects[i], e, p.k)) return true;
    return false;
}

// Whether page rect i is a highlighter-like stroke on the object at p: not one of its strokes, as thick as a
// highlighter (at least 0.35 em high), and mostly (70%) on the object.
bool isHighlightStroke(const PageObjects &po, const Placement &p, int i) {
    if (p.members.contains(i)) return false;
    const QRect &r = po.rects[i];
    const double em = po.recs[p.rec].em * p.k;
    if (r.height() < 0.35 * em || r.width() < 0.4 * em || r.height() > 2.5 * em) return false;
    const QRectF on = QRectF(r).intersected(p.box.adjusted(-0.25 * em, -0.25 * em, 0.25 * em, 0.25 * em));
    return on.width() * on.height() >= 0.7 * r.width() * r.height();
}

// ---- drawing bands ----
// Each band rect as one straight highlighter stroke through its middle, as thick as the band, from half the thickness
// inside each end (its ink then fills the rect). Returns the number drawn, -1 if Line is not as expected.
int addBands(QObject *sc, const QList<Hl> &hls) {
    const int add = sc->metaObject()->indexOfMethod("addDrawingLine(Line)");
    const QMetaType type = add >= 0 ? sc->metaObject()->method(add).parameterMetaType(0) : QMetaType();
    if (add < 0 || type.sizeOf() != 88) return -1;
    int n = 0;
    for (const Hl &h : hls)
        for (const QRectF &r : h.rects) {
            const double w = r.height();
            const double y = r.center().y();
            double x0 = r.left() + w / 2, x1 = r.right() - w / 2;
            if (x1 < x0) x0 = x1 = r.center().x();
            QList<InkPt> pts;
            const int np = 12;
            for (int i = 0; i < np; ++i) {
                const double t = double(i) / (np - 1);
                pts.append(InkPt{float(x0 + (x1 - x0) * t), float(y), 0, quint16(qBound(4.0, w * 4, 65000.0)), 0, 255});
            }
            void *line = type.create();
            auto *b = static_cast<char *>(line);
            *reinterpret_cast<int *>(b + 0x00) = kHighlighterTool;
            *reinterpret_cast<int *>(b + 0x04) = h.color;
            *reinterpret_cast<uint *>(b + 0x08) = h.argb;
            *reinterpret_cast<QList<InkPt> *>(b + 0x10) = pts;
            void *argv[] = {nullptr, line};
            sc->qt_metacall(QMetaObject::InvokeMetaMethod, add, argv);
            type.destroy(line);
            ++n;
        }
    return n;
}

int bandCount(const QList<Hl> &hls) {
    int n = 0;
    for (const Hl &h : hls) n += int(h.rects.size());
    return n;
}

// The highlights of an edited text moved onto the new text and placed on its lines.
QList<Hl> placeHighlights(const QList<Hl> &hls, const QString &oldText, const QString &text, const QList<LineBox> &lines) {
    QList<Hl> out;
    const QList<uint> o = oldText.toUcs4(), n = text.toUcs4();
    for (Hl h : hls) {
        if (!mapRange(o, n, &h.a, &h.b)) continue;
        h.rects = bandRects(text, lines, h.a, h.b);
        if (!h.rects.isEmpty()) out.append(h);
    }
    return out;
}

// Splits the rects of new ink into the glyph strokes and the bands of hls (expected band rects: hls' rects, page
// units): each band takes the closest rect of its size. hls' rects become the found ones (unfound bands: none).
QList<QRect> splitBands(const QList<QRect> &rects, QList<Hl> *hls) {
    QList<QRect> glyphs = rects;
    for (Hl &h : *hls) {
        QList<QRectF> found;
        for (const QRectF &e : h.rects) {
            int best = -1;
            double bestErr = 0;
            for (int i = 0; i < glyphs.size(); ++i) {
                if (!sameBand(glyphs[i], e, 1.0)) continue;
                const double err = (centre(glyphs[i]) - e.center()).manhattanLength();
                if (best < 0 || err < bestErr) { best = i; bestErr = err; }
            }
            if (best >= 0) found.append(QRectF(glyphs.takeAt(best)));
        }
        h.rects = found;
    }
    return glyphs;
}

// ---- tool in use ----
// The tool button that is selected now (the tool to come back to after using the selection tool for a moment).
QQuickItem *activeToolButton() {
    QQuickItem *sel = selectionToolButton();
    if (!sel) return nullptr;
    QQuickItem *up = sel->parentItem();
    for (int depth = 0; up && depth < 4; ++depth, up = up->parentItem()) {
        QQuickItem *f = findItem(up, [sel](QQuickItem *i) {
            // tool buttons are "...Button" (selectionButton) or "...Menu" (primaryPenMenu, secondaryPenMenu: the pens)
            const QString n = i->objectName();
            return i != sel && i->isVisible() && (n.endsWith(QLatin1String("Button")) || n.endsWith(QLatin1String("Menu")))
                   && i->property("selected").toBool();
        });
        if (f) return f;
    }
    return nullptr;
}

// The pen in use: whether it is a highlighter, and its colour.
bool highlighterInUse(QObject *ph, int *color, uint *argb) {
    if (!ph) return false;
    const int tool = ph->property("lineTool").toInt();
    if (tool != kHighlighterTool && tool != kOldHighlighterTool) return false;
    *color = ph->property("lineColor").toInt();
    *argb = ph->property("lineArgbCode").toUInt();
    return true;
}
