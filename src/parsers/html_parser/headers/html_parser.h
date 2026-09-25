#pragma once
#include <string>
#include <vector>
#include <cctype>
#include <cstdint>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include "./arena_memory_allocator/headers/arena.h"
#include "dom.h"

class HTMLParser {
public:
    explicit HTMLParser(ArenaAllocator& arena) : arena_(arena) {}

    // Основная точка входа. Можно вызывать многократно на одном объекте.
    DOMNode* parse(const std::string& html) {
        reset(html);

        DOMNode* root = arena_.Alloc<DOMNode>();
        root->type = NodeType::Document;
        root->tag_name = "#document";
        stack_.push_back(root);

        while (pos_ < html_.size()) {
            char c = html_[pos_];
            process_char(c);
            ++pos_;
        }
        flush_text_buffer();
        return root;
    }

private:
    // ------------------------------------------------------------------
    //  Состояния токенизатора (по мотивам HTML5)
    // ------------------------------------------------------------------
    enum class State {
        // Текстовые режимы
        Data,
        RCDATA,
        RAWTEXT,
        ScriptData,
        PLAINTEXT,

        // Обработка тега
        TagOpen,
        EndTagOpen,
        TagName,

        // Обнаружение закрывающего тега внутри RCDATA/RAWTEXT/ScriptData
        RCDATALessThanSign,
        RCDATAEndTagOpen,
        RCDATAEndTagName,
        RAWTEXTLessThanSign,
        RAWTEXTEndTagOpen,
        RAWTEXTEndTagName,
        ScriptDataLessThanSign,
        ScriptDataEndTagOpen,
        ScriptDataEndTagName,

        // Атрибуты
        BeforeAttributeName,
        AttributeName,
        AfterAttributeName,
        BeforeAttributeValue,
        AttributeValueDoubleQuoted,
        AttributeValueSingleQuoted,
        AttributeValueUnquoted,
        AfterAttributeValueQuoted,
        SelfClosingStartTag,

        // Комментарии
        BogusComment,
        MarkupDeclarationOpen,
        CommentStart,
        CommentStartDash,
        Comment,
        CommentEndDash,
        CommentEnd,
        CommentEndBang,

        // DOCTYPE
        Doctype,
        BeforeDoctypeName,
        DoctypeName,
        AfterDoctypeName,
        BogusDoctype,

        // CDATA
        CdataSection,
        CdataSectionBracket,
        CdataSectionEnd,

        // Символьные ссылки
        CharacterReference,
        NumericCharacterReferenceStart,
        NumericCharacterReference,
        NumericCharacterReferenceEnd
    };

    // ------------------------------------------------------------------
    //  Данные парсера
    // ------------------------------------------------------------------
    ArenaAllocator& arena_;
    std::string html_;
    size_t pos_ = 0;
    State state_ = State::Data;
    State return_state_ = State::Data;

    std::string buffer_;
    std::string text_buffer_;
    std::string comment_buffer_;
    std::string doctype_buffer_;
    std::string char_ref_buffer_;

    std::string current_tag_name_;
    std::string current_attr_name_;
    std::string current_attr_value_;
    std::string last_start_tag_; // для RCDATA/RAWTEXT — имя открывающего тега

    std::vector<DOMNode*> stack_;
    DOMNode* current_node_ = nullptr;

    bool is_end_tag_ = false;
    bool is_self_closing_ = false;

    // ------------------------------------------------------------------
    //  Наборы тегов
    // ------------------------------------------------------------------
    static const std::unordered_set<std::string>& void_elements() {
        static const std::unordered_set<std::string> s = {
            "area", "base", "br", "col", "embed", "hr", "img", "input",
            "link", "meta", "param", "source", "track", "wbr",
            "basefont", "bgsound", "frame", "keygen"
        };
        return s;
    }
    static const std::unordered_set<std::string>& rcdata_elements() {
        static const std::unordered_set<std::string> s = { "title", "textarea" };
        return s;
    }
    static const std::unordered_set<std::string>& rawtext_elements() {
        static const std::unordered_set<std::string> s = {
            "style", "xmp", "iframe", "noembed", "noframes", "noscript"
        };
        return s;
    }

    // ------------------------------------------------------------------
    //  Именованные HTML-сущности (небольшая, но полезная выборка)
    // ------------------------------------------------------------------
    static const std::unordered_map<std::string, uint32_t>& named_entities() {
        static const std::unordered_map<std::string, uint32_t> m = {
            {"amp", '&'},   {"lt", '<'},   {"gt", '>'},   {"quot", '"'},
            {"apos", '\''}, {"nbsp", 0xA0},{"copy", 0xA9},{"reg", 0xAE},
            {"trade", 0x2122}, {"mdash", 0x2014}, {"ndash", 0x2013},
            {"hellip", 0x2026}, {"laquo", 0xAB}, {"raquo", 0xBB},
            {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"lsquo", 0x2018},
            {"rsquo", 0x2019}, {"times", 0xD7}, {"divide", 0xF7},
            {"deg", 0xB0}, {"plusmn", 0xB1}, {"middot", 0xB7},
            {"bull", 0x2022}, {"dagger", 0x2020}, {"sect", 0xA7},
            {"para", 0xB6}, {"euro", 0x20AC}, {"pound", 0xA3},
            {"yen", 0xA5}, {"cent", 0xA2}
        };
        return m;
    }

    // ------------------------------------------------------------------
    //  Сброс состояния
    // ------------------------------------------------------------------
    void reset(const std::string& html) {
        html_ = html;
        pos_ = 0;
        state_ = State::Data;
        return_state_ = State::Data;
        buffer_.clear();
        text_buffer_.clear();
        comment_buffer_.clear();
        doctype_buffer_.clear();
        char_ref_buffer_.clear();
        current_tag_name_.clear();
        current_attr_name_.clear();
        current_attr_value_.clear();
        last_start_tag_.clear();
        stack_.clear();
        current_node_ = nullptr;
        is_end_tag_ = false;
        is_self_closing_ = false;
    }

    // ------------------------------------------------------------------
    //  Утилиты
    // ------------------------------------------------------------------
    static bool is_ascii_alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
    static bool is_ascii_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
    static bool is_ascii_alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
    static bool is_html_space(char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
    }
    static char to_lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

    static bool is_attr_value_state(State s) {
        return s == State::AttributeValueDoubleQuoted
            || s == State::AttributeValueSingleQuoted
            || s == State::AttributeValueUnquoted;
    }

    static std::string encode_utf8(uint32_t cp) {
        std::string out;
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
        return out;
    }

    static uint32_t sanitize_codepoint(uint32_t cp) {
        // Замена недопустимых кодовых точек на U+FFFD
        if (cp == 0 || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF) ||
            (cp >= 0xFDD0 && cp <= 0xFDEF) ||
            (cp & 0xFFFE) == 0xFFFE) {
            return 0xFFFD;
        }
        return cp;
    }

    // ------------------------------------------------------------------
    //  Работа с DOM/стеком
    // ------------------------------------------------------------------
    void flush_text_buffer() {
        if (text_buffer_.empty()) return;
        DOMNode* text_node = arena_.Alloc<DOMNode>();
        text_node->type = NodeType::Text;
        text_node->text_content = std::move(text_buffer_);
        text_buffer_.clear();
        if (!stack_.empty()) stack_.back()->add_child(text_node);
    }

    void commit_attribute() {
        if (current_attr_name_.empty() || !current_node_) {
            current_attr_name_.clear();
            current_attr_value_.clear();
            buffer_.clear();
            return;
        }
        // Атрибуты в DOM храним в нижнем регистре
        std::string name = current_attr_name_;
        for (char& ch : name) ch = to_lower(ch);
        current_node_->attributes[name] = current_attr_value_;
        current_attr_name_.clear();
        current_attr_value_.clear();
        buffer_.clear();
    }

    void emit_tag() {
        if (is_end_tag_) {
            // Закрывающий тег: ищем соответствующий элемент в стеке
            for (size_t i = stack_.size(); i-- > 1; ) {
                if (stack_[i]->tag_name == current_tag_name_) {
                    stack_.resize(i);
                    break;
                }
            }
            state_ = State::Data;
        }
        else {
            // Переиспользуем current_node_, а не выделяем новый!
            DOMNode* node = current_node_;
            if (!node) {
                node = arena_.Alloc<DOMNode>();
            }
            node->type = NodeType::Element;
            node->tag_name = current_tag_name_;

            stack_.back()->add_child(node);

            const bool is_void = void_elements().count(current_tag_name_) > 0;
            if (!is_self_closing_ && !is_void) {
                stack_.push_back(node);
            }

            // Переход в специальный режим текста...
            if (rcdata_elements().count(current_tag_name_)) {
                last_start_tag_ = current_tag_name_;
                state_ = State::RCDATA;
            }
            else if (rawtext_elements().count(current_tag_name_)) {
                last_start_tag_ = current_tag_name_;
                state_ = State::RAWTEXT;
            }
            else if (current_tag_name_ == "script") {
                last_start_tag_ = current_tag_name_;
                state_ = State::ScriptData;
            }
            else if (current_tag_name_ == "plaintext") {
                state_ = State::PLAINTEXT;
            }
            else {
                state_ = State::Data;
            }
        }

        // Сброс временных данных
        current_node_ = nullptr;
        is_end_tag_ = false;
        is_self_closing_ = false;
        current_tag_name_.clear();
        current_attr_name_.clear();
        current_attr_value_.clear();
        buffer_.clear();
    }

    void emit_raw_end_tag(const std::string& tag) {
        for (size_t i = stack_.size(); i-- > 1; ) {
            if (stack_[i]->tag_name == tag) {
                stack_.resize(i);
                break;
            }
        }
        last_start_tag_.clear();
        buffer_.clear();
    }

    void append_to_current_text(const std::string& s) {
        if (is_attr_value_state(return_state_)) {
            current_attr_value_ += s;
        }
        else {
            text_buffer_ += s;
        }
    }

    void append_char(uint32_t cp) {
        append_to_current_text(encode_utf8(cp));
    }

    // ------------------------------------------------------------------
    //  Разбор символьных ссылок (&...;)
    // ------------------------------------------------------------------
    void parse_character_reference() {
        // pos_ указывает на '&'. Возвращаемся сюда после обработки.
        const size_t amp_pos = pos_;
        size_t i = amp_pos + 1;
        if (i >= html_.size()) {
            append_to_current_text("&");
            return;
        }

        // Числовая ссылка?
        if (html_[i] == '#') {
            ++i;
            bool hex = false;
            if (i < html_.size() && (html_[i] == 'x' || html_[i] == 'X')) {
                hex = true;
                ++i;
            }
            size_t digits_start = i;
            uint32_t value = 0;
            while (i < html_.size()) {
                char cc = html_[i];
                int d;
                if (hex) {
                    if (cc >= '0' && cc <= '9') d = cc - '0';
                    else if (cc >= 'a' && cc <= 'f') d = 10 + cc - 'a';
                    else if (cc >= 'A' && cc <= 'F') d = 10 + cc - 'A';
                    else break;
                }
                else {
                    if (cc >= '0' && cc <= '9') d = cc - '0';
                    else break;
                }
                value = value * (hex ? 16u : 10u) + static_cast<uint32_t>(d);
                ++i;
            }
            if (i == digits_start) {
                // Нет цифр — не ссылка
                append_to_current_text("&");
                return;
            }
            if (i < html_.size() && html_[i] == ';') ++i;
            append_char(sanitize_codepoint(value));
            pos_ = i - 1; // внешний цикл сделает ++pos_
            return;
        }

        // Именованная ссылка
        size_t start = i;
        std::string name;
        const size_t max_len = 32;
        while (i < html_.size() && is_ascii_alnum(html_[i]) && name.size() < max_len) {
            name += html_[i];
            ++i;
        }
        // Пробуем найти самую длинную подходящую сущность
        for (size_t len = name.size(); len > 0; --len) {
            auto it = named_entities().find(name.substr(0, len));
            if (it != named_entities().end()) {
                append_char(it->second);
                // Позиция после сущности (при желании можно съесть ';')
                pos_ = start + len - 1;
                return;
            }
        }
        // Не нашли — оставляем '&' как обычный символ
        append_to_current_text("&");
    }

    // ------------------------------------------------------------------
    //  Основной шаг токенизации
    // ------------------------------------------------------------------
    void process_char(char c) {
        switch (state_) {

            // =============================================================
            // DATA — основной текстовый режим
            // =============================================================
        case State::Data:
            if (c == '&') {
                return_state_ = State::Data;
                parse_character_reference();
            }
            else if (c == '<') {
                flush_text_buffer();
                state_ = State::TagOpen;
            }
            else {
                text_buffer_ += c;
            }
            break;

            // =============================================================
            // RCDATA (title, textarea)
            // =============================================================
        case State::RCDATA:
            if (c == '&') {
                return_state_ = State::RCDATA;
                parse_character_reference();
            }
            else if (c == '<') {
                state_ = State::RCDATALessThanSign;
            }
            else {
                text_buffer_ += c;
            }
            break;

        case State::RCDATALessThanSign:
            if (c == '/') {
                buffer_.clear();
                state_ = State::RCDATAEndTagOpen;
            }
            else {
                text_buffer_ += '<';
                if (c == '<') {
                    // остаёмся в этом же состоянии
                }
                else {
                    text_buffer_ += c;
                    state_ = State::RCDATA;
                }
            }
            break;

        case State::RCDATAEndTagOpen:
            if (is_ascii_alpha(c)) {
                buffer_ += to_lower(c);
                state_ = State::RCDATAEndTagName;
            }
            else {
                text_buffer_ += "</";
                if (c == '<') state_ = State::RCDATALessThanSign;
                else { text_buffer_ += c; state_ = State::RCDATA; }
            }
            break;

        case State::RCDATAEndTagName:
            if (c == '>' && buffer_ == last_start_tag_) {
                emit_raw_end_tag(last_start_tag_);
                state_ = State::Data;
            }
            else if (is_ascii_alpha(c)) {
                buffer_ += to_lower(c);
            }
            else {
                text_buffer_ += "</" + buffer_;
                buffer_.clear();
                if (c == '<') state_ = State::RCDATALessThanSign;
                else { text_buffer_ += c; state_ = State::RCDATA; }
            }
            break;

            // =============================================================
            // RAWTEXT (style, xmp, iframe, noembed, noframes, noscript)
            // =============================================================
        case State::RAWTEXT:
            if (c == '<') state_ = State::RAWTEXTLessThanSign;
            else          text_buffer_ += c;
            break;

        case State::RAWTEXTLessThanSign:
            if (c == '/') {
                buffer_.clear();
                state_ = State::RAWTEXTEndTagOpen;
            }
            else {
                text_buffer_ += '<';
                if (c == '<') { /* остаёмся */ }
                else { text_buffer_ += c; state_ = State::RAWTEXT; }
            }
            break;

        case State::RAWTEXTEndTagOpen:
            if (is_ascii_alpha(c)) {
                buffer_ += to_lower(c);
                state_ = State::RAWTEXTEndTagName;
            }
            else {
                text_buffer_ += "</";
                if (c == '<') state_ = State::RAWTEXTLessThanSign;
                else { text_buffer_ += c; state_ = State::RAWTEXT; }
            }
            break;

        case State::RAWTEXTEndTagName:
            if (c == '>' && buffer_ == last_start_tag_) {
                emit_raw_end_tag(last_start_tag_);
                state_ = State::Data;
            }
            else if (is_ascii_alpha(c)) {
                buffer_ += to_lower(c);
            }
            else {
                text_buffer_ += "</" + buffer_;
                buffer_.clear();
                if (c == '<') state_ = State::RAWTEXTLessThanSign;
                else { text_buffer_ += c; state_ = State::RAWTEXT; }
            }
            break;

            // =============================================================
            // ScriptData (script) — упрощённый режим без вложенных escape
            // =============================================================
        case State::ScriptData:
            if (c == '<') state_ = State::ScriptDataLessThanSign;
            else          text_buffer_ += c;
            break;

        case State::ScriptDataLessThanSign:
            if (c == '/') {
                buffer_.clear();
                state_ = State::ScriptDataEndTagOpen;
            }
            else {
                text_buffer_ += '<';
                if (c == '<') { /* остаёмся */ }
                else { text_buffer_ += c; state_ = State::ScriptData; }
            }
            break;

        case State::ScriptDataEndTagOpen:
            if (is_ascii_alpha(c)) {
                buffer_ += to_lower(c);
                state_ = State::ScriptDataEndTagName;
            }
            else {
                text_buffer_ += "</";
                if (c == '<') state_ = State::ScriptDataLessThanSign;
                else { text_buffer_ += c; state_ = State::ScriptData; }
            }
            break;

        case State::ScriptDataEndTagName:
            if (c == '>' && buffer_ == last_start_tag_) {
                emit_raw_end_tag(last_start_tag_);
                state_ = State::Data;
            }
            else if (is_ascii_alpha(c)) {
                buffer_ += to_lower(c);
            }
            else {
                text_buffer_ += "</" + buffer_;
                buffer_.clear();
                if (c == '<') state_ = State::ScriptDataLessThanSign;
                else { text_buffer_ += c; state_ = State::ScriptData; }
            }
            break;

            // =============================================================
            // PLAINTEXT
            // =============================================================
        case State::PLAINTEXT:
            text_buffer_ += c;
            break;

            // =============================================================
            // Открытие тега
            // =============================================================
        case State::TagOpen:
            if (c == '!') {
                state_ = State::MarkupDeclarationOpen;
                buffer_.clear();
            }
            else if (c == '/') {
                is_end_tag_ = true;
                state_ = State::EndTagOpen;
            }
            else if (is_ascii_alpha(c)) {
                is_end_tag_ = false;
                is_self_closing_ = false;
                current_node_ = arena_.Alloc<DOMNode>();
                current_node_->type = NodeType::Element;
                current_tag_name_.clear();
                buffer_.clear();
                buffer_ += to_lower(c);
                state_ = State::TagName;
            }
            else if (c == '?') {
                // Обработка <? ... > как bogus comment
                state_ = State::BogusComment;
                comment_buffer_.clear();
            }
            else {
                // '<' без тега — просто текст
                text_buffer_ += '<';
                text_buffer_ += c;
                state_ = State::Data;
            }
            break;

        case State::EndTagOpen:
            if (is_ascii_alpha(c)) {
                buffer_.clear();
                buffer_ += to_lower(c);
                state_ = State::TagName;
            }
            else if (c == '>') {
                // </> — пустой закрывающий тег, игнорируем
                state_ = State::Data;
            }
            else {
                state_ = State::BogusComment;
                comment_buffer_.clear();
                if (c != 0) comment_buffer_ += c;
            }
            break;

        case State::TagName:
            if (is_html_space(c)) {
                current_tag_name_ = buffer_;
                buffer_.clear();
                state_ = State::BeforeAttributeName;
            }
            else if (c == '/') {
                current_tag_name_ = buffer_;
                buffer_.clear();
                state_ = State::SelfClosingStartTag;
            }
            else if (c == '>') {
                current_tag_name_ = buffer_;
                buffer_.clear();
                emit_tag();
            }
            else {
                buffer_ += to_lower(c);
            }
            break;

            // =============================================================
            // Атрибуты
            // =============================================================
        case State::BeforeAttributeName:
            if (is_html_space(c)) {
                // пропускаем пробелы
            }
            else if (c == '/') {
                state_ = State::SelfClosingStartTag;
            }
            else if (c == '>') {
                emit_tag();
            }
            else if (c == '=') {
                // невалидный случай: '=' перед именем атрибута
                buffer_.clear();
                buffer_ += c;
                state_ = State::AttributeName;
            }
            else {
                buffer_.clear();
                buffer_ += c;
                state_ = State::AttributeName;
            }
            break;

        case State::AttributeName:
            if (is_html_space(c)) {
                current_attr_name_ = buffer_;
                buffer_.clear();
                commit_attribute();
                state_ = State::AfterAttributeName;
            }
            else if (c == '=') {
                current_attr_name_ = buffer_;
                buffer_.clear();
                state_ = State::BeforeAttributeValue;
            }
            else if (c == '>') {
                current_attr_name_ = buffer_;
                buffer_.clear();
                commit_attribute();
                emit_tag();
            }
            else if (c == '/') {
                current_attr_name_ = buffer_;
                buffer_.clear();
                commit_attribute();
                state_ = State::SelfClosingStartTag;
            }
            else {
                buffer_ += to_lower(c);
            }
            break;

        case State::AfterAttributeName:
            if (is_html_space(c)) {
                // пропускаем
            }
            else if (c == '=') {
                state_ = State::BeforeAttributeValue;
            }
            else if (c == '>') {
                emit_tag();
            }
            else if (c == '/') {
                state_ = State::SelfClosingStartTag;
            }
            else {
                buffer_.clear();
                buffer_ += c;
                state_ = State::AttributeName;
            }
            break;

        case State::BeforeAttributeValue:
            if (is_html_space(c)) {
                // пропускаем
            }
            else if (c == '"') {
                buffer_.clear();
                state_ = State::AttributeValueDoubleQuoted;
            }
            else if (c == '\'') {
                buffer_.clear();
                state_ = State::AttributeValueSingleQuoted;
            }
            else if (c == '>') {
                current_attr_value_.clear();
                commit_attribute();
                emit_tag();
            }
            else {
                buffer_.clear();
                buffer_ += c;
                state_ = State::AttributeValueUnquoted;
            }
            break;

        case State::AttributeValueDoubleQuoted:
            if (c == '"') {
                current_attr_value_ = buffer_;
                buffer_.clear();
                commit_attribute();
                state_ = State::AfterAttributeValueQuoted;
            }
            else if (c == '&') {
                return_state_ = State::AttributeValueDoubleQuoted;
                parse_character_reference();
            }
            else {
                buffer_ += c;
            }
            break;

        case State::AttributeValueSingleQuoted:
            if (c == '\'') {
                current_attr_value_ = buffer_;
                buffer_.clear();
                commit_attribute();
                state_ = State::AfterAttributeValueQuoted;
            }
            else if (c == '&') {
                return_state_ = State::AttributeValueSingleQuoted;
                parse_character_reference();
            }
            else {
                buffer_ += c;
            }
            break;

        case State::AttributeValueUnquoted:
            if (is_html_space(c)) {
                current_attr_value_ = buffer_;
                buffer_.clear();
                commit_attribute();
                state_ = State::BeforeAttributeName;
            }
            else if (c == '>') {
                current_attr_value_ = buffer_;
                buffer_.clear();
                commit_attribute();
                emit_tag();
            }
            else if (c == '&') {
                return_state_ = State::AttributeValueUnquoted;
                parse_character_reference();
            }
            else {
                buffer_ += c;
            }
            break;

        case State::AfterAttributeValueQuoted:
            if (is_html_space(c)) {
                state_ = State::BeforeAttributeName;
            }
            else if (c == '/') {
                state_ = State::SelfClosingStartTag;
            }
            else if (c == '>') {
                emit_tag();
            }
            else {
                // Невалидный символ — начинаем новый атрибут
                buffer_.clear();
                buffer_ += c;
                state_ = State::AttributeName;
            }
            break;

        case State::SelfClosingStartTag:
            if (c == '>') {
                is_self_closing_ = true;
                emit_tag();
            }
            else if (is_html_space(c)) {
                // допускается
            }
            else {
                // Не '>' — возвращаемся к атрибутам
                state_ = State::BeforeAttributeName;
                // Повторно обработать текущий символ
                process_char(c);
            }
            break;

            // =============================================================
            // Комментарии
            // =============================================================
        case State::BogusComment:
            if (c == '>') {
                DOMNode* node = arena_.Alloc<DOMNode>();
                node->type = NodeType::Comment;
                node->text_content = comment_buffer_;
                if (!stack_.empty()) stack_.back()->add_child(node);
                comment_buffer_.clear();
                state_ = State::Data;
            }
            else {
                comment_buffer_ += c;
            }
            break;

        case State::MarkupDeclarationOpen:
            // Мы ожидаем "DOCTYPE", "CDATA[" или "--"
            if (html_.compare(pos_, 7, "DOCTYPE") == 0 ||
                html_.compare(pos_, 7, "doctype") == 0) {
                pos_ += 6; // внешний цикл добавит ещё 1
                doctype_buffer_.clear();
                state_ = State::Doctype;
            }
            else if (html_.compare(pos_, 7, "[CDATA[") == 0) {
                pos_ += 6;
                comment_buffer_.clear();
                state_ = State::CdataSection;
            }
            else if (html_.compare(pos_, 2, "--") == 0) {
                pos_ += 1;
                comment_buffer_.clear();
                state_ = State::CommentStart;
            }
            else {
                state_ = State::BogusComment;
                comment_buffer_.clear();
            }
            break;

        case State::CommentStart:
            if (c == '-') {
                state_ = State::CommentStartDash;
            }
            else if (c == '>') {
                // <!----> — пустой комментарий
                DOMNode* node = arena_.Alloc<DOMNode>();
                node->type = NodeType::Comment;
                node->text_content = "";
                if (!stack_.empty()) stack_.back()->add_child(node);
                state_ = State::Data;
            }
            else {
                comment_buffer_ += c;
                state_ = State::Comment;
            }
            break;

        case State::CommentStartDash:
            if (c == '-') {
                state_ = State::CommentEnd;
            }
            else if (c == '>') {
                DOMNode* node = arena_.Alloc<DOMNode>();
                node->type = NodeType::Comment;
                node->text_content = "";
                if (!stack_.empty()) stack_.back()->add_child(node);
                state_ = State::Data;
            }
            else {
                comment_buffer_ += '-';
                comment_buffer_ += c;
                state_ = State::Comment;
            }
            break;

        case State::Comment:
            if (c == '-') {
                state_ = State::CommentEndDash;
            }
            else {
                comment_buffer_ += c;
            }
            break;

        case State::CommentEndDash:
            if (c == '-') {
                state_ = State::CommentEnd;
            }
            else {
                comment_buffer_ += '-';
                comment_buffer_ += c;
                state_ = State::Comment;
            }
            break;

        case State::CommentEnd:
            if (c == '>') {
                DOMNode* node = arena_.Alloc<DOMNode>();
                node->type = NodeType::Comment;
                node->text_content = comment_buffer_;
                if (!stack_.empty()) stack_.back()->add_child(node);
                comment_buffer_.clear();
                state_ = State::Data;
            }
            else if (c == '!') {
                state_ = State::CommentEndBang;
            }
            else if (c == '-') {
                comment_buffer_ += '-';
            }
            else {
                comment_buffer_ += "--";
                comment_buffer_ += c;
                state_ = State::Comment;
            }
            break;

        case State::CommentEndBang:
            if (c == '>') {
                DOMNode* node = arena_.Alloc<DOMNode>();
                node->type = NodeType::Comment;
                node->text_content = comment_buffer_;
                if (!stack_.empty()) stack_.back()->add_child(node);
                comment_buffer_.clear();
                state_ = State::Data;
            }
            else {
                comment_buffer_ += "--!";
                comment_buffer_ += c;
                state_ = State::Comment;
            }
            break;

            // =============================================================
            // DOCTYPE
            // =============================================================
        case State::Doctype:
            if (is_html_space(c)) {
                state_ = State::BeforeDoctypeName;
            }
            else if (c == '>') {
                DOMNode* node = arena_.Alloc<DOMNode>();
                node->type = NodeType::Doctype;
                node->text_content = "";
                if (!stack_.empty()) stack_.back()->add_child(node);
                state_ = State::Data;
            }
            else {
                state_ = State::BeforeDoctypeName;
                process_char(c);
            }
            break;

        case State::BeforeDoctypeName:
            if (is_html_space(c)) {
                // пропускаем
            }
            else if (c == '>') {
                DOMNode* node = arena_.Alloc<DOMNode>();
                node->type = NodeType::Doctype;
                node->text_content = "";
                if (!stack_.empty()) stack_.back()->add_child(node);
                state_ = State::Data;
            }
            else {
                doctype_buffer_.clear();
                doctype_buffer_ += to_lower(c);
                state_ = State::DoctypeName;
            }
            break;

        case State::DoctypeName:
            if (is_html_space(c)) {
                state_ = State::AfterDoctypeName;
            }
            else if (c == '>') {
                DOMNode* node = arena_.Alloc<DOMNode>();
                node->type = NodeType::Doctype;
                node->text_content = doctype_buffer_;
                if (!stack_.empty()) stack_.back()->add_child(node);
                doctype_buffer_.clear();
                state_ = State::Data;
            }
            else {
                doctype_buffer_ += to_lower(c);
            }
            break;

        case State::AfterDoctypeName:
            if (is_html_space(c)) {
                // пропускаем
            }
            else if (c == '>') {
                DOMNode* node = arena_.Alloc<DOMNode>();
                node->type = NodeType::Doctype;
                node->text_content = doctype_buffer_;
                if (!stack_.empty()) stack_.back()->add_child(node);
                doctype_buffer_.clear();
                state_ = State::Data;
            }
            else {
                // Игнорируем PUBLIC/SYSTEM — уходим в bogus doctype
                state_ = State::BogusDoctype;
            }
            break;

        case State::BogusDoctype:
            if (c == '>') {
                state_ = State::Data;
            }
            // иначе игнорируем
            break;

            // =============================================================
            // CDATA
            // =============================================================
        case State::CdataSection:
            if (c == ']') {
                state_ = State::CdataSectionBracket;
            }
            else {
                text_buffer_ += c;
            }
            break;

        case State::CdataSectionBracket:
            if (c == ']') {
                state_ = State::CdataSectionEnd;
            }
            else {
                text_buffer_ += ']';
                text_buffer_ += c;
                state_ = State::CdataSection;
            }
            break;

        case State::CdataSectionEnd:
            if (c == '>') {
                state_ = State::Data;
            }
            else if (c == ']') {
                text_buffer_ += ']';
            }
            else {
                text_buffer_ += "]]";
                text_buffer_ += c;
                state_ = State::CdataSection;
            }
            break;

            // =============================================================
            // Заглушки для состояний, обрабатываемых в parse_character_reference
            // =============================================================
        case State::CharacterReference:
        case State::NumericCharacterReferenceStart:
        case State::NumericCharacterReference:
        case State::NumericCharacterReferenceEnd:
            // Эти состояния обрабатываются вне основного цикла —
            // сюда мы попадать не должны.
            state_ = State::Data;
            break;
        }
    }
};