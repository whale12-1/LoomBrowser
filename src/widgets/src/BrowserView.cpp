#include "../headers/BrowserView.h"

#include <algorithm>
#include <QPainter>
#include <QPaintEvent>          // ← вот это
#include <QColor>
#include <QFont>
#include <QPalette>
#include <QDebug>
// ============================================================
//  BrowserCanvas
// ============================================================

BrowserCanvas::BrowserCanvas(QWidget* parent) : QWidget(parent) {
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::white);
    setPalette(pal);
}

void BrowserCanvas::setDisplayList(const DisplayList& list) {
    list_ = list;

    // Размер виджета = размер документа. QScrollArea выставит скроллбары.
    const int w = std::max(1, int(list_.document_width));
    const int h = std::max(1, int(list_.document_height));
    setMinimumSize(w, h);

    // Проверим, влезает ли backing по памяти. Если нет — пойдём
    // медленным путём через paintEvent каждый кадр.
    const qreal dpr = devicePixelRatioF();
    const qint64 bytes = qint64(w) * qint64(h) *
        qint64(dpr * dpr) * qint64(4);
    use_backing_ = (bytes <= kMaxBackingBytes);

    if (!use_backing_) {
        qWarning() << "[backing] too large:" << bytes / (1024 * 1024) << "MB, "
            << "falling back to direct rendering (" << w << "x" << h << ")";
    }

    invalidateBacking();
    update();
}

void BrowserCanvas::invalidateBacking() {
    backing_ = QPixmap();
    backing_dirty_ = true;
}

void BrowserCanvas::ensureBacking() {
    if (!use_backing_) return;

    const qreal dpr = devicePixelRatioF();

    // Уже валиден и с тем же DPR?
    if (!backing_dirty_ && !backing_.isNull() && backing_dpr_ == dpr) return;

    renderToBacking();

    backing_dpr_ = dpr;
    backing_dirty_ = false;
}

void BrowserCanvas::renderToBacking() {
    const int doc_w = std::max(1, int(list_.document_width));
    const int doc_h = std::max(1, int(list_.document_height));
    const qreal dpr = devicePixelRatioF();
    qDebug() << "[dpr] canvas:" << dpr
        << "screen:" << (screen() ? screen()->devicePixelRatio() : -1.0)
        << "topLevel:" << (window() ? window()->devicePixelRatioF() : -1.0)
        << "phys_size:" << QSize(int(doc_w * dpr), int(doc_h * dpr));
    // Физический размер pixmap'а — с учётом DPR (для HiDPI).
    const QSize phys_size(std::max(1, int(doc_w * dpr)),
        std::max(1, int(doc_h * dpr)));

    backing_ = QPixmap(phys_size);
    if (backing_.isNull()) {
        // QPixmap отказался — слишком большой. Фолбэк.
        qWarning() << "[backing] pixmap allocation failed, falling back";
        use_backing_ = false;
        return;
    }

    // Логические координаты при рисовании — 0..doc_w, 0..doc_h.
    backing_.setDevicePixelRatio(dpr);
    backing_.fill(Qt::white);

    QPainter p(&backing_);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Тот же цикл, что был в старом paintEvent.
    for (const auto& item : list_.items) {
        const QColor color(int((item.color >> 24) & 0xFF),
            int((item.color >> 16) & 0xFF),
            int((item.color >> 8) & 0xFF),
            int(item.color & 0xFF));

        switch (item.type) {
        case DisplayItemType::Rect:
            p.fillRect(QRectF(item.x, item.y, item.width, item.height), color);
            break;

        case DisplayItemType::Text: {
            QFont f = font();
            f.setPixelSize(std::max(1, int(item.font_size)));         // ← без dpr
            f.setBold(item.bold);
            p.setFont(f);
            p.setPen(color);
            p.drawText(QRectF(item.x, item.y, item.width, item.height),
                Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                QString::fromStdString(item.text));
            break;
        }
        }
    }
}

void BrowserCanvas::paintEvent(QPaintEvent* ev) {
    if (use_backing_ && !list_.items.empty()) {
        ensureBacking();
        if (!backing_.isNull()) {
            QPainter p(this);

            // ev->rect() — логические координаты виджета.
            // Источник в pixmap'е должен быть в ФИЗИЧЕСКИХ пикселях.
            const qreal dpr = devicePixelRatioF();
            const QRect  target = ev->rect();              // логические
            const QRect  source(int(target.x() * dpr),
                int(target.y() * dpr),
                int(target.width() * dpr),
                int(target.height() * dpr));

            p.drawPixmap(target, backing_, source);
            return;
        }
    }

    // Fallback: рендерим всё напрямую, как раньше.
    // Сюда попадаем либо когда backing отключён по памяти,
    // либо когда QPixmap не смог выделиться.
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // Отсекаем всё за пределами ev->rect() — это сильно ускоряет fallback.
    p.setClipRect(ev->rect());

    for (const auto& item : list_.items) {
        const QColor color(int((item.color >> 24) & 0xFF),
            int((item.color >> 16) & 0xFF),
            int((item.color >> 8) & 0xFF),
            int(item.color & 0xFF));

        switch (item.type) {
        case DisplayItemType::Rect:
            p.fillRect(QRectF(item.x, item.y, item.width, item.height), color);
            break;

        case DisplayItemType::Text: {
            QFont f = font();
            f.setPixelSize(std::max(1, int(item.font_size)));
            f.setBold(item.bold);
            p.setFont(f);
            p.setPen(color);
            p.drawText(QRectF(item.x, item.y, item.width, item.height),
                Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                QString::fromStdString(item.text));
            break;
        }
        }
    }
}
void BrowserCanvas::mousePressEvent(QMouseEvent* ev) {
    if (ev->button() == Qt::LeftButton) {
        emit clicked(float(ev->position().x()),
            float(ev->position().y()));
    }
    QWidget::mousePressEvent(ev);
}

// ============================================================
//  BrowserView
// ============================================================

BrowserView::BrowserView(QWidget* parent) : QScrollArea(parent) {
    canvas_ = new BrowserCanvas(this);
    setWidget(canvas_);
    setWidgetResizable(false);
    setAlignment(Qt::AlignLeft | Qt::AlignTop);
    setBackgroundRole(QPalette::Base);
}

void BrowserView::setDisplayList(const DisplayList& list) {
    canvas_->setDisplayList(list);
}

