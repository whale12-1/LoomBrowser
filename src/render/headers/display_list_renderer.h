#pragma once
#include <QImage>
#include "./parsers/selector_matcher/display_list.h"

// Растеризует DisplayList в QImage через QPainter.
// Требует, чтобы QGuiApplication уже был создан (для QFont).
class DisplayListRenderer {
public:
    static QImage render(const DisplayList& list, int width, int height);
};