#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <iostream>

// ============================================================
//  Комбинаторы между compound-селекторами
// ============================================================
enum class Combinator {
    None,             // начало complex selector
    Descendant,       // " " (или перевод строки)
    Child,            // >
    AdjacentSibling,  // +
    GeneralSibling    // ~
};

// ============================================================
//  Простейший селектор (одна единица внутри compound)
// ============================================================
struct SimpleSelector {
    enum class Kind {
        Universal,     // *
        Tag,           // div
        Class,         // .cls
        Id,            // #id
        Attribute,     // [attr], [attr=val], [attr~=val], ...
        PseudoClass,   // :hover, :nth-child(...)
        PseudoElement  // ::before, :before
    };
    Kind kind = Kind::Universal;
    std::string name;         // tag/class/id/attr/pseudo
    std::string op;           // для attribute: =, ~=, |=, ^=, $=, *=
    std::string arg;          // для attribute — значение; для pseudo — аргумент
    bool has_arg = false;     // для функциональных псевдо-классов
};

// ============================================================
//  Compound (цепочка простейших без комбинаторов): div.cls#id[attr]
// ============================================================
struct CompoundSelector {
    Combinator combinator = Combinator::None; // комбинатор ПЕРЕД этим compound
    std::vector<SimpleSelector> parts;
};

// ============================================================
//  Complex (последовательность compound через комбинаторы): div > p.foo
// ============================================================
struct ComplexSelector {
    std::vector<CompoundSelector> compounds;
};

// ============================================================
//  Декларация (свойство: значение [!important])
// ============================================================
struct Declaration {
    std::string property;
    std::string value;
    bool important = false;
    bool is_custom_property = false; // --foo
};

struct AtRule; // forward

// ============================================================
//  Qualified-правило: selectors { declarations | @nested }
// ============================================================
struct CSSRule {
    std::vector<ComplexSelector> selectors;
    std::vector<Declaration> declarations;
    std::vector<AtRule> nested_at_rules;
};

// ============================================================
//  At-правило: @media { ... }, @import "..."; и т.п.
// ============================================================
struct AtRule {
    std::string name;                       // "media", "font-face", ...
    std::string prelude;                    // "screen and (max-width: 600px)"
    std::vector<Declaration> declarations;  // для @font-face, @page, @property
    std::vector<CSSRule> rules;             // для @media, @supports, @keyframes
    std::vector<AtRule> nested_at_rules;    // вложенные @-правила
};

// ============================================================
//  Корень
// ============================================================
struct StyleSheet {
    std::vector<CSSRule> rules;
    std::vector<AtRule> at_rules;
};

// ============================================================
//  Отладочная печать
// ============================================================
namespace css_debug {

    inline const char* combinator_str(Combinator c) {
        switch (c) {
        case Combinator::Descendant:      return " ";
        case Combinator::Child:           return " > ";
        case Combinator::AdjacentSibling: return " + ";
        case Combinator::GeneralSibling:  return " ~ ";
        case Combinator::None:            return "";
        }
        return "";
    }

    inline void print_selector(const ComplexSelector& cs, std::ostream& os = std::cout) {
        bool first = true;
        for (const auto& comp : cs.compounds) {
            if (!first) os << combinator_str(comp.combinator);
            first = false;
            bool first_simple = true;
            for (const auto& s : comp.parts) {
                using K = SimpleSelector::Kind;
                switch (s.kind) {
                case K::Universal:     os << "*"; break;
                case K::Tag:           if (!first_simple) os << s.name; else os << s.name; break;
                case K::Class:         os << "." << s.name; break;
                case K::Id:            os << "#" << s.name; break;
                case K::Attribute:
                    os << "[" << s.name;
                    if (!s.op.empty()) os << s.op << "\"" << s.arg << "\"";
                    os << "]";
                    break;
                case K::PseudoClass:
                    os << ":" << s.name;
                    if (s.has_arg) os << "(" << s.arg << ")";
                    break;
                case K::PseudoElement:
                    os << "::" << s.name;
                    if (s.has_arg) os << "(" << s.arg << ")";
                    break;
                }
                first_simple = false;
            }
        }
    }

    inline void print_declaration(const Declaration& d, const std::string& indent, std::ostream& os = std::cout) {
        os << indent << d.property << ": " << d.value;
        if (d.important) os << " !important";
        os << ";\n";
    }

    inline void print_rule(const CSSRule& r, const std::string& indent, std::ostream& os = std::cout);

    inline void print_at_rule(const AtRule& at, const std::string& indent, std::ostream& os = std::cout) {
        os << indent << "@" << at.name;
        if (!at.prelude.empty()) os << " " << at.prelude;
        if (at.declarations.empty() && at.rules.empty() && at.nested_at_rules.empty()) {
            os << ";\n";
            return;
        }
        os << " {\n";
        for (const auto& d : at.declarations) print_declaration(d, indent + "  ", os);
        for (const auto& r : at.rules)      print_rule(r, indent + "  ", os);
        for (const auto& n : at.nested_at_rules) print_at_rule(n, indent + "  ", os);
        os << indent << "}\n";
    }

    inline void print_rule(const CSSRule& r, const std::string& indent, std::ostream& os) {
        os << indent;
        bool first = true;
        for (const auto& s : r.selectors) {
            if (!first) os << ", ";
            first = false;
            print_selector(s, os);
        }
        os << " {\n";
        for (const auto& d : r.declarations) print_declaration(d, indent + "  ", os);
        for (const auto& n : r.nested_at_rules) print_at_rule(n, indent + "  ", os);
        os << indent << "}\n";
    }

    inline void print_sheet(const StyleSheet& s, std::ostream& os = std::cout) {
        for (const auto& r : s.rules)     print_rule(r, "", os);
        for (const auto& at : s.at_rules) print_at_rule(at, "", os);
    }

} // namespace css_debug