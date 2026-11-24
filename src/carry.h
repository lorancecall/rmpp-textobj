// ---- records carried in the strokes ----
// A text object's record travels inside its own strokes: the speed field of their points (u16 per point, stored in
// the page's .rm file with the stroke). Fineliner strokes are drawn the same whatever the speed (checked on the
// device, OS 3.29.1: a marked and an unmarked object pixel for pixel), and xochitl keeps the values through moves,
// scaling, copy and paste, and sync. So the record goes wherever the strokes go: to the cloud, to another tablet, into
// a copy on another page; it is gone with them when the page or notebook is deleted. A stock tablet sees plain
// strokes. Nothing is written next to the notebook (an unknown file in a document folder stops its sync: the cloud
// rejects it) and no record file is kept on the tablet.
//
// Per stroke: point 0: 0xA000 | stroke index (12 bits); last point: the object id (16 bits); the points between hold
// the record's words in order, stroke after stroke (stroke 0's first, then stroke 1's ...; words past the end are 0).
// xochitl drops points that lie exactly on the line between their neighbours (device: midpoints added to two-point
// strokes were gone, their neighbours kept with their values), so where the glyphs' own inner points are too few,
// points are added a quarter unit off the segment's line (a pen is two units wide or more: nothing visible).
// Highlights are not carried: they live in the session's record only (after a restart the bands on an object are
// taken as marks drawn over it).
// Words: the stroke count, the byte count, then the bytes (little endian pairs):
//   u8 version (1), u8 weight, f32 em, f32 frame width, f32 frame height, f32 ox, f32 oy,
//   u16 text bytes + UTF-8, u16 line count, per line: u16 bytes + UTF-8, f32 x, f32 y, f32 em, then u16 CRC-16.
// The frame is the union of the strokes' points (no pen width); ox, oy and the lines are relative to its top-left.
// A copy drawn at another size scales them by its frame's width.

constexpr quint16 kCarryTag = 0xA000;
constexpr int kCarryMaxStrokes = 0x0fff;

quint32 newCarryId() {
    static quint32 seq = 0;
    const quint32 v = (quint32(QDateTime::currentMSecsSinceEpoch()) * 2654435761u + (++seq) * 40503u) >> 16;
    return v ? v : 1;
}

QString carryKey(quint32 id) { return QStringLiteral("c") + QString::number(id, 16); }

quint16 crc16(const QByteArray &b) {
    quint16 c = 0xffff;
    for (const char ch : b) {
        c ^= quint16(quint8(ch)) << 8;
        for (int i = 0; i < 8; ++i) c = (c & 0x8000) ? quint16((c << 1) ^ 0x1021) : quint16(c << 1);
    }
    return c;
}

QRectF pointsFrame(const QList<QList<QPointF>> &strokes) {
    QRectF f;
    bool any = false;
    for (const auto &s : strokes)
        for (const QPointF &p : s) {
            if (!any) { f = QRectF(p, QSizeF(0, 0)); any = true; }
            else {
                f.setLeft(qMin(f.left(), p.x())); f.setRight(qMax(f.right(), p.x()));
                f.setTop(qMin(f.top(), p.y())); f.setBottom(qMax(f.bottom(), p.y()));
            }
        }
    return f;
}

namespace carry {
void u8(QByteArray *b, int v) { b->append(char(quint8(v))); }
void u16(QByteArray *b, int v) { b->append(char(v & 0xff)); b->append(char((v >> 8) & 0xff)); }
void f32(QByteArray *b, double v) {
    const float f = float(v);
    quint32 u;
    memcpy(&u, &f, 4);
    for (int i = 0; i < 4; ++i) b->append(char((u >> (8 * i)) & 0xff));
}
void str(QByteArray *b, const QString &s) {
    const QByteArray u = s.toUtf8().left(0xffff);
    u16(b, int(u.size()));
    b->append(u);
}
struct Rd {
    const QByteArray &b;
    int p = 0;
    bool ok = true;
    bool need(int n) { if (p + n > b.size()) ok = false; return ok; }
    int u8() { if (!need(1)) return 0; return quint8(b[p++]); }
    int u16() { if (!need(2)) return 0; const int v = quint8(b[p]) | (quint8(b[p + 1]) << 8); p += 2; return v; }
    double f32() {
        if (!need(4)) return 0;
        quint32 u = 0;
        for (int i = 0; i < 4; ++i) u |= quint32(quint8(b[p + i])) << (8 * i);
        p += 4;
        float f;
        memcpy(&f, &u, 4);
        return std::isfinite(f) ? double(f) : 0;
    }
    QString str() { const int n = u16(); if (!need(n)) return {}; const QString s = QString::fromUtf8(b.constData() + p, n); p += n; return s; }
};
}   // namespace carry

// The record's bytes for text placed as pl (pl.strokes in page units, before they are drawn).
QByteArray packRecord(const QString &text, const Placed &pl) {
    const QRectF f = pointsFrame(pl.strokes);
    QByteArray b;
    carry::u8(&b, 1);
    carry::u8(&b, pl.weight);
    carry::f32(&b, pl.em);
    carry::f32(&b, f.width());
    carry::f32(&b, f.height());
    carry::f32(&b, pl.origin.x() - f.left());
    carry::f32(&b, pl.origin.y() - f.top());
    carry::str(&b, text);
    carry::u16(&b, int(qMin<qsizetype>(pl.lines.size(), 0xffff)));
    for (const LineBox &l : pl.lines) {
        carry::str(&b, l.t);
        carry::f32(&b, l.o.x() - f.left());
        carry::f32(&b, l.o.y() - f.top());
        carry::f32(&b, l.em);
    }
    carry::u16(&b, crc16(b));
    return b;
}

struct Carried {
    QString text;
    int weight = 1;
    double em = 0, fw = 0, fh = 0, ox = 0, oy = 0;
    QList<LineBox> lines;   // o relative to the frame's top-left
};

bool unpackRecord(const QByteArray &b, Carried *out) {
    if (b.size() < 3 || crc16(b.left(b.size() - 2)) != (quint8(b[b.size() - 2]) | (quint8(b[b.size() - 1]) << 8))) return false;
    carry::Rd r{b};
    if (r.u8() != 1) return false;
    out->weight = qBound(0, r.u8(), 2);
    out->em = r.f32();
    out->fw = r.f32();
    out->fh = r.f32();
    out->ox = r.f32();
    out->oy = r.f32();
    out->text = r.str();
    const int nl = r.u16();
    for (int i = 0; i < nl && r.ok; ++i) {
        LineBox l;
        l.t = r.str();
        const double x = r.f32(), y = r.f32();
        l.o = QPointF(x, y);
        l.em = r.f32();
        out->lines.append(l);
    }
    return r.ok && out->em > 0 && !out->text.isEmpty();
}

// Words the record needs: stroke count, byte count, the bytes.
int carryWordCount(int bytes) { return 2 + (bytes + 1) / 2; }

// Room for `words` words between the strokes' first and last points: points are added on the longest segments, a
// quarter unit off their line (on alternate sides), so that xochitl keeps them. One-point strokes get a second point.
void makeCarryRoom(QList<QList<QPointF>> *strokes, int words) {
    int room = 0;
    for (QList<QPointF> &s : *strokes) {
        if (s.size() == 1) s.append(s[0] + QPointF(0.3, 0.3));
        room += qMax(0, int(s.size()) - 2);
    }
    int side = 1;
    // stroke 0 holds the stroke count where the reader looks first: two inner points at least
    while (!strokes->isEmpty() && (*strokes)[0].size() < 4) {
        QList<QPointF> &s = (*strokes)[0];
        int bi = 0;
        double bl = -1;
        for (int i = 0; i + 1 < s.size(); ++i) {
            const QPointF d = s[i + 1] - s[i];
            if (d.x() * d.x() + d.y() * d.y() > bl) { bl = d.x() * d.x() + d.y() * d.y(); bi = i; }
        }
        const QPointF d = s[bi + 1] - s[bi];
        const double l = std::sqrt(qMax(bl, 1e-9));
        const QPointF nv = bl > 1e-6 ? QPointF(-d.y() / l, d.x() / l) : QPointF(0.7071, 0.7071);
        s.insert(bi + 1, (s[bi] + s[bi + 1]) / 2 + nv * (0.25 * side));
        side = -side;
        ++room;
    }
    while (room < words) {
        int bs = -1, bi = 0;
        double bl = -1;
        for (int si = 0; si < strokes->size(); ++si) {
            const QList<QPointF> &s = (*strokes)[si];
            for (int i = 0; i + 1 < s.size(); ++i) {
                const QPointF d = s[i + 1] - s[i];
                const double l = d.x() * d.x() + d.y() * d.y();
                if (l > bl) { bl = l; bs = si; bi = i; }
            }
        }
        if (bs < 0) return;
        QList<QPointF> &s = (*strokes)[bs];
        const QPointF d = s[bi + 1] - s[bi];
        const double l = std::sqrt(qMax(bl, 1e-9));
        const QPointF n = bl > 1e-6 ? QPointF(-d.y() / l, d.x() / l) : QPointF(0.7071, 0.7071);
        s.insert(bi + 1, (s[bi] + s[bi + 1]) / 2 + n * (0.25 * side));
        side = -side;
        ++room;
    }
}

// The speed of every point of every stroke: the head and tail marks and the record's words. Empty when the strokes
// have no room (makeCarryRoom first) or are too many.
QList<QList<quint16>> carrySpeeds(quint32 id, const QByteArray &payload, const QList<QList<QPointF>> &strokes) {
    QList<QList<quint16>> out;
    const int n = int(strokes.size());
    if (n <= 0 || n > kCarryMaxStrokes || payload.size() > 0xffff) return out;
    QList<quint16> stream{quint16(n), quint16(payload.size())};
    for (int i = 0; i < payload.size(); i += 2)
        stream << quint16(quint8(payload[i]) | (i + 1 < payload.size() ? quint8(payload[i + 1]) << 8 : 0));
    int at = 0;
    for (int si = 0; si < n; ++si) {
        const int m = int(strokes[si].size());
        if (m < 2) return {};
        QList<quint16> w(m, 0);
        w[0] = quint16(kCarryTag | si);
        w[m - 1] = quint16(id & 0xffff);
        for (int k = 1; k + 1 < m; ++k) w[k] = at < stream.size() ? stream[at++] : quint16(0);
        out << w;
    }
    if (at < stream.size()) return {};
    return out;
}

// ---- reading a page file (.rm v6): our strokes and their points ----
// Format notes follow rmscene (MIT) as rmpp-hwr's reader does (src/rm.rs there).
struct FileStroke {
    int tool = 0;
    double thickness = 0;
    QList<InkPt> pts;
};

namespace rmf {
struct Cur {
    const char *b;
    qsizetype n, p = 0;
    qsizetype left() const { return n - p; }
    bool u8(quint8 *v) { if (p >= n) return false; *v = quint8(b[p++]); return true; }
    bool raw(void *dst, qsizetype k) { if (p + k > n) return false; memcpy(dst, b + p, size_t(k)); p += k; return true; }
    bool varuint(quint64 *v) {
        *v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            quint8 c;
            if (!u8(&c)) return false;
            *v |= quint64(c & 0x7f) << shift;
            if (!(c & 0x80)) return true;
        }
        return false;
    }
    bool tag(quint64 index, quint64 kind) {
        const qsizetype save = p;
        quint64 t;
        if (varuint(&t) && (t >> 4) == index && (t & 0xf) == kind) return true;
        p = save;
        return false;
    }
    bool id(quint64 index) { quint8 a; quint64 c; return tag(index, 0xf) && u8(&a) && varuint(&c); }
    bool i32(quint64 index, quint32 *v) { return tag(index, 0x4) && raw(v, 4); }
    bool f64(quint64 index, double *v) { return tag(index, 0x8) && raw(v, 8); }
    bool sub(quint64 index, Cur *out) {
        quint32 len;
        if (!tag(index, 0xc) || !raw(&len, 4) || p + len > n) return false;
        *out = Cur{b + p, qsizetype(len)};
        p += len;
        return true;
    }
};
}   // namespace rmf

// The strokes of a page file that carry a record (deleted strokes left out). False if it is not a v6 file.
bool readPageStrokes(const QByteArray &data, QList<FileStroke> *out) {
    static const char kHead[] = "reMarkable .lines file, version=6";
    if (data.size() < 43 || !data.startsWith(kHead)) return false;
    rmf::Cur c{data.constData(), data.size(), 43};
    while (c.left() >= 8) {
        quint32 len;
        quint8 unk, minv, ver, kind;
        if (!c.raw(&len, 4) || !c.u8(&unk) || !c.u8(&minv) || !c.u8(&ver) || !c.u8(&kind) || c.p + len > c.n) break;
        rmf::Cur body{c.b + c.p, qsizetype(len)};
        c.p += len;
        if (kind != 0x05) continue;   // line item block
        quint32 deleted = 0, tool = 0, color = 0;
        double thick = 0;
        rmf::Cur v, pts;
        quint8 itemType;
        if (!body.id(1) || !body.id(2) || !body.id(3) || !body.id(4) || !body.i32(5, &deleted) || deleted) continue;
        if (!body.sub(6, &v) || !v.u8(&itemType) || itemType != 0x03) continue;
        if (!v.i32(1, &tool) || !v.i32(2, &color) || !v.f64(3, &thick)) continue;
        float start;
        if (v.tag(4, 0x4)) v.raw(&start, 4);
        if (!v.sub(5, &pts) || ver < 2) continue;
        FileStroke s;
        s.tool = int(tool);
        s.thickness = thick;
        if (pts.left() < 28 || (quint16(quint8(pts.b[8]) | (quint8(pts.b[9]) << 8)) & 0xf000) != kCarryTag)
            continue;   // not one of ours (point 0's speed has no tag)
        while (pts.left() >= 14) {
            InkPt q;
            pts.raw(&q, 14);
            s.pts.append(q);
        }
        if (!s.pts.isEmpty()) out->append(s);
    }
    return true;
}
