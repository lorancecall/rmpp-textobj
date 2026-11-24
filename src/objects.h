// ---- text object records and finding the objects on a page ----
// One record per object: {"id", "page", "text", "n" (strokes), "em", "weight", "w", "h" (bounds), "ox", "oy" (em
// origin of the first line relative to the bounds' top-left), "rects": [x, y, w, h, ...] (bounds of each stroke
// relative to the bounds' top-left, as xochitl reports them), "lines": [[text, x, y, em], ...] (each drawn line's em
// box, relative to the bounds' top-left), "hl": highlights (src/highlight.h)}. The bounds are the union of the stroke
// rects. Records without "lines" (earlier builds) have their text's lines 1.15 em apart from (ox, oy).
//
// Records are kept in memory only: made here (a conversion, an edit) or read from the strokes of a page file when
// the page is looked at (src/carry.h: each object carries its record in its strokes). There is no record file.
//
// An object is found on a page by its stroke rects: wherever it was moved (and whatever size it was scaled to), its
// strokes keep their pattern, so the page's stroke rects (SceneController::getLineBoundingRectsToBeSelected) are
// searched for it. No position is stored; copies of an object are found as well (each is the same text object).
// Records without "rects" ("legacy", rmpp-hwr 0.2-0.4) are no longer read from anywhere; the code that gave them
// their rects stays for records made by an older add-on through the API.

QString newRecordId() {
    static int n = 0;
    return QString::number(QDateTime::currentMSecsSinceEpoch(), 36) + QLatin1Char('-') + QString::number(++n);
}

// Every record known in this session (made here or read from page files), oldest first.
QJsonArray g_records;

void writeRecords(const QJsonArray &a) { g_records = a; }

// A record that is no object: blank text (nothing to draw: there are no strokes for it) or no strokes.
bool emptyRecord(const QJsonObject &o) {
    return blankText(o.value(QStringLiteral("text")).toString()) || o.value(QStringLiteral("n")).toInt() <= 0;
}

QJsonArray readRecords() { return g_records; }

struct Rec {
    QString id, page, text;
    int n = 0, weight = 1;
    double em = 60, w = 0, h = 0, ox = 0, oy = 0;
    QList<QRect> rects;   // relative to the bounds' top-left; empty for legacy records
    QList<LineBox> lines; // relative to the bounds' top-left
    QJsonArray hl;        // highlights (src/highlight.h)
};

Rec recFrom(const QJsonObject &o) {
    Rec r;
    r.id = o.value(QStringLiteral("id")).toString();
    r.page = o.value(QStringLiteral("page")).toString();
    r.text = o.value(QStringLiteral("text")).toString();
    r.n = o.value(QStringLiteral("n")).toInt();
    r.weight = o.value(QStringLiteral("weight")).toInt(1);
    r.em = o.value(QStringLiteral("em")).toDouble(60);
    r.w = o.value(QStringLiteral("w")).toDouble();
    r.h = o.value(QStringLiteral("h")).toDouble();
    r.ox = o.value(QStringLiteral("ox")).toDouble();
    r.oy = o.value(QStringLiteral("oy")).toDouble();
    const QJsonArray a = o.value(QStringLiteral("rects")).toArray();
    for (int i = 0; i + 3 < a.size(); i += 4) r.rects.append(QRect(a[i].toInt(), a[i + 1].toInt(), a[i + 2].toInt(), a[i + 3].toInt()));
    if (r.rects.size() != r.n) r.rects.clear();
    for (const QJsonValue &v : o.value(QStringLiteral("lines")).toArray()) {
        const QJsonArray l = v.toArray();
        if (l.size() == 4) r.lines.append(LineBox{l[0].toString(), QPointF(l[1].toDouble(), l[2].toDouble()), l[3].toDouble()});
    }
    if (r.lines.isEmpty()) {
        const QStringList ts = r.text.split(QLatin1Char('\n'));
        for (int i = 0; i < ts.size(); ++i) r.lines.append(LineBox{ts[i], QPointF(r.ox, r.oy + i * 1.15 * r.em), r.em});
    }
    r.hl = o.value(QStringLiteral("hl")).toArray();
    return r;
}

QList<Rec> readRecs() {
    QList<Rec> out;
    for (const QJsonValue &v : readRecords()) out.append(recFrom(v.toObject()));
    return out;
}

QRect unite(const QList<QRect> &rs) {
    QRect b;
    for (const QRect &r : rs) b = b.isNull() ? r : b.united(r);
    return b;
}

// A record for strokes now on the page: their rects (page units), the text, the em origin (page units) and size,
// the drawn lines (page units; none: from the text) and highlights (their "r" in page units here).
QJsonObject makeRecord(const QString &page, const QString &text, const QList<QRect> &rects, const QPointF &origin,
                       double em, int weight, const QString &id = QString(), const QList<LineBox> &lines = {},
                       const QJsonArray &hl = {}) {
    const QRect b = unite(rects);
    QJsonArray flat;
    for (const QRect &r : rects) { flat.append(r.x() - b.x()); flat.append(r.y() - b.y()); flat.append(r.width()); flat.append(r.height()); }
    QJsonObject o{{QStringLiteral("id"), id.isEmpty() ? newRecordId() : id}, {QStringLiteral("page"), page},
                  {QStringLiteral("text"), text}, {QStringLiteral("n"), int(rects.size())},
                  {QStringLiteral("em"), em}, {QStringLiteral("weight"), weight},
                  {QStringLiteral("w"), b.width()}, {QStringLiteral("h"), b.height()},
                  {QStringLiteral("ox"), origin.x() - b.x()}, {QStringLiteral("oy"), origin.y() - b.y()},
                  {QStringLiteral("rects"), flat}};
    if (!lines.isEmpty()) {
        QJsonArray ls;
        for (const LineBox &l : lines) ls.append(QJsonArray{l.t, l.o.x() - b.x(), l.o.y() - b.y(), l.em});
        o.insert(QStringLiteral("lines"), ls);
    }
    if (!hl.isEmpty()) {
        QJsonArray out;
        for (const QJsonValue &v : hl) {
            QJsonObject h = v.toObject();
            const QJsonArray r = h.value(QStringLiteral("r")).toArray();
            QJsonArray rel;
            for (int i = 0; i + 3 < r.size(); i += 4)
                rel << r[i].toDouble() - b.x() << r[i + 1].toDouble() - b.y() << r[i + 2] << r[i + 3];
            h.insert(QStringLiteral("r"), rel);
            out.append(h);
        }
        o.insert(QStringLiteral("hl"), out);
    }
    return o;
}

// Replaces the highlights of record id (hl with "r" relative to the record's bounds, record units).
void setRecordHl(const QString &id, const QJsonArray &hl) {
    QJsonArray recs = readRecords();
    for (int i = 0; i < recs.size(); ++i) {
        QJsonObject o = recs[i].toObject();
        if (o.value(QStringLiteral("id")).toString() != id) continue;
        if (hl.isEmpty()) o.remove(QStringLiteral("hl"));
        else o.insert(QStringLiteral("hl"), hl);
        recs[i] = o;
        writeRecords(recs);
        return;
    }
}

// Adds `add` (if not empty; it replaces a record with its id). `remove` is not acted on: an object is found by its
// strokes, so a record whose strokes are gone finds nothing, and one whose strokes come back (an undo) works again.
void updateRecords(const QJsonObject &add, const QStringList &remove) {
    Q_UNUSED(remove);
    if (add.isEmpty()) return;
    const QString id = add.value(QStringLiteral("id")).toString(), page = add.value(QStringLiteral("page")).toString();
    QJsonArray out;
    for (const QJsonValue &v : g_records) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("id")).toString() != id || o.value(QStringLiteral("page")).toString() != page) out.append(v);
    }
    out.append(add);
    g_records = out;
}

// The rect xochitl reports for a stroke: its points grown by 1.5 times the pen width (the point width / 4), each edge
// rounded (device, OS 3.29.1: Fineliner at width 2.25, a glyph stroke 92.81,238.04 13.75x20.92 -> 89,235 21x27, a dot
// -> 7x7; within a unit for every stroke checked).
QRect strokeRect(const QList<QPointF> &pts, double widthPx) {
    const QRectF f = pointsFrame({pts});
    const double h = 1.5 * widthPx;
    const int l = qRound(f.left() - h), t = qRound(f.top() - h), r = qRound(f.right() + h), b = qRound(f.bottom() + h);
    return QRect(l, t, r - l, b - t);
}

// The objects whose records ride in a page file's strokes, as records of this page. One record per object: its
// copies (same id) are found by their strokes like the original.
QList<QJsonObject> recordsFromStrokes(const QString &page, const QList<FileStroke> &all) {
    struct Part { int idx; const FileStroke *s; QPointF c; };
    QHash<quint32, QList<Part>> by;
    for (const FileStroke &s : all) {
        if (s.pts.size() < 2 || (s.pts[0].speed & 0xf000) != kCarryTag) continue;
        QRectF f;
        for (int i = 0; i < s.pts.size(); ++i) {
            const QPointF p(s.pts[i].x, s.pts[i].y);
            f = i ? f.united(QRectF(p, QSizeF(0, 0))) : QRectF(p, QSizeF(0, 0));
        }
        by[s.pts.last().speed].append(Part{s.pts[0].speed & 0x0fff, &s, f.center()});
    }
    QList<QJsonObject> out;
    int partial = 0, bad = 0, turned = 0;
    for (auto it = by.cbegin(); it != by.cend(); ++it) {
        const QList<Part> &parts = it.value();
        int maxIdx = 0;
        for (const Part &p : parts) maxIdx = qMax(maxIdx, p.idx);
        bool done = false, complete = false, read = false;
        Carried c;
        for (const Part &anchor : parts) {   // each copy's first stroke: the strokes nearest to it make that copy
            if (done || anchor.idx != 0) continue;
            QList<const Part *> pick(maxIdx + 1, nullptr);
            for (const Part &p : parts) {
                const Part *&q = pick[p.idx];
                const QPointF d = p.c - anchor.c, e = q ? q->c - anchor.c : QPointF();
                if (!q || d.x() * d.x() + d.y() * d.y() < e.x() * e.x() + e.y() * e.y()) q = &p;
            }
            pick[0] = &anchor;
            const auto &a0 = anchor.s->pts;
            const int count = a0.size() > 2 ? a0[1].speed : 0;
            if (count <= 0 || count > pick.size()) continue;
            pick.resize(count);
            if (pick.contains(nullptr)) continue;
            complete = true;
            QList<QList<QPointF>> strokes;
            QList<QRect> rects;
            QList<quint16> stream;
            for (const Part *p : pick) {
                const auto &ps = p->s->pts;
                for (int k = 1; k + 1 < ps.size(); ++k) stream << ps[k].speed;
                QList<QPointF> pts;
                double w = 0;
                for (const InkPt &q : ps) { pts << QPointF(q.x, q.y); w = qMax(w, q.width / 4.0); }
                strokes << pts;
                rects << strokeRect(pts, w);
            }
            if (!read) {   // every copy carries the same words
                QByteArray bytes;
                const int len = stream.size() > 1 ? stream[1] : -1;
                for (int i = 2; i < stream.size() && bytes.size() < len; ++i) {
                    bytes.append(char(stream[i] & 0xff));
                    if (bytes.size() < len) bytes.append(char(stream[i] >> 8));
                }
                if (len < 0 || bytes.size() != len || !unpackRecord(bytes, &c)) break;
                read = true;
            }
            const QRectF f = pointsFrame(strokes);
            // the same shape as drawn (a mix of two copies, or a rotated copy, is not)
            if (c.fw > 1 && c.fh > 1 && f.height() > 0
                && qAbs((f.width() / f.height()) / (c.fw / c.fh) - 1) > 0.05) { ++turned; continue; }
            const double k = c.fw > 1 ? f.width() / c.fw : (c.fh > 1 ? f.height() / c.fh : 1.0);
            QList<LineBox> lines;
            for (const LineBox &l : c.lines) lines << LineBox{l.t, f.topLeft() + l.o * k, l.em * k};
            out << makeRecord(page, c.text, rects, f.topLeft() + QPointF(c.ox, c.oy) * k, c.em * k, c.weight,
                              carryKey(it.key()), lines);
            done = true;
        }
        if (!complete) ++partial;
        else if (!read) ++bad;
    }
    if (turned) TLOG("page file: %d copy(ies) not in their drawn shape (rotated?): not read", turned);
    if (partial || bad) TLOG("page file: %d object(s) with strokes missing, %d unreadable: plain strokes", partial, bad);
    return out;
}

// The page's .rm file (xochitl/<notebook>/<page>.rm), or empty for a page not saved yet.
QString pageFilePath(const QString &page) {
    static QHash<QString, QString> cache;
    static QHash<QString, qint64> missed;   // not found: looked for again after 5 s
    const auto it = cache.constFind(page);
    if (it != cache.constEnd() && QFileInfo::exists(*it)) return *it;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - missed.value(page, -100000) < 5000) return {};
    const QString root = QStringLiteral("/home/root/.local/share/remarkable/xochitl");
    for (const QString &d : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString p = root + QLatin1Char('/') + d + QLatin1Char('/') + page + QStringLiteral(".rm");
        if (QFileInfo::exists(p)) { cache.insert(page, p); missed.remove(page); return p; }
    }
    missed.insert(page, now);
    return {};
}

// Reads the records carried by the page file's strokes when the file changed since the last read. Records made in
// this session stay as they are (theirs are the rects xochitl reported).
void loadPageRecords(const QString &page) {
    static QHash<QString, qint64> seen;
    if (page.isEmpty()) return;
    const QString path = pageFilePath(page);
    if (path.isEmpty()) return;
    const qint64 m = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    if (seen.value(page, -1) == m) return;
    QElapsedTimer t;
    t.start();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    QList<FileStroke> strokes;
    if (!readPageStrokes(f.readAll(), &strokes)) { TLOG("page file: not a v6 page"); return; }
    seen.insert(page, m);
    // a record per object and page: a copy on another page has the same id, and each page needs its own (only a
    // page's own records are found at another size)
    auto key = [](const QJsonObject &o) {
        return o.value(QStringLiteral("id")).toString() + QLatin1Char('@') + o.value(QStringLiteral("page")).toString(); };
    QSet<QString> have;
    for (const QJsonValue &v : g_records) have.insert(key(v.toObject()));
    int added = 0;
    for (const QJsonObject &o : recordsFromStrokes(page, strokes))
        if (!have.contains(key(o))) { g_records.append(o); ++added; }
    TLOG("page file: %lld stroke(s), %d object record(s) read, %lld ms", (long long)strokes.size(), added, (long long)t.elapsed());
}

// ---- finding objects among stroke rects ----
// An object at a place on the page: page rect indices of its strokes (in record order), scale k, offset d
// (page = d + k * record, for centres and the bounds' top-left), and its bounds.
struct Placement {
    int rec = -1;            // index into the Rec list it was searched for
    double k = 1;
    QPointF d;
    QList<int> members;      // page rect index per record stroke (-1: not found)
    int matched = 0;
    QRectF box;
};

// Matching (tests/match_test.py is a 1:1 Python port with its unit tests; keep them in step):
// - strokes are compared by the centre and size of their rects: a moved stroke's whole-unit rect rounds differently,
//   by up to a unit at each edge, and the pen width does not scale with scaled strokes;
// - an anchor (one of the record's three biggest strokes) on a page stroke of the right size gives scale k and
//   offset d; one (all, when no partial match is wanted) of the other anchors must be there as well;
// - each page stroke goes to one record stroke, the closest pairs first (traced glyphs leave clusters of tiny
//   fragments that trade places when rounded);
// - scaled: k and d are fitted to the matched centres (least squares, twice, dropping strokes off the fit), the
//   pen-width size offset is taken from the matched sizes, and the strokes are matched again strictly.
constexpr double kPosTol = 2.5;   // page units

QPointF centre(const QRect &r) { return QPointF(r.x() + r.width() / 2.0, r.y() + r.height() / 2.0); }

// Stroke rects indexed by a coarse grid of their centres.
struct RectIndex {
    const QList<QRect> *rects = nullptr;
    QHash<qint64, QList<int>> grid;
    static qint64 key(int gx, int gy) { return (qint64(gx) << 32) ^ quint32(gy); }
    explicit RectIndex(const QList<QRect> &rs) : rects(&rs) {
        for (int i = 0; i < rs.size(); ++i) {
            const QPointF c = centre(rs[i]);
            grid[key(int(std::floor(c.x() / 16)), int(std::floor(c.y() / 16)))].append(i);
        }
    }
    // (error, index) of the rects with the centre within tc of c and the size within ts of size.
    void candidates(const QPointF &c, const QSizeF &size, double tc, double ts, QList<QPair<double, int>> *out) const {
        const int gx0 = int(std::floor((c.x() - tc) / 16)), gx1 = int(std::floor((c.x() + tc) / 16));
        const int gy0 = int(std::floor((c.y() - tc) / 16)), gy1 = int(std::floor((c.y() + tc) / 16));
        for (int gx = gx0; gx <= gx1; ++gx)
            for (int gy = gy0; gy <= gy1; ++gy) {
                auto it = grid.constFind(key(gx, gy));
                if (it == grid.constEnd()) continue;
                for (int i : *it) {
                    const QRect &r = (*rects)[i];
                    const QPointF rc = centre(r);
                    const double err = qMax(qAbs(rc.x() - c.x()), qAbs(rc.y() - c.y()));
                    const double serr = qMax(qAbs(r.width() - size.width()), qAbs(r.height() - size.height()));
                    if (err <= tc && serr <= ts) out->append({err + 0.5 * serr, i});
                }
            }
    }
    bool any(const QPointF &c, const QSizeF &size, double tc, double ts) const {
        QList<QPair<double, int>> out;
        candidates(c, size, tc, ts, &out);
        return !out.isEmpty();
    }
};

// Whether a record may be found at another size: only on its own page and with 4+ strokes. A record of one to three
// strokes found at any size would turn ordinary handwriting (a dash, a dot) into a text object.
bool scaleAllowed(const Rec &r, const QString &pageId) { return r.page == pageId && r.n >= 4; }

// The record's strokes placed at scale k, offset d (page centre = d + k * record centre); sizes compared as
// k * size + sizeOff. members[i] = page rect of record stroke i or -1. Returns the number matched.
int matchAt(const Rec &r, const RectIndex &idx, double k, const QPointF &d, double sizeOff, const QPointF &ca, bool loose,
            QList<int> *members) {
    struct Pair { double err; int ri, pi; };
    QList<Pair> pairs;
    QList<QPair<double, int>> cand;
    for (int ri = 0; ri < r.rects.size(); ++ri) {
        const QRect &rr = r.rects[ri];
        const QPointF cr = centre(rr);
        const double big = qMax(rr.width(), rr.height());
        const bool tiny = big <= 5;   // tracing leaves clusters of tiny fragments that trade places when rounded
        const double dist = std::hypot(cr.x() - ca.x(), cr.y() - ca.y());
        const double tc = kPosTol + (tiny ? 2.0 : 0.0) + (loose ? 0.04 * k * dist : 0.004 * k * big);
        const double ts = kPosTol + 0.06 * k * big + (loose ? 4.0 : 0.0);
        cand.clear();
        idx.candidates(d + cr * k, QSizeF(k * rr.width() + sizeOff, k * rr.height() + sizeOff), tc, ts, &cand);
        for (const auto &c : cand) pairs.append({c.first, ri, c.second});
    }
    std::sort(pairs.begin(), pairs.end(), [](const Pair &a, const Pair &b) {
        return a.err != b.err ? a.err < b.err : (a.ri != b.ri ? a.ri < b.ri : a.pi < b.pi); });
    members->fill(-1, r.rects.size());
    QSet<int> taken;
    for (const Pair &p : pairs)
        if ((*members)[p.ri] < 0 && !taken.contains(p.pi)) { (*members)[p.ri] = p.pi; taken.insert(p.pi); }
    return int(taken.size());
}

// k and d with page centre = d + k * record centre, least squares over the matched strokes.
bool fitPlacement(const Rec &r, const QList<QRect> &page, const QList<int> &members, double *k, QPointF *d) {
    QList<QPair<QPointF, QPointF>> pr;
    for (int i = 0; i < members.size(); ++i) if (members[i] >= 0) pr.append({centre(r.rects[i]), centre(page[members[i]])});
    if (pr.size() < 2) return false;
    QPointF mr, mp;
    for (const auto &a : pr) { mr += a.first; mp += a.second; }
    mr /= pr.size(); mp /= pr.size();
    double num = 0, den = 0;
    for (const auto &a : pr) {
        num += (a.first.x() - mr.x()) * (a.second.x() - mp.x()) + (a.first.y() - mr.y()) * (a.second.y() - mp.y());
        den += (a.first.x() - mr.x()) * (a.first.x() - mr.x()) + (a.first.y() - mr.y()) * (a.first.y() - mr.y());
    }
    if (den <= 0) return false;
    *k = num / den;
    *d = mp - mr * *k;
    return true;
}

// Every place where record r's stroke pattern is found among the page rects (full matches), and the best partial
// match (most strokes found) when there is no full one (partial non-null).
void findRec(const Rec &r, int recIdx, const QList<QRect> &page, const RectIndex &idx, QList<Placement> *full, Placement *partial,
             bool allowScale) {
    if (r.rects.isEmpty()) return;
    QList<int> anchors;   // the three biggest strokes
    for (int i = 0; i < r.rects.size(); ++i) anchors << i;
    std::stable_sort(anchors.begin(), anchors.end(), [&](int a, int b) {
        return r.rects[a].width() * r.rects[a].height() > r.rects[b].width() * r.rects[b].height(); });
    if (anchors.size() > 3) anchors.resize(3);
    QList<int> members;
    for (int a : anchors) {
        const QRect &ar = r.rects[a];
        const QPointF ca = centre(ar);
        for (const QRect &o : page) {
            double k = 1;
            if (allowScale) {
                k = double(o.width() + o.height()) / qMax(1, ar.width() + ar.height());
                if (k < 0.3 || k > 4) continue;
                if (qMin(qMin(ar.width(), ar.height()), qMin(o.width(), o.height())) > 6
                    && qAbs((double(o.width()) / o.height()) / (double(ar.width()) / ar.height()) - 1) > 0.3)
                    continue;
            } else if (qAbs(o.width() - ar.width()) > kPosTol || qAbs(o.height() - ar.height()) > kPosTol) {
                continue;
            }
            QPointF d = centre(o) - ca * k;
            bool dup = false;   // the same place found from another anchor
            for (const Placement &p : *full) if (p.rec == recIdx && (p.d - d).manhattanLength() < 6) dup = true;
            if (dup) continue;
            const bool scaled = qAbs(k - 1) > 0.02;
            // quick check before matching every stroke: one (all, without partial) of the other anchors is there
            int others = 0, present = 0;
            for (int b : anchors) {
                if (b == a) continue;
                ++others;
                const QRect &br = r.rects[b];
                const QPointF cb = centre(br);
                const double dist = std::hypot(cb.x() - ca.x(), cb.y() - ca.y());
                if (idx.any(d + cb * k, QSizeF(k * br.width(), k * br.height()), kPosTol + (scaled ? 0.04 * k * dist : 0.0),
                            kPosTol + 0.06 * k * qMax(br.width(), br.height()) + (scaled ? 4.0 : 0.0)))
                    ++present;
            }
            if (others > 0 && (partial ? present == 0 : present < others)) continue;
            int matched = matchAt(r, idx, k, d, 0, ca, scaled, &members);
            if (scaled && matched * 2 >= r.n) {
                for (int pass = 0; pass < 2; ++pass) {
                    if (!fitPlacement(r, page, members, &k, &d)) break;
                    for (int i = 0; i < members.size(); ++i) {
                        if (members[i] < 0) continue;
                        const QPointF e = d + centre(r.rects[i]) * k, c = centre(page[members[i]]);
                        if (qMax(qAbs(e.x() - c.x()), qAbs(e.y() - c.y())) > kPosTol + 1) members[i] = -1;
                    }
                }
                QList<double> offs;
                for (int i = 0; i < members.size(); ++i)
                    if (members[i] >= 0) offs << page[members[i]].width() - k * r.rects[i].width();
                if (!offs.isEmpty()) {
                    std::sort(offs.begin(), offs.end());
                    matched = matchAt(r, idx, k, d, offs[offs.size() / 2], ca, false, &members);
                }
            }
            Placement p;
            p.rec = recIdx; p.k = k; p.d = d; p.members = members; p.matched = matched;
            // the record's bounds: the union of its rects, top-left at (0, 0) in record units
            p.box = QRectF(d, QSizeF(r.w, r.h) * k);
            if (matched == r.n) full->append(p);
            else if (partial && matched > partial->matched) *partial = p;
        }
    }
}

QPointF originOf(const Rec &r, const Placement &p) { return p.d + QPointF(r.ox, r.oy) * p.k; }

// The objects on the open page: full placements of its records (also records made on other pages when
// otherPages: pasted copies), with the records they belong to.
struct PageObjects {
    QList<Rec> recs;
    QList<QRect> rects;         // every stroke of the page
    QList<Placement> found;     // full matches
};

PageObjects findObjects(QObject *sc, const QString &pageId, bool otherPages) {
    PageObjects po;
    po.rects = pageLineRects(sc);
    if (po.rects.isEmpty()) return po;
    loadPageRecords(pageId);
    po.recs = readRecs();
    const RectIndex idx(po.rects);
    for (int i = 0; i < po.recs.size(); ++i) {
        const Rec &r = po.recs[i];
        if (r.rects.isEmpty() || r.n > po.rects.size()) continue;
        // records of other pages: pasted copies (same size), never tiny ones
        if (r.page != pageId && (!otherPages || r.n < 3)) continue;
        findRec(r, i, po.rects, idx, &po.found, nullptr, scaleAllowed(r, pageId));
    }
    // placements sharing strokes: a record matching part of a bigger object ("ab" inside "abc"), or two records of
    // the same strokes (an object edited back to an older text): the bigger one wins, then the newest record.
    // Sharing a few strokes (tiny fragments of overlapping objects) is fine.
    QList<Placement> order = po.found;
    std::stable_sort(order.begin(), order.end(), [](const Placement &a, const Placement &b) {
        return a.matched != b.matched ? a.matched > b.matched : a.rec > b.rec; });
    QSet<int> used;
    QList<Placement> keep;
    for (const Placement &p : order) {
        int shared = 0;
        for (int m : p.members) if (used.contains(m)) ++shared;
        if (shared > qMax(2, p.matched / 5)) continue;
        for (int m : p.members) used.insert(m);
        keep.append(p);
    }
    po.found = keep;
    return po;
}

// Characters of text that no glyph source has (spaces and line breaks aside).
QString missingGlyphs(const QString &text) {
    QString missing;
    useGlyphLanguage(text);
    for (const uint c : text.toUcs4()) {
        const QString ch = QString::fromUcs4(reinterpret_cast<const char32_t *>(&c), 1);
        if (!QChar::isSpace(c) && !glyphOf(char32_t(c)) && !missing.contains(ch)) missing += ch;
    }
    return missing;
}
