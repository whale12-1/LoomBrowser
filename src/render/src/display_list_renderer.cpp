#include "./headers/display_list_renderer.h"

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPointF>
#include <QRectF>
#include <QString>

#include <algorithm>

namespace {

    // ��� DisplayItem::color �������� ��� 0xRRGGBBAA (CSS-�����).
    // Qt ������� 0xAARRGGBB (QRgb). ������������.
    QRgb css_to_qt(uint32_t css_rgba) {
        const uint8_t r = static_cast<uint8_t>((css_rgba >> 24) & 0xFF);
        const uint8_t g = static_cast<uint8_t>((css_rgba >> 16) & 0xFF);
        const uint8_t b = static_cast<uint8_t>((css_rgba >> 8) & 0xFF);
        const uint8_t a = static_cast<uint8_t>(css_rgba & 0xFF);
        return qRgba(r, g, b, a);
    }

} // namespace

QImage DisplayListRenderer::render(const DisplayList& list,
    int width, int height)
{
    QImage img(width, height, QImage::Format_ARGB32);
    img.fill(Qt::white);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);

    for (const auto& item : list.items) {
        p.setOpacity(item.opacity);
        switch (item.type) {
        case DisplayItemType::Rect: {
            const QColor c = QColor::fromRgba(css_to_qt(item.color));
            if (c.alpha() == 0) break;   // ��������� ���������� � ����������
            p.fillRect(QRectF(item.x, item.y, item.width, item.height), c);
            break;
        }
        case DisplayItemType::Text: {
            if (item.text.empty()) break;

            QFont f;
            f.setPixelSize(std::max(1, qRound(item.font_size)));
            f.setBold(item.bold);
            f.setItalic(item.italic);
            if (item.letter_spacing != 0.0f) {
                f.setLetterSpacing(QFont::AbsoluteSpacing, item.letter_spacing);
            }
            p.setFont(f);
            p.setPen(QColor::fromRgba(css_to_qt(item.color)));

            // drawText(QPointF) ���������� baseline. ��� geometry.y � ���� ������.
            // ������� ���� �� ascent, ����� baseline ����� ���������.
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
        }
        // TODO: opacity применяется поштучно. Вложенные элементы с
        // opacity < 1 не композируются (родитель 0.5 × ребёнок 1.0
        // должен давать 0.5, а не 1.0). Правильный stacking context
        // — отдельная задача.
        p.setOpacity(1.0);
    }

    p.end();   // �����������: ��������� QImage �� save()
    return img;
}