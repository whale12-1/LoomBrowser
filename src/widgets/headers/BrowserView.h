#pragma once

#include <QWidget>
#include <QScrollArea>

#include "../parsers/selector_matcher/display_list.h"

class BrowserCanvas : public QWidget {
public:
    explicit BrowserCanvas(QWidget* parent = nullptr);

    void setDisplayList(const DisplayList& list);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    DisplayList list_;
};

class BrowserView : public QScrollArea {
public:
    explicit BrowserView(QWidget* parent = nullptr);

    void setDisplayList(const DisplayList& list);
    BrowserCanvas* canvas() const { return canvas_; }

private:
    BrowserCanvas* canvas_{};
};