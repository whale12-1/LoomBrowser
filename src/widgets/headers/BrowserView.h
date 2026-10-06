#pragma once

#include <QWidget>
#include <QScrollArea>
#include <QPixmap>

#include "../parsers/selector_matcher/display_list.h"

class BrowserCanvas : public QWidget {
public:
    explicit BrowserCanvas(QWidget* parent = nullptr);

    // Публичный API — вызывается из BrowserWindow::onHttpResponse.
    // Первый вызов после load делает дорогой рендер в backing_.
    void setDisplayList(const DisplayList& list);

    // Нужен для makeQtTextMetrics(view_->canvas()->font()).
    const DisplayList& displayList() const { return list_; }

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    void invalidateBacking();
    void ensureBacking();               // гарантирует, что backing_ актуален
    void renderToBacking();

    DisplayList list_;
    QPixmap     backing_;
    bool        backing_dirty_ = true;  // true = нужно перерисовать
    qreal       backing_dpr_ = 0.0;   // DPR, при котором был отрендерен

    // Порог памяти для backing store. Если W*H*DPR^2*4 > kMaxBackingBytes,
    // рендерим напрямую, без кэша. Спасает на огромных страницах.
    static constexpr qint64 kMaxBackingBytes = 200ll * 1024 * 1024;   // 200 МБ
    bool use_backing_ = true;
};

class BrowserView : public QScrollArea {
public:
    explicit BrowserView(QWidget* parent = nullptr);

    void setDisplayList(const DisplayList& list);
    BrowserCanvas* canvas() const { return canvas_; }

private:
    BrowserCanvas* canvas_{};
};