#pragma once
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include "./html_parser/headers/dom.h"
#include "./css_parser/headers/css_dom.h"

// ============================================================
//  Specificity (a, b, c):
//    a = #id
//    b = .class + [attr] + :pseudo-class  (кроме :where)
//    c = tag + ::pseudo-element
//  :where() обнуляет свой вклад.
// ============================================================
struct Specificity {
    uint32_t a = 0, b = 0, c = 0;

    bool operator<(const Specificity& o) const {
        if (a != o.a) return a < o.a;
        if (b != o.b) return b < o.b;
        return c < o.c;
    }
};

namespace css_util {

    // Разбивает class-строку на слова, не аллоцируя.
    inline bool has_class(const DOMNode* node, std::string_view cls) {
        auto it = node->attributes.find("class");
        if (it == node->attributes.end()) return false;
        const std::string& s = it->second;
        size_t i = 0, n = s.size();
        while (i < n) {
            while (i < n && std::isspace((unsigned char)s[i])) ++i;
            size_t start = i;
            while (i < n && !std::isspace((unsigned char)s[i])) ++i;
            if (i - start == cls.size() &&
                std::memcmp(s.data() + start, cls.data(), cls.size()) == 0) return true;
        }
        return false;
    }

    inline const std::string* get_attr(const DOMNode* node, const std::string& name) {
        auto it = node->attributes.find(name);
        return it == node->attributes.end() ? nullptr : &it->second;
    }

    inline bool attr_present(const DOMNode* node, const std::string& name) {
        return node->attributes.find(name) != node->attributes.end();
    }
    
    // ------- attribute operators -------
    inline bool attr_eq(const std::string& have, const std::string& want) { return have == want; }
    inline bool attr_includes(const std::string& have, const std::string& want) {
        if (want.empty()) return false;
        size_t i = 0, n = have.size();
        while (i < n) {
            while (i < n && std::isspace((unsigned char)have[i])) ++i;
            size_t s = i;
            while (i < n && !std::isspace((unsigned char)have[i])) ++i;
            if (i - s == want.size() &&
                std::memcmp(have.data() + s, want.data(), want.size()) == 0) return true;
        }
        return false;
    }
    inline bool attr_dash(const std::string& have, const std::string& want) {
        if (have.size() < want.size()) return false;
        if (have.compare(0, want.size(), want) != 0) return false;
        return have.size() == want.size() || have[want.size()] == '-';
    }
    inline bool attr_prefix(const std::string& h, const std::string& w) {
        return !w.empty() && h.size() >= w.size() && h.compare(0, w.size(), w) == 0;
    }
    inline bool attr_suffix(const std::string& h, const std::string& w) {
        return !w.empty() && h.size() >= w.size() &&
            h.compare(h.size() - w.size(), w.size(), w) == 0;
    }
    inline bool attr_substr(const std::string& h, const std::string& w) {
        return !w.empty() && h.find(w) != std::string::npos;
    }

    // ------- pseudo-class helpers (position in parent) -------
    inline uint32_t child_index(const DOMNode* node) {
        if (!node || !node->parent) return 1;
        uint32_t i = 1;
        for (DOMNode* c : node->parent->children) {
            if (c == node) return i;
            if (c->type == NodeType::Element) ++i;
        }
        return i;
    }

    inline uint32_t element_child_count(const DOMNode* node) {
        if (!node || !node->parent) return 1;
        uint32_t n = 0;
        for (DOMNode* c : node->parent->children) if (c->type == NodeType::Element) ++n;
        return n;
    }

    // Парсер an+b для :nth-child(2n+1), odd, even, 3
    inline bool parse_anb(const std::string& s, int& a, int& b) {
        std::string t;
        for (char c : s) if (!std::isspace((unsigned char)c)) t += (char)std::tolower((unsigned char)c);
        if (t.empty()) return false;
        if (t == "odd") { a = 2; b = 1; return true; }
        if (t == "even") { a = 2; b = 0; return true; }
        size_t npos = t.find('n');
        if (npos == std::string::npos) {
            char* end = nullptr;
            long v = std::strtol(t.c_str(), &end, 10);
            if (end == t.c_str() || *end != '\0') return false;
            a = 0; b = (int)v; return true;
        }
        std::string as = t.substr(0, npos);
        if (as.empty() || as == "+") a = 1;
        else if (as == "-") a = -1;
        else {
            char* end = nullptr;
            long v = std::strtol(as.c_str(), &end, 10);
            if (end == as.c_str() || *end != '\0') return false;
            a = (int)v;
        }
        std::string bs = t.substr(npos + 1);
        if (bs.empty()) { b = 0; return true; }
        char* end = nullptr;
        long v = std::strtol(bs.c_str(), &end, 10);
        if (end == bs.c_str() || *end != '\0') return false;
        b = (int)v;
        return true;
    }
    inline bool nth_match(int idx, int a, int b) {
        if (a == 0) return idx == b;
        int k = idx - b;
        if (a > 0) return k >= 0 && k % a == 0;
        return k <= 0 && (-k) % (-a) == 0;
    }

} // namespace css_util

// ============================================================
//  Основной matcher
// ============================================================
class SelectorMatcher {
public:
    // Right-to-left matching of complex selector.
    static bool match_complex(DOMNode* node, const ComplexSelector& complex) {
        if (!node || complex.compounds.empty()) return false;
        if (node->type != NodeType::Element) return false;

        const int last = (int)complex.compounds.size() - 1;
        if (!match_compound(node, complex.compounds[last])) return false;

        return match_left_of(node, complex, last - 1);
    }

    // Считаем специфичность один раз, кэшируется вызывающим.
    static Specificity compute_specificity(const ComplexSelector& cs) {
        Specificity s{};
        for (const auto& comp : cs.compounds) {
            for (const auto& p : comp.parts) {
                using K = SimpleSelector::Kind;
                switch (p.kind) {
                case K::Id:            s.a += 1; break;
                case K::Class:         s.b += 1; break;
                case K::Attribute:     s.b += 1; break;
                case K::PseudoClass:
                    // :where() обнуляет вклад
                    if (p.name == "where") break;
                    if (p.name == "is" || p.name == "not" || p.name == "has") {
                        // грубая аппроксимация — берём вклад только этих псевдо
                        s.b += 1;
                    }
                    else s.b += 1;
                    break;
                case K::PseudoElement: s.c += 1; break;
                case K::Tag:           s.c += 1; break;
                case K::Universal:     break;
                }
            }
        }
        return s;
    }

private:
    // Рекурсивный обход «левой части» селектора справа налево.
    static bool match_left_of(DOMNode* node, const ComplexSelector& cs, int comp_idx) {
        if (comp_idx < 0) return true;

        const Combinator comb = cs.compounds[comp_idx + 1].combinator;
        const CompoundSelector& target = cs.compounds[comp_idx];

        switch (comb) {
        case Combinator::Child: {
            DOMNode* p = node->parent;
            if (!p || p->type != NodeType::Element) return false;
            if (!match_compound(p, target)) return false;
            return match_left_of(p, cs, comp_idx - 1);
        }
        case Combinator::Descendant: {
            for (DOMNode* a = node->parent; a; a = a->parent) {
                if (a->type != NodeType::Element) continue;
                if (match_compound(a, target) && match_left_of(a, cs, comp_idx - 1))
                    return true;
            }
            return false;
        }
        case Combinator::AdjacentSibling: {
            DOMNode* prev = prev_element_sibling(node);
            if (!prev) return false;
            if (!match_compound(prev, target)) return false;
            return match_left_of(prev, cs, comp_idx - 1);
        }
        case Combinator::GeneralSibling: {
            for (DOMNode* a = prev_element_sibling(node); a;
                a = prev_element_sibling(a)) {
                if (match_compound(a, target) && match_left_of(a, cs, comp_idx - 1))
                    return true;
            }
            return false;
        }
        case Combinator::None:
        default:
            return false;
        }
    }

    static DOMNode* prev_element_sibling(DOMNode* node) {
        if (!node || !node->parent) return nullptr;
        DOMNode* prev = nullptr;
        for (DOMNode* c : node->parent->children) {
            if (c == node) return prev;
            if (c->type == NodeType::Element) prev = c;
        }
        return nullptr;
    }

    static bool match_compound(DOMNode* node, const CompoundSelector& compound) {
        if (!node || node->type != NodeType::Element) return false;
        for (const auto& s : compound.parts) {
            if (!match_simple(node, s)) return false;
        }
        return true;
    }

    static bool match_simple(DOMNode* node, const SimpleSelector& s) {
        using K = SimpleSelector::Kind;
        switch (s.kind) {
        case K::Universal:
            return true;

        case K::Tag:
            // HTML-теги нечувствительны к регистру; для SVG — нет,
            // но у нас DOM и так хранит lowercase.
            return node->tag_name == s.name;

        case K::Class:
            return css_util::has_class(node, s.name);

        case K::Id: {
            auto it = node->attributes.find("id");
            return it != node->attributes.end() && it->second == s.name;
        }

        case K::Attribute:
            return match_attr(node, s);

        case K::PseudoClass:
            return match_pseudo(node, s);

        case K::PseudoElement:
            // Псевдоэлементы не матчатся на самом DOM-узле;
            // их обрабатывает layout (::before/::after — генерируемый контент).
            return false;
        }
        return false;
    }

    static bool match_attr(DOMNode* node, const SimpleSelector& s) {
        const std::string* have = css_util::get_attr(node, s.name);
        if (!have) return false;
        if (s.op.empty()) return true;    // [attr]
        if (s.op == "=")  return css_util::attr_eq(*have, s.arg);
        if (s.op == "~=") return css_util::attr_includes(*have, s.arg);
        if (s.op == "|=") return css_util::attr_dash(*have, s.arg);
        if (s.op == "^=") return css_util::attr_prefix(*have, s.arg);
        if (s.op == "$=") return css_util::attr_suffix(*have, s.arg);
        if (s.op == "*=") return css_util::attr_substr(*have, s.arg);
        return false;
    }

    static bool match_pseudo(DOMNode* node, const SimpleSelector& s) {
        const std::string& n = s.name;

        // --- структурные ---
        if (n == "first-child")  return css_util::child_index(node) == 1;
        if (n == "last-child")   return css_util::child_index(node) == css_util::element_child_count(node);
        if (n == "only-child")   return css_util::element_child_count(node) == 1;
        if (n == "empty") {
            for (DOMNode* c : node->children) {
                if (c->type == NodeType::Element) return false;
                if (c->type == NodeType::Text && !c->text_content.empty()) return false;
            }
            return true;
        }
        if (n == "root") return node->parent == nullptr || node->parent->type != NodeType::Element;

        if (n == "nth-child" || n == "nth-last-child") {
            int a = 0, b = 0;
            if (!css_util::parse_anb(s.arg, a, b)) return false;
            uint32_t idx = css_util::child_index(node);
            if (n == "nth-last-child") {
                idx = css_util::element_child_count(node) + 1 - idx;
            }
            return css_util::nth_match((int)idx, a, b);
        }
        if (n == "first-of-type" || n == "last-of-type" ||
            n == "nth-of-type" || n == "nth-last-of-type") {
            // Считаем позицию только среди элементов с тем же tag_name
            uint32_t idx = 1, total = 0;
            DOMNode* p = node->parent;
            if (!p) return false;
            for (DOMNode* c : p->children) {
                if (c->type != NodeType::Element || c->tag_name != node->tag_name) continue;
                ++total;
                if (c == node) { /* idx оставляем */ }
                else if (total < idx || true) { if (c != node) {} }
            }
            // Пересчёт честный
            idx = 0; uint32_t seen = 0;
            for (DOMNode* c : p->children) {
                if (c->type != NodeType::Element || c->tag_name != node->tag_name) continue;
                ++seen;
                if (c == node) { idx = seen; break; }
            }
            if (n == "first-of-type") return idx == 1;
            if (n == "last-of-type")  return idx == total;
            int a = 0, b = 0;
            if (!css_util::parse_anb(s.arg, a, b)) return false;
            uint32_t pos = (n == "nth-of-type") ? idx : (total + 1 - idx);
            return css_util::nth_match((int)pos, a, b);
        }

        // --- логические ---
        if (n == "not" || n == "is" || n == "where" || n == "matches" || n == "any") {
            std::string_view arg = s.arg;             // ← было std::string
            size_t start = 0;
            bool matched = false;

            while (start <= arg.size()) {
                size_t comma = arg.find(',', start);
                std::string_view part = (comma == std::string_view::npos)
                    ? arg.substr(start)
                    : arg.substr(start, comma - start);

                // trim по краям — без аллокаций
                while (!part.empty() && std::isspace((unsigned char)part.front()))
                    part.remove_prefix(1);
                while (!part.empty() && std::isspace((unsigned char)part.back()))
                    part.remove_suffix(1);

                if (match_simple_selector_text(node, part)) { matched = true; break; }
                if (comma == std::string_view::npos) break;
                start = comma + 1;
            }

            if (n == "not") return !matched;
            return matched;
        }

        // --- state-based: в статичном дереве НЕ матчатся ---
        if (n == "hover" || n == "focus" || n == "active" ||
            n == "visited" || n == "link" || n == "target") return false;

        // --- link-related: <a href> / <area href> ---
        if (n == "any-link" || n == "link") {
            return (node->tag_name == "a" || node->tag_name == "area") &&
                css_util::attr_present(node, "href");
        }

        // --- прочие молча пропускаем как «не совпадает» ---
        return false;
    }

    // Упрощённый матчер одного простого селектора по строке (для :not/:is).
    // Поддерживает tag, .class, #id, [attr], :pseudo.
    static bool match_simple_selector_text(DOMNode* node, std::string_view text) {
        size_t i = 0, n = text.size();
        while (i < n) {
            char c = text[i];

            if (c == '*') { ++i; continue; }

            if (c == '.') {
                ++i; size_t s = i;
                while (i < n && (std::isalnum((unsigned char)text[i]) ||
                    text[i] == '-' || text[i] == '_')) ++i;
                if (i == s) return false;
                // has_class уже принимает string_view — без копии
                if (!css_util::has_class(node, text.substr(s, i - s))) return false;
            }
            else if (c == '#') {
                ++i; size_t s = i;
                while (i < n && (std::isalnum((unsigned char)text[i]) ||
                    text[i] == '-' || text[i] == '_')) ++i;
                auto it = node->attributes.find("id");
                if (it == node->attributes.end()) return false;
                // сравнение std::string и string_view — без аллокации
                if (it->second != text.substr(s, i - s)) return false;
            }
            else if (c == ':') {
                ++i; if (i < n && text[i] == ':') ++i;
                size_t s = i;
                while (i < n && (std::isalnum((unsigned char)text[i]) ||
                    text[i] == '-')) ++i;

                SimpleSelector ps;
                ps.kind = SimpleSelector::Kind::PseudoClass;
                ps.name.assign(text.data() + s, i - s);   // одна неизбежная аллокация:
                // match_pseudo сравнивает name
                // со std::string литералами
                if (!match_pseudo(node, ps)) return false;
            }
            else if (c == '[') {
                size_t close = text.find(']', i);
                if (close == std::string_view::npos) return false;
                std::string_view body = text.substr(i + 1, close - i - 1);

                size_t eq = body.find('=');
                std::string_view name = (eq == std::string_view::npos)
                    ? body : body.substr(0, eq);
                while (!name.empty() && std::isspace((unsigned char)name.back()))
                    name.remove_suffix(1);

                std::string_view val;
                if (eq != std::string_view::npos) {
                    val = body.substr(eq + 1);
                    if (val.size() >= 2 && (val.front() == '"' || val.front() == '\''))
                        val = val.substr(1, val.size() - 2);
                }

                // get_attr принимает const std::string& — единственная копия имени,
                // но только при встрече с [attr]. В горячем цикле :is/:not(.x) сюда
                // не попадаем.
                auto it = node->attributes.find(std::string(name));
                if (it == node->attributes.end()) return false;
                if (eq != std::string_view::npos && it->second != val) return false;
                i = close + 1;
            }
            else if (std::isalpha((unsigned char)c)) {
                size_t s = i;
                while (i < n && (std::isalnum((unsigned char)text[i]) ||
                    text[i] == '-' || text[i] == '_')) ++i;
                // сравнение std::string ↔ string_view
                if (std::string_view(node->tag_name.data(), node->tag_name.size())
                    != text.substr(s, i - s)) return false;
            }
            else return false;
        }
        return true;
    }
};