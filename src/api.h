// ---- C API for other add-ons (rmpp-hwr) ----
// Another add-on finds these with dlopen("/usr/lib/rmpp/rmpp-textobj/librmpp-textobj-ui.so", RTLD_NOW | RTLD_NOLOAD)
// + dlsym (rmpp-textobj is loaded first: preload.d/55-rmpp-textobj.so). All of them run on xochitl's main (GUI)
// thread and work on the open page and its current lasso selection. Text is UTF-8. rmpp-textobj owns the text object
// records; callers never read or write objects.json.
//
//   int rmpp_textobj_api(void)
//       API version (1).
//   int rmpp_textobj_selection(char *lines, int cap)
//       What the selection holds: -1 nothing selected, 0 no text object, 1 only whole text objects, 2 text objects
//       and other strokes. lines gets the objects' text, one line per text line, top to bottom:
//       "x0 y0 x1 y1<TAB>text\n" (page units: that line's em box). Callers use this text instead of recognizing the
//       objects' strokes again.
//   int rmpp_textobj_edit_selection(void)
//       Opens the edit box for the selected text object(s) (selection = only text objects). 1 if it opened.
//   int rmpp_textobj_drop_objects_from_selection(void)
//       Takes the text objects' strokes out of the selection (to recognize only the handwriting in it). Returns the
//       number of strokes taken out; the selection updates after ~0.5 s.
//   int rmpp_textobj_put_lines(const char *lines, double left, double top, double right, double bottom,
//                              int replace, const char *lang)
//       Draws recognized lines as one text object: lines = "x0 y0 x1 y1<TAB>text\n" per line (page units, each line
//       at the size of its handwriting, over it when replace, else stacked below the selection bounds given).
//       replace: the selection is deleted first. lang: the recognizer's language ("ko", "zh", ...; "" = unknown),
//       the first place to look for glyphs. Returns 1 when the object is being drawn, 0 if there is nothing to draw,
//       -1 if characters have no glyphs (nothing is drawn; a hint names them), -2 no open page.
//   int rmpp_textobj_missing(const char *text, char *missing, int cap)
//       Characters of text without glyphs (written to missing); returns their count.

namespace {

constexpr int kApiVersion = 1;

int copyOut(const QString &s, char *buf, int cap) {
    if (!buf || cap <= 0) return 0;
    const QByteArray u = s.toUtf8();
    const int n = qMin(int(u.size()), cap - 1);
    memcpy(buf, u.constData(), size_t(n));
    buf[n] = '\0';
    return n;
}

struct OpenPage {
    QQuickItem *dv = nullptr;
    QObject *sc = nullptr, *ph = nullptr;
    QString pageId;
};

OpenPage openPage() {
    OpenPage p;
    p.dv = documentView();
    p.sc = p.dv ? p.dv->property("sceneController").value<QObject *>() : nullptr;
    p.ph = p.dv ? p.dv->property("penHandler").value<QObject *>() : nullptr;
    if (p.dv) p.pageId = p.dv->property("currentPageId").toString();
    return p;
}

// The text lines of objects with their em boxes (page units), top to bottom.
QString objectLines(const PageObjects &po, QList<Placement> objs) {
    std::sort(objs.begin(), objs.end(), [](const Placement &a, const Placement &b) { return a.box.top() < b.box.top(); });
    QString out;
    for (const Placement &p : objs) {
        const Rec &r = po.recs[p.rec];
        const QPointF o = originOf(r, p);
        const double em = r.em * p.k;
        const QStringList lines = r.text.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            if (blankText(lines[i])) continue;
            const double y = o.y() + i * em * 1.15;
            out += QStringLiteral("%1 %2 %3 %4\t%5\n").arg(o.x()).arg(y).arg(qMax(o.x() + em, p.box.right())).arg(y + em).arg(lines[i]);
        }
    }
    return out;
}

} // namespace

#define RMPP_TEXTOBJ_API extern "C" __attribute__((visibility("default")))

RMPP_TEXTOBJ_API int rmpp_textobj_api(void) { return kApiVersion; }

RMPP_TEXTOBJ_API int rmpp_textobj_selection(char *lines, int cap) {
    copyOut(QString(), lines, cap);
    const OpenPage p = openPage();
    if (!p.sc) return -1;
    const QList<QRect> sel = lineRects(p.sc);
    if (sel.isEmpty()) return -1;
    upgradeLegacy(p.sc, p.pageId, sel);
    const PageObjects po = findObjects(p.sc, p.pageId, true);
    bool only = false;
    const QList<Placement> objs = selectedObjects(po, sel, &only);
    if (objs.isEmpty()) return 0;
    copyOut(objectLines(po, objs), lines, cap);
    return only ? 1 : 2;
}

RMPP_TEXTOBJ_API int rmpp_textobj_edit_selection(void) {
    const OpenPage p = openPage();
    if (!p.sc || !p.ph || g_edit.box) return 0;
    const QList<QRect> sel = lineRects(p.sc);
    if (sel.isEmpty()) return 0;
    const PageObjects po = findObjects(p.sc, p.pageId, true);
    bool only = false;
    QList<int> marks;
    const QList<Placement> objs = selectedObjects(po, sel, &only, nullptr, &marks);
    if (!only) return 0;
    openObjectEdit(p.dv, p.sc, p.ph, po, objs, marks);
    return g_edit.box ? 1 : 0;
}

RMPP_TEXTOBJ_API int rmpp_textobj_drop_objects_from_selection(void) {
    const OpenPage p = openPage();
    if (!p.sc) return 0;
    const QList<QRect> sel = lineRects(p.sc);
    if (sel.isEmpty()) return 0;
    const PageObjects po = findObjects(p.sc, p.pageId, true);
    QSet<QString> selected;
    for (const QRect &r : sel) selected.insert(rectKey(r));
    QList<int> drop, marks;
    selectedObjects(po, sel, nullptr, nullptr, &marks);
    for (const Placement &p2 : po.found)
        for (int m : p2.members) if (selected.contains(rectKey(po.rects[m]))) drop << m;
    drop << marks;   // marks on the objects are not handwriting to recognize either
    dropFromSelection(p.sc, po.rects, drop, selected);
    return int(drop.size());
}

RMPP_TEXTOBJ_API int rmpp_textobj_missing(const char *text, char *missing, int cap) {
    const QString m = missingGlyphs(QString::fromUtf8(text ? text : ""));
    copyOut(m, missing, cap);
    return int(m.toUcs4().size());
}

RMPP_TEXTOBJ_API int rmpp_textobj_put_lines(const char *lines, double left, double top, double right, double bottom,
                                            int replace, const char *lang) {
    const OpenPage p = openPage();
    if (!p.sc || !p.ph) return -2;
    g_langHint = QString::fromUtf8(lang ? lang : "");
    const QStringList ls = QString::fromUtf8(lines ? lines : "").split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QStringList texts;
    for (const QString &l : ls)
        if (!blankText(l.section(QLatin1Char('\t'), 1))) texts << l.section(QLatin1Char('\t'), 1);
    if (texts.isEmpty()) return 0;   // nothing but blanks: no object
    const QString missing = missingGlyphs(texts.join(QLatin1Char(' ')));
    if (!missing.isEmpty()) {
        showHint(p.dv, QStringLiteral("No glyphs for: %1. Add the language in Settings > Languages.").arg(missing));
        return -1;
    }
    Placed pl = placeLines(ls, QRectF(QPointF(left, top), QPointF(right, bottom)), replace != 0);
    if (pl.strokes.isEmpty()) return 0;
    TLOG("text object from %lld recognized line(s)", (long long)ls.size());
    putInk(p.sc, p.ph, p.dv, p.sc->property("currentLayer").toInt(), replace != 0, pl, texts.join(QLatin1Char('\n')), {});
    return 1;
}
