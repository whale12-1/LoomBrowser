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

    // Наш DisplayItem::color хранится как 0xRRGGBBAA (CSS-стиль).
    // Qt ожидает 0xAARRGGBB (QRgb). Конвертируем.
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
        switch (item.type) {
        case DisplayItemType::Rect: {
            const QColor c = QColor::fromRgba(css_to_qt(item.color));
            if (c.alpha() == 0) break;   // полностью прозрачный — пропускаем
            p.fillRect(QRectF(item.x, item.y, item.width, item.height), c);
            break;
        }
        case DisplayItemType::Text: {
            if (item.text.empty()) break;

            QFont f;
            f.setPixelSize(std::max(1, qRound(item.font_size)));
            f.setBold(item.bold);
            p.setFont(f);
            p.setPen(QColor::fromRgba(css_to_qt(item.color)));

            // drawText(QPointF) использует baseline. Наш geometry.y — верх строки.
            // Смещаем вниз на ascent, чтобы baseline встал правильно.
            QFontMetricsF fm(f);
            const QString text = QString::fromUtf8(
                item.text.data(), static_cast<int>(item.text.size()));
            p.drawText(QPointF(item.x, item.y + fm.ascent()), text);
            break;
        }
        }
    }

    p.end();   // обязательно: отпускает QImage до save()
    return img;
}