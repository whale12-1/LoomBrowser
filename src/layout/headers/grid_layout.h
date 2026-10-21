#pragma once
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <cmath>
#include "layout_node.h"
#include "../parsers/selector_matcher/style_storage_soa.h"

class GridLayoutEngine {
public:
    using LayoutChildFn = std::function<void(LayoutNode*, float, float, float, float)>;

    static void layout_grid(LayoutNode* node, float content_width, float col_gap, float row_gap,
        const StyleStorageSoA& storage,
        LayoutChildFn layout_child_fn)
    {
        const uint32_t si = node->style_soa_idx;
        const float pb_h = node->geometry.padding_left + node->geometry.padding_right + node->geometry.border_left + node->geometry.border_right;

        // 2. Парсим шаблон колонок
        std::string template_cols = (si != UINT32_MAX) ? storage.grid_template_columns[si] : "";
        if (template_cols.empty()) template_cols = "none";

        std::vector<float> col_widths = parse_tracks(template_cols, content_width, col_gap);

        if (col_widths.empty()) {
            col_widths.push_back(content_width);
        }

        // 3. Раскладываем элементы (Auto-placement)
        struct GridItem {
            LayoutNode* node;
            int r, c, rowspan, colspan;
        };
        std::vector<GridItem> items;
        int num_cols = (int)col_widths.size();

        int cur_row = 0;
        int cur_col = 0;
        std::vector<std::vector<bool>> occupied;

        for (auto& child : node->children) {
            if (child->style_soa_idx != UINT32_MAX && storage.positions[child->style_soa_idx] == style::Position::Absolute) continue;

            int colspan = 1, rowspan = 1;
            std::string col_val = (child->style_soa_idx != UINT32_MAX) ? storage.grid_column[child->style_soa_idx] : "";

            if (col_val.find("span") != std::string::npos) {
                try { colspan = std::stoi(col_val.substr(col_val.find("span") + 4)); }
                catch (...) {}
            }

            while (true) {
                if (cur_row >= (int)occupied.size()) occupied.resize(cur_row + 1);
                if (cur_col >= (int)occupied[cur_row].size()) occupied[cur_row].resize(cur_col + 1, false);

                bool fits = true;
                for (int r = 0; r < rowspan; ++r) {
                    for (int c = 0; c < colspan; ++c) {
                        if (cur_row + r >= (int)occupied.size()) occupied.resize(cur_row + r + 1);
                        if (cur_col + c >= (int)occupied[cur_row + r].size()) occupied[cur_row + r].resize(cur_col + c + 1, false);
                        if (occupied[cur_row + r][cur_col + c]) { fits = false; break; }
                    }
                    if (!fits) break;
                }

                if (fits) break;
                cur_col++;
                if (cur_col >= num_cols) { cur_col = 0; cur_row++; }
            }

            for (int r = 0; r < rowspan; ++r) {
                for (int c = 0; c < colspan; ++c) {
                    occupied[cur_row + r][cur_col + c] = true;
                }
            }

            items.push_back({ child.get(), cur_row, cur_col, rowspan, colspan });
            cur_col += colspan;
            if (cur_col >= num_cols) { cur_col = 0; cur_row++; }
        }

        // 4. Вычисляем высоты строк
        int total_rows = (int)occupied.size();
        std::vector<float> row_heights(total_rows, 0.0f);

        for (auto& item : items) {
            float item_x = node->geometry.x + node->geometry.border_left + node->geometry.padding_left;
            for (int i = 0; i < item.c; ++i) item_x += col_widths[i] + col_gap;

            float item_w = 0;
            for (int i = 0; i < item.colspan; ++i) item_w += col_widths[item.c + i];
            if (item.colspan > 1) item_w += col_gap * (item.colspan - 1);

            layout_child_fn(item.node, item_x, node->geometry.y, item_w, -1.0f);
            float h = item.node->geometry.height;
            row_heights[item.r] = std::max(row_heights[item.r], h);
        }

        // 5. Финализируем позиции
        float total_height = 0.0f;
        for (float h : row_heights) total_height += h;
        if (total_rows > 1) total_height += row_gap * (total_rows - 1);

        float current_y = node->geometry.y + node->geometry.border_top + node->geometry.padding_top;

        for (auto& item : items) {
            float item_y = current_y;
            for (int i = 0; i < item.r; ++i) item_y += row_heights[i] + row_gap;

            float item_x = node->geometry.x + node->geometry.border_left + node->geometry.padding_left;
            for (int i = 0; i < item.c; ++i) item_x += col_widths[i] + col_gap;

            float item_w = 0;
            for (int i = 0; i < item.colspan; ++i) item_w += col_widths[item.c + i];
            if (item.colspan > 1) item_w += col_gap * (item.colspan - 1);

            float item_h = 0;
            for (int i = 0; i < item.rowspan; ++i) item_h += row_heights[item.r + i];
            if (item.rowspan > 1) item_h += row_gap * (item.rowspan - 1);

            layout_child_fn(item.node, item_x, item_y, item_w, item_h);
        }

        node->geometry.width = pb_h + content_width;
        node->geometry.height = node->geometry.border_top + node->geometry.padding_top + total_height + node->geometry.padding_bottom + node->geometry.border_bottom;
    }

private:
    static std::vector<float> parse_tracks(const std::string& tmpl, float width, float gap) {
        std::string expanded = expand_repeat(tmpl, width);

        // Разбиваем по пробелам (игнорируя пробелы внутри скобок)
        std::vector<std::string> tokens;
        size_t i = 0;
        while (i < expanded.size()) {
            while (i < expanded.size() && std::isspace(expanded[i])) ++i;
            if (i >= expanded.size()) break;
            size_t start = i;
            if (expanded[i] == 'm' && expanded.substr(i, 7) == "minmax(") {
                int depth = 1; i += 7;
                while (i < expanded.size() && depth > 0) {
                    if (expanded[i] == '(') depth++;
                    else if (expanded[i] == ')') depth--;
                    ++i;
                }
            }
            else {
                while (i < expanded.size() && !std::isspace(expanded[i])) ++i;
            }
            tokens.push_back(expanded.substr(start, i - start));
        }

        float total_gap = (tokens.size() > 1) ? (tokens.size() - 1) * gap : 0;
        float available = width - total_gap;

        float fixed_sum = 0;
        float fr_sum = 0;
        std::vector<float> sizes(tokens.size(), 0.0f);
        std::vector<bool> is_fr(tokens.size(), false);

        for (size_t i = 0; i < tokens.size(); ++i) {
            std::string t = tokens[i];
            if (t.find("fr") != std::string::npos) {
                try { fr_sum += std::stof(t.substr(0, t.size() - 2)); }
                catch (...) {}
                is_fr[i] = true;
            }
            else if (t.find("px") != std::string::npos) {
                try { sizes[i] = std::stof(t.substr(0, t.size() - 2)); }
                catch (...) {}
                fixed_sum += sizes[i];
            }
            else if (t == "auto") {
                sizes[i] = 100.0f; // Базовый минимум для auto
                fixed_sum += 100.0f;
            }
            else if (t.find("minmax(") == 0) {
                if (t.find("1fr") != std::string::npos) {
                    fr_sum += 1.0f; is_fr[i] = true;
                }
                else {
                    sizes[i] = 150.0f; fixed_sum += 150.0f;
                }
            }
        }

        float fr_size = (fr_sum > 0) ? std::max(0.0f, available - fixed_sum) / fr_sum : 0;

        for (size_t i = 0; i < tokens.size(); ++i) {
            if (is_fr[i]) {
                std::string t = tokens[i];
                float val = 1.0f;
                try { val = std::stof(t.substr(0, t.find("fr"))); }
                catch (...) {}
                sizes[i] = val * fr_size;
            }
        }

        return sizes;
    }

    static size_t find_close_paren(const std::string& s, size_t open) {
        int depth = 1;
        for (size_t i = open + 1; i < s.size(); ++i) {
            if (s[i] == '(') depth++;
            else if (s[i] == ')') { depth--; if (depth == 0) return i; }
        }
        return std::string::npos;
    }

    static std::string trim(const std::string& s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return "";
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    static std::string expand_repeat(const std::string& tmpl, int container_width) {
        std::string result;
        size_t pos = 0;
        while (pos < tmpl.size()) {
            size_t rep = tmpl.find("repeat(", pos);
            if (rep == std::string::npos) {
                result += tmpl.substr(pos);
                break;
            }
            result += tmpl.substr(pos, rep - pos);
            size_t end = find_close_paren(tmpl, rep + 6);
            if (end == std::string::npos) break;

            std::string inner = tmpl.substr(rep + 7, end - rep - 7);
            size_t comma = inner.find(',');
            if (comma != std::string::npos) {
                std::string count_str = trim(inner.substr(0, comma));
                std::string track_str = trim(inner.substr(comma + 1));

                int count = 1;
                if (count_str == "auto-fill" || count_str == "auto-fit") {
                    count = 4; // Заглушка
                }
                else {
                    try { count = std::stoi(count_str); }
                    catch (...) {}
                }

                for (int i = 0; i < count; ++i) {
                    result += track_str + " ";
                }
            }
            pos = end + 1;
        }
        return result;
    }
};