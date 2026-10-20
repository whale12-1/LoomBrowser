#include "./headers/display_list_renderer.h"

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QImage>
#include <algorithm>
#include <QPainterPath>
#include <QDebug>
#include <QFontDatabase>

namespace {

    QRgb css_to_qt(uint32_t css_rgba) {
        const uint8_t r = static_cast<uint8_t>((css_rgba >> 24) & 0xFF);
        const uint8_t g = static_cast<uint8_t>((css_rgba >> 16) & 0xFF);
        const uint8_t b = static_cast<uint8_t>((css_rgba >> 8) & 0xFF);
        const uint8_t a = static_cast<uint8_t>(css_rgba & 0xFF);
        return qRgba(r, g, b, a);
    }

    // Path со скруглёнными углами. Углы могут быть разными.
    QPainterPath rounded_path(const DisplayItem& it) {
        QPainterPath path;
        const QRectF r(it.x, it.y, it.width, it.height);
        const float tl = std::min(it.radius_tl, float(std::min(it.width, it.height) * 0.5f));
        const float tr = std::min(it.radius_tr, float(std::min(it.width, it.height) * 0.5f));
        const float br = std::min(it.radius_br, float(std::min(it.width, it.height) * 0.5f));
        const float bl = std::min(it.radius_bl, float(std::min(it.width, it.height) * 0.5f));

        path.moveTo(r.left() + tl, r.top());
        path.lineTo(r.right() - tr, r.top());
        if (tr > 0) path.arcTo(QRectF(r.right() - 2 * tr, r.top(), 2 * tr, 2 * tr), 90, -90);
        path.lineTo(r.right(), r.bottom() - br);
        if (br > 0) path.arcTo(QRectF(r.right() - 2 * br, r.bottom() - 2 * br, 2 * br, 2 * br), 0, -90);
        path.lineTo(r.left() + bl, r.bottom());
        if (bl > 0) path.arcTo(QRectF(r.left(), r.bottom() - 2 * bl, 2 * bl, 2 * bl), 270, -90);
        path.lineTo(r.left(), r.top() + tl);
        if (tl > 0) path.arcTo(QRectF(r.left(), r.top(), 2 * tl, 2 * tl), 180, -90);
        path.closeSubpath();
        return path;
    }

    // Рисует background-image с учётом size/position/repeat.
    void draw_background(QPainter& p, const DisplayItem& it) {
        QImage img;
        if (it.image) {
            img = *it.image;
        }
        else {
            if (!img.load(QString::fromStdString(it.image_path))) return;
        }

        const float cw = it.width, ch = it.height;
        const float iw = float(img.width()), ih = float(img.height());
        if (iw <= 0 || ih <= 0 || cw <= 0 || ch <= 0) return;

        float dw = iw, dh = ih;

        switch (it.bg_size_kind) {
        case 0: // auto — используем intrinsic; если задан только один explicit,
            // сохраняем пропорции.
            if (it.bg_size_w > 0 && it.bg_size_h > 0) {
                dw = it.bg_size_w; dh = it.bg_size_h;
            }
            else if (it.bg_size_w > 0) {
                dw = it.bg_size_w; dh = dw * ih / iw;
            }
            else if (it.bg_size_h > 0) {
                dh = it.bg_size_h; dw = dh * iw / ih;
            }
            break;
        case 1: { // cover
            const float s = std::max(cw / iw, ch / ih);
            dw = iw * s; dh = ih * s;
            break;
        }
        case 2: { // contain
            const float s = std::min(cw / iw, ch / ih);
            dw = iw * s; dh = ih * s;
            break;
        }
        case 3: { // explicit
            dw = it.bg_size_w > 0 ? it.bg_size_w : iw;
            dh = it.bg_size_h > 0 ? it.bg_size_h
                : (dw * ih / iw);
            if (it.bg_size_h > 0 && it.bg_size_w <= 0)
                dw = dh * iw / ih;
            break;
        }
        }

        if (dw <= 0 || dh <= 0) return;

        // Offset: % означает (container - image) * pct, px — просто сдвиг.
        float ox, oy;
        if (it.bg_pos_x_unit == 1) ox = (cw - dw) * it.bg_pos_x * 0.01f;
        else                        ox = it.bg_pos_x;
        if (it.bg_pos_y_unit == 1) oy = (ch - dh) * it.bg_pos_y * 0.01f;
        else                        oy = it.bg_pos_y;

        const float base_x = it.x + ox;
        const float base_y = it.y + oy;

        const bool rep_x = (it.bg_repeat == 0 || it.bg_repeat == 1);
        const bool rep_y = (it.bg_repeat == 0 || it.bg_repeat == 2);

        p.save();
        if (it.has_radius()) p.setClipPath(rounded_path(it));

        if (!rep_x && !rep_y) {
            p.drawImage(QRectF(base_x, base_y, dw, dh), img);
        }
        else {
            // Начало тайлинга: сдвигаем base_x/base_y назад до попадания
            // в контейнер, чтобы покрыть весь прямоугольник.
            float sx = base_x;
            float sy = base_y;
            if (rep_x) {
                while (sx > it.x) sx -= dw;
                while (sx + dw <= it.x) sx += dw;
            }
            if (rep_y) {
                while (sy > it.y) sy -= dh;
                while (sy + dh <= it.y) sy += dh;
            }
            const float x_end = it.x + cw;
            const float y_end = it.y + ch;
            for (float y = sy; y < y_end; y += dh) {
                for (float x = sx; x < x_end; x += dw) {
                    p.drawImage(QRectF(x, y, dw, dh), img);
                    if (!rep_x) break;
                }
                if (!rep_y) break;
            }
        }
        p.restore();
    }

} // namespace


QImage DisplayListRenderer::render(const DisplayList& list,
    int width, int height)
{
    QImage img(width, height, QImage::Format_ARGB32);
    img.fill(Qt::white);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    for (const auto& item : list.items) {
        // ---- Clip ----
        if (item.has_clip) {
            p.save();
            p.setClipRect(QRectF(item.clip_x, item.clip_y,
                item.clip_w, item.clip_h));
        }

        p.setOpacity(item.opacity);
        switch (item.type) {
        case DisplayItemType::Rect: {
            const QColor c = QColor::fromRgba(css_to_qt(item.color));
            if (c.alpha() == 0) break;
            if (item.has_radius()) {
                p.fillPath(rounded_path(item), c);
            }
            else {
                p.fillRect(QRectF(item.x, item.y, item.width, item.height), c);
            }
            break;
        }
        case DisplayItemType::Text: {
            QFont f;
            if (!item.font_family.empty()) {
                QStringList families = QString::fromStdString(item.font_family).split(",", Qt::SkipEmptyParts);
                for (QString& fam : families) {
                    fam = fam.trimmed();
                    if (fam.startsWith("\"") && fam.endsWith("\""))
                        fam = fam.mid(1, fam.size() - 2);
                    else if (fam.startsWith("'") && fam.endsWith("'"))
                        fam = fam.mid(1, fam.size() - 2);
                }
                QString chosen = "";
                for (const QString& fam : families) {
                    if (fam.compare("sans-serif", Qt::CaseInsensitive) == 0) { chosen = "Arial"; break; }
                    if (fam.compare("serif", Qt::CaseInsensitive) == 0) { chosen = "Times New Roman"; break; }
                    if (fam.compare("monospace", Qt::CaseInsensitive) == 0) { chosen = "Courier New"; break; }
                    if (QFontDatabase::families().contains(fam, Qt::CaseInsensitive)) {
                        chosen = fam; break;
                    }
                }
                if (chosen.isEmpty() && !families.isEmpty()) chosen = families[0];
                if (!chosen.isEmpty()) f.setFamily(chosen);
            }
            f.setPixelSize(std::max(1, qRound(item.font_size)));
            f.setBold(item.bold);
            f.setItalic(item.italic);

            if (item.letter_spacing != 0.0f)
                f.setLetterSpacing(QFont::AbsoluteSpacing, item.letter_spacing);
            p.setFont(f);
            p.setPen(QColor::fromRgba(css_to_qt(item.color)));

            QFontMetricsF fm(f);
            const QString text = QString::fromUtf8(
                item.text.data(), static_cast<int>(item.text.size()));
            const float baseline_y = item.y + fm.ascent();
            p.drawText(QPointF(item.x, baseline_y), text);

            const float underline_y = baseline_y + fm.underlinePos();
            const float overline_y = item.y;
            const float line_through_y = item.y + item.height * 0.5f;
            if (item.underline)
                p.drawLine(QPointF(item.x, underline_y),
                    QPointF(item.x + item.width, underline_y));
            if (item.overline)
                p.drawLine(QPointF(item.x, overline_y),
                    QPointF(item.x + item.width, overline_y));
            if (item.line_through)
                p.drawLine(QPointF(item.x, line_through_y),
                    QPointF(item.x + item.width, line_through_y));
            break;
        }
        case DisplayItemType::Image: {
            if (item.is_background) {
                draw_background(p, item);
            }
            else {
                QImage src;
                if (item.image) {
                    src = *item.image; // <--- БЕРЕМ ИЗ ПАМЯТИ
                }
                else if (!item.image_path.empty()) {
                    if (!src.load(QString::fromStdString(item.image_path))) break;
                }
                else {
                    break;
                }

                if (item.has_radius()) {
                    p.save();
                    p.setClipPath(rounded_path(item));
                    p.drawImage(QRectF(item.x, item.y, item.width, item.height), src);
                    p.restore();
                }
                else {
                    qDebug() << "[Render] Drawing image:" << QString::fromStdString(item.image_path)
                        << "has_image_ptr:" << (item.image != nullptr)
                        << "size:" << item.width << "x" << item.height;
                    p.drawImage(QRectF(item.x, item.y, item.width, item.height), src);
                }
            }
            break;
        }
        }
        p.setOpacity(1.0);

        if (item.has_clip) p.restore();
    }

    p.end();
    return img;
}