#include "./headers/tree_dumper.h"

#include <ostream>
#include <string>

namespace {

    const char* box_type_name(BoxType t) {
        switch (t) {
        case BoxType::Block:          return "Block";
        case BoxType::Inline:         return "Inline";
        case BoxType::AnonymousBlock: return "AnonBlock";
        case BoxType::Text:           return "Text";
        }
        return "?";
    }

    const char* display_name(style::Display d) {
        switch (d) {
        case style::Display::None:        return "none";
        case style::Display::Contents:    return "contents";
        case style::Display::Inline:      return "inline";
        case style::Display::Block:       return "block";
        case style::Display::InlineBlock: return "inline-block";
        case style::Display::Flex:        return "flex";
        case style::Display::InlineFlex:  return "inline-flex";
        case style::Display::Grid:        return "grid";
        case style::Display::InlineGrid:  return "inline-grid";
        case style::Display::Table:       return "table";
        case style::Display::TableRow:    return "table-row";
        case style::Display::TableCell:   return "table-cell";
        }
        return "?";
    }

    const char* position_name(style::Position p) {
        switch (p) {
        case style::Position::Static:   return "static";
        case style::Position::Relative: return "relative";
        case style::Position::Absolute: return "absolute";
        case style::Position::Fixed:    return "fixed";
        case style::Position::Sticky:   return "sticky";
        }
        return "?";
    }

    void indent(std::ostream& out, int depth) {
        for (int i = 0; i < depth; ++i) out << "  ";
    }

    // ѕечатает uint32_t как 8-hex-цифр в CSS-формате #RRGGBBAA
    void print_color(std::ostream& out, uint32_t c) {
        const char* hex = "0123456789abcdef";
        out << '#';
        for (int shift = 28; shift >= 0; shift -= 4) {
            out << hex[(c >> shift) & 0xF];
        }
    }

    void dump_node(const LayoutNode* node,
        const StyleStorageSoA& storage,
        std::ostream& out,
        int depth)
    {
        if (!node) return;

        indent(out, depth);
        out << '[' << box_type_name(node->type) << ']';

        // soa idx и стили Ч только дл€ реальных element-узлов
        if (node->style_soa_idx != UINT32_MAX) {
            const uint32_t si = node->style_soa_idx;
            out << " soa=" << si;
            out << " display=" << display_name(storage.displays[si]);
            out << " position=" << position_name(storage.positions[si]);

            // ÷вета Ч только если не дефолтные
            const uint32_t fg = storage.text_colors[si];
            const uint32_t bg = storage.background_colors[si];
            if (fg != 0x000000FFu) {
                out << " color=";
                print_color(out, fg);
            }
            if (bg != 0x00000000u) {
                out << " bg=";
                print_color(out, bg);
            }

            const float fs = storage.font_sizes[si];
            if (fs != 16.0f) out << " font-size=" << fs;
        }
        else {
            out << " soa=Ч";
        }

        // √еометри€
        const auto& g = node->geometry;
        out << " rect=("
            << g.x << ',' << g.y << ' '
            << g.width << 'x' << g.height << ')';

        // Margin / padding Ч печатаем только если хоть одно не ноль
        if (g.margin_top != 0 || g.margin_right != 0 ||
            g.margin_bottom != 0 || g.margin_left != 0)
        {
            out << " margin=(" << g.margin_top << ',' << g.margin_right
                << ',' << g.margin_bottom << ',' << g.margin_left << ')';
        }
        if (g.padding_top != 0 || g.padding_right != 0 ||
            g.padding_bottom != 0 || g.padding_left != 0)
        {
            out << " padding=(" << g.padding_top << ',' << g.padding_right
                << ',' << g.padding_bottom << ',' << g.padding_left << ')';
        }

        // “екст
        if (node->type == BoxType::Text) {
            out << " \"";
            // Escape кавычек и переводов строк дл€ читаемости
            for (char c : node->text_content) {
                if (c == '"')       out << "\\\"";
                else if (c == '\n') out << "\\n";
                else                out << c;
            }
            out << '"';
        }

        out << '\n';

        for (const auto& c : node->children) {
            dump_node(c.get(), storage, out, depth + 1);
        }
    }

} // namespace

void TreeDumper::dump(const LayoutNode* root,
    const StyleStorageSoA& storage,
    std::ostream& out)
{
    if (!root) {
        out << "(empty layout tree)\n";
        return;
    }
    dump_node(root, storage, out, 0);
}