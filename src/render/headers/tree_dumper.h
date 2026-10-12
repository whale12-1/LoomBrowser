#pragma once
#include <iosfwd>

#include "../layout/headers/layout_node.h"
#include "../parsers/selector_matcher/style_storage_soa.h"

// Выгружает дерево layout-узлов в текстовый вид: тип, стили,
// геометрия, margin/padding, текст. Формат — одна строка на узел,
// дети с отступом в 2 пробела на уровень.
class TreeDumper {
public:
    static void dump(const LayoutNode* root,
        const StyleStorageSoA& storage,
        std::ostream& out);
};