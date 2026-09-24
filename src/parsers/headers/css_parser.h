#pragma once
#include <string>
#include <vector>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include "arena.h"
#include "css_dom.h"

class CSSParser {
public:
    explicit CSSParser(ArenaAllocator& arena) : arena_(arena) {}

    // Точка входа. Объект переиспользуем между вызовами.
    StyleSheet* parse(const std::string& css_text) {
        input_ = css_text;
        pos_ = 0;
        errors_.clear();

        StyleSheet* sheet = arena_.Alloc<StyleSheet>();

        while (true) {
            skip_ws_and_comments();
            if (eof()) break;

            if (peek() == '@') {
                sheet->at_rules.push_back(parse_at_rule());
            }
            else if (peek() == '}') {
                report_error("Stray '}'");
                ++pos_;
            }
            else {
                CSSRule r = parse_qualified_rule();
                if (!r.selectors.empty() || !r.declarations.empty() ||
                    !r.nested_at_rules.empty()) {
                    sheet->rules.push_back(std::move(r));
                }
            }
        }
        return sheet;
    }

    const std::vector<std::string>& errors() const { return errors_; }
    bool has_errors() const { return !errors_.empty(); }

private:
    // =================================================================
    //  Состояние
    // =================================================================
    ArenaAllocator& arena_;
    std::string input_;
    size_t pos_ = 0;
    std::vector<std::string> errors_;

    // =================================================================
    //  Базовые утилиты
    // =================================================================
    bool eof() const { return pos_ >= input_.size(); }
    char peek(size_t off = 0) const {
        return (pos_ + off < input_.size()) ? input_[pos_ + off] : '\0';
    }
    char advance() { return (pos_ < input_.size()) ? input_[pos_++] : '\0'; }
    bool match(char c) { if (!eof() && input_[pos_] == c) { ++pos_; return true; } return false; }

    static bool is_ws(char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
    }
    static bool is_ident_start(char c) {
        return std::isalpha(static_cast<unsigned char>(c)) ||
            c == '_' || c == '-' ||
            static_cast<unsigned char>(c) >= 0x80;
    }
    static bool is_ident_char(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) ||
            c == '-' || c == '_' ||
            static_cast<unsigned char>(c) >= 0x80;
    }

    void report_error(std::string msg) {
        errors_.push_back("pos " + std::to_string(pos_) + ": " + std::move(msg));
    }

    void skip_ws() { while (!eof() && is_ws(peek())) ++pos_; }

    void skip_ws_and_comments() {
        while (!eof()) {
            if (is_ws(peek())) { ++pos_; continue; }
            if (peek() == '/' && peek(1) == '*') {
                pos_ += 2;
                while (!eof() && !(peek() == '*' && peek(1) == '/')) ++pos_;
                if (!eof()) pos_ += 2;
                else report_error("Unterminated comment");
                continue;
            }
            break;
        }
    }

    // =================================================================
    //  Идентификаторы с CSS escape-последовательностями (\XX, \XXXXXX)
    // =================================================================
    std::string parse_identifier() {
        std::string out;
        while (!eof()) {
            char c = peek();
            if (c == '\\' && pos_ + 1 < input_.size()) {
                ++pos_;
                uint32_t cp = 0;
                int digits = 0;
                while (!eof() && digits < 6 &&
                    std::isxdigit(static_cast<unsigned char>(peek()))) {
                    char h = peek();
                    int d = (h >= '0' && h <= '9') ? h - '0'
                        : (h >= 'a' && h <= 'f') ? 10 + h - 'a'
                        : 10 + h - 'A';
                    cp = cp * 16 + static_cast<uint32_t>(d);
                    ++pos_; ++digits;
                }
                if (digits == 0) cp = static_cast<unsigned char>(advance());
                else if (is_ws(peek())) ++pos_; // один пробел после escape
                append_utf8(out, cp);
            }
            else if (is_ident_char(c)) {
                out += c; ++pos_;
            }
            else {
                break;
            }
        }
        return out;
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out += "\xEF\xBF\xBD"; // U+FFFD
            return;
        }
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        }
        else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    // =================================================================
    //  Строки ("..." и '...')
    // =================================================================
    std::string parse_string() {
        char quote = advance();
        std::string out;
        while (!eof()) {
            char c = peek();
            if (c == quote) { ++pos_; return out; }
            if (c == '\\') {
                ++pos_;
                if (eof()) break;
                char n = peek();
                if (n == '\n' || n == '\r' || n == '\f') { // line continuation
                    ++pos_;
                    if (n == '\r' && peek() == '\n') ++pos_;
                }
                else {
                    out += n; ++pos_;
                }
            }
            else {
                out += c; ++pos_;
            }
        }
        report_error("Unterminated string");
        return out;
    }

    // =================================================================
    //  Значение декларации: до ';' / '}' (с учётом вложенности скобок)
    //  Корректно обрабатывает строки, url(), calc(), !important.
    // =================================================================
    std::string parse_value_until(char stop1, char stop2, bool& important_flag) {
        std::string out;
        int paren_depth = 0;
        important_flag = false;

        while (!eof()) {
            char c = peek();

            // Комментарии внутри значения выкидываем
            if (c == '/' && peek(1) == '*') {
                pos_ += 2;
                while (!eof() && !(peek() == '*' && peek(1) == '/')) ++pos_;
                if (!eof()) pos_ += 2;
                continue;
            }

            if (paren_depth == 0) {
                if (c == stop1 || c == stop2) break;

                if (c == '!') {
                    size_t save = pos_;
                    ++pos_;
                    skip_ws();
                    std::string ident = parse_identifier();
                    for (char& ch : ident) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (ident == "important") {
                        important_flag = true;
                        skip_ws();
                        while (!out.empty() && is_ws(out.back())) out.pop_back();
                        break;
                    }
                    pos_ = save;
                    out += c; ++pos_;
                    continue;
                }
            }

            if (c == '"' || c == '\'') {
                char q = c;
                out += q; ++pos_;
                while (!eof() && peek() != q) {
                    char cc = peek();
                    if (cc == '\\' && pos_ + 1 < input_.size()) {
                        out += cc; ++pos_;
                        out += peek(); ++pos_;
                    }
                    else {
                        out += cc; ++pos_;
                    }
                }
                if (!eof()) { out += q; ++pos_; }
                continue;
            }

            if (c == '(') { ++paren_depth; out += c; ++pos_; continue; }
            if (c == ')') { if (paren_depth > 0) --paren_depth; out += c; ++pos_; continue; }

            if ((c == '{' || c == '}') && paren_depth == 0) break;

            out += c; ++pos_;
        }

        // trim
        size_t b = 0, e = out.size();
        while (b < e && is_ws(out[b])) ++b;
        while (e > b && is_ws(out[e - 1])) --e;
        return out.substr(b, e - b);
    }

    // =================================================================
    //  Qualified rule
    // =================================================================
    CSSRule parse_qualified_rule() {
        CSSRule rule;
        rule.selectors = parse_selector_list();
        skip_ws_and_comments();

        if (!match('{')) {
            report_error("Expected '{' after selectors");
            while (!eof() && peek() != '{' && peek() != '}') ++pos_;
            if (peek() == '{') ++pos_;
        }
        parse_block_contents(rule.declarations, rule.nested_at_rules);
        return rule;
    }

    // =================================================================
    //  Selector list: complex (, complex)*
    // =================================================================
    std::vector<ComplexSelector> parse_selector_list() {
        std::vector<ComplexSelector> list;
        while (true) {
            skip_ws_and_comments();
            ComplexSelector sel = parse_complex_selector();
            if (!sel.compounds.empty()) list.push_back(std::move(sel));
            skip_ws_and_comments();
            if (!match(',')) break;
        }
        return list;
    }

    ComplexSelector parse_complex_selector() {
        ComplexSelector cs;
        Combinator pending = Combinator::None;
        bool saw_space = false;

        while (!eof()) {
            char c = peek();
            if (c == '{' || c == ',') break;

            if (is_ws(c)) {
                skip_ws_and_comments();
                saw_space = true;
                continue;
            }

            if (c == '>' || c == '+' || c == '~') {
                pending = (c == '>') ? Combinator::Child
                    : (c == '+') ? Combinator::AdjacentSibling
                    : Combinator::GeneralSibling;
                ++pos_;
                skip_ws_and_comments();
                saw_space = false;
                continue;
            }

            CompoundSelector compound;
            if (!cs.compounds.empty()) {
                compound.combinator = (pending != Combinator::None)
                    ? pending
                    : (saw_space ? Combinator::Descendant
                        : Combinator::Descendant);
            }
            pending = Combinator::None;
            saw_space = false;

            parse_compound_selector(compound);
            if (compound.parts.empty()) break;
            cs.compounds.push_back(std::move(compound));
        }
        return cs;
    }

    void parse_compound_selector(CompoundSelector& compound) {
        size_t start_pos = pos_;
        while (!eof()) {
            char c = peek();
            if (is_ws(c) || c == '{' || c == ',' || c == '>' || c == '+' || c == '~') break;

            SimpleSelector s;
            if (c == '*') {
                s.kind = SimpleSelector::Kind::Universal;
                ++pos_;
            }
            else if (c == '.') {
                s.kind = SimpleSelector::Kind::Class;
                ++pos_;
                s.name = parse_identifier();
                if (s.name.empty()) { report_error("Expected class name after '.'"); break; }
            }
            else if (c == '#') {
                s.kind = SimpleSelector::Kind::Id;
                ++pos_;
                s.name = parse_identifier();
                if (s.name.empty()) { report_error("Expected id after '#'"); break; }
            }
            else if (c == '[') {
                if (!parse_attribute_selector(s)) break;
            }
            else if (c == ':') {
                if (!parse_pseudo(s)) break;
            }
            else if (is_ident_start(c)) {
                s.kind = SimpleSelector::Kind::Tag;
                s.name = parse_identifier();
            }
            else {
                break;
            }
            compound.parts.push_back(std::move(s));
        }
        if (pos_ == start_pos && !eof()) ++pos_; // гарантия прогресса
    }

    bool parse_attribute_selector(SimpleSelector& s) {
        s.kind = SimpleSelector::Kind::Attribute;
        ++pos_; // '['
        skip_ws();
        s.name = parse_identifier();
        skip_ws();

        char c = peek();
        if (c == ']') { ++pos_; return true; }

        if (c == '=') { s.op = "="; ++pos_; }
        else if ((c == '~' || c == '|' || c == '^' || c == '$' || c == '*') && peek(1) == '=') {
            s.op += c; s.op += '=';
            pos_ += 2;
        }
        else {
            report_error("Invalid attribute operator");
            while (!eof() && peek() != ']') ++pos_;
            if (!eof()) ++pos_;
            return true;
        }

        skip_ws();
        if (peek() == '"' || peek() == '\'') s.arg = parse_string();
        else                                 s.arg = parse_identifier();

        skip_ws();
        if (!match(']')) report_error("Expected ']' in attribute selector");
        return true;
    }

    bool parse_pseudo(SimpleSelector& s) {
        ++pos_; // ':'
        bool double_colon = false;
        if (peek() == ':') { double_colon = true; ++pos_; }

        s.name = parse_identifier();
        if (s.name.empty()) { report_error("Expected pseudo name after ':'"); return false; }
        s.kind = double_colon ? SimpleSelector::Kind::PseudoElement
            : SimpleSelector::Kind::PseudoClass;

        if (peek() == '(') {
            s.has_arg = true;
            ++pos_;
            int depth = 1;
            std::string arg;
            while (!eof() && depth > 0) {
                char c = peek();
                if (c == '(') ++depth;
                else if (c == ')') {
                    --depth;
                    if (depth == 0) { ++pos_; break; }
                }
                arg += c; ++pos_;
            }
            size_t b = 0, e = arg.size();
            while (b < e && is_ws(arg[b])) ++b;
            while (e > b && is_ws(arg[e - 1])) --e;
            s.arg = arg.substr(b, e - b);
        }
        return true;
    }

    // =================================================================
    //  Блок { ... }: declarations + вложенные @-правила
    // =================================================================
    void parse_block_contents(std::vector<Declaration>& decls,
        std::vector<AtRule>& nested) {
        while (!eof()) {
            skip_ws_and_comments();
            if (eof()) { report_error("Unexpected EOF inside block"); return; }
            if (peek() == '}') { ++pos_; return; }

            if (peek() == '@') {
                nested.push_back(parse_at_rule());
                continue;
            }

            size_t save = pos_;
            std::string prop = parse_identifier_or_custom_property();
            skip_ws();

            if (peek() == ':') {
                ++pos_;
                skip_ws();
                bool important = false;
                std::string value = parse_value_until(';', '}', important);

                if (!prop.empty()) {
                    Declaration d;
                    d.property = std::move(prop);
                    d.value = std::move(value);
                    d.important = important;
                    d.is_custom_property = (d.property.size() >= 2 &&
                        d.property[0] == '-' && d.property[1] == '-');
                    decls.push_back(std::move(d));
                }
                match(';');
            }
            else {
                // Не декларация — попытка вложенного стилевого правила (CSS Nesting).
                // Полноценно не поддерживаем — пропускаем до ';' или '}'.
                pos_ = save;
                report_error("Nested style rule not supported — skipped");
                int depth = 0;
                while (!eof()) {
                    char c = peek();
                    if (c == '{') ++depth;
                    else if (c == '}') {
                        if (depth == 0) break;
                        --depth;
                        ++pos_;
                        if (depth == 0) break;
                        continue;
                    }
                    else if (c == ';' && depth == 0) { ++pos_; break; }
                    ++pos_;
                }
            }
        }
    }

    std::string parse_identifier_or_custom_property() {
        if (peek() == '-' && peek(1) == '-') {
            std::string out;
            out += advance(); out += advance();
            out += parse_identifier();
            return out;
        }
        return parse_identifier();
    }

    // =================================================================
    //  At-rules
    // =================================================================
    AtRule parse_at_rule() {
        AtRule at;
        ++pos_; // '@'
        at.name = parse_identifier();
        for (char& c : at.name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        // Prelude: до '{' или ';' на нулевой глубине скобок
        {
            size_t start = pos_;
            int depth = 0;
            while (!eof()) {
                char c = peek();
                if (c == '(') ++depth;
                else if (c == ')') { if (depth > 0) --depth; }
                else if (depth == 0 && (c == '{' || c == ';')) break;
                ++pos_;
            }
            at.prelude = input_.substr(start, pos_ - start);
            size_t b = 0, e = at.prelude.size();
            while (b < e && is_ws(at.prelude[b])) ++b;
            while (e > b && is_ws(at.prelude[e - 1])) --e;
            at.prelude = at.prelude.substr(b, e - b);
        }

        if (match(';')) return at; // @import "..."; @charset "..."; @namespace ...;

        if (!match('{')) {
            report_error("Expected '{' or ';' after @" + at.name);
            return at;
        }

        // Тела разных at-rules
        static const char* decl_only[] = {
            "font-face", "page", "viewport", "counter-style", "property", "font-feature-values"
        };
        bool is_decl_body = false;
        for (const char* n : decl_only) if (at.name == n) { is_decl_body = true; break; }

        if (is_decl_body) {
            parse_block_contents(at.declarations, at.nested_at_rules);
        }
        else {
            while (!eof()) {
                skip_ws_and_comments();
                if (eof()) { report_error("Unexpected EOF inside @" + at.name); break; }
                if (peek() == '}') { ++pos_; break; }
                if (peek() == '@') at.nested_at_rules.push_back(parse_at_rule());
                else               at.rules.push_back(parse_qualified_rule());
            }
        }
        return at;
    }
};