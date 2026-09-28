#include "./headers/BrowserView.h"

#include <algorithm>
#include <QPainter>
#include <QColor>
#include <QFont>
#include <QPalette>

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
    setMinimumSize(std::max(1, int(list_.document_width)),
        std::max(1, int(list_.document_height)));
    update();
}

void BrowserCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

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