#pragma once
#include <string>
#include <iostream>
#include <memory_resource>
#include <unordered_map>
#include "../../arena_memory_allocator/headers/arena.h"

enum class NodeType { Document, Element, Text, Comment, Doctype };

// Thread-local текущий PMR-ресурс. Устанавливается в HTMLParser::parse
// и CSSParser::parse на время разбора — все pmr-контейнеры внутри
// созданных узлов будут аллоцироваться в арене парсера.
inline ArenaMemoryResource*& current_arena_resource() {
    static thread_local ArenaMemoryResource* r = nullptr;
    return r;
}

// Хелпер: возвращает ресурс для новых контейнеров.
// Если current_arena_resource не установлен (например, в тестах),
// падаем на дефолтный — обычный new/delete.
inline std::pmr::memory_resource* pmr() {
    auto* r = current_arena_resource();
    return r ? static_cast<std::pmr::memory_resource*>(r)
        : std::pmr::get_default_resource();
}

struct DOMNode {
    NodeType type = NodeType::Element;
    std::pmr::string tag_name;
    std::pmr::string text_content;

    // Ключи и значения тоже pmr — иначе каждая пара (attr,value) утечёт
    std::pmr::unordered_map<std::pmr::string, std::pmr::string> attributes;

    DOMNode* parent = nullptr;
    std::pmr::vector<DOMNode*> children;

    DOMNode()
        : tag_name(pmr())
        , text_content(pmr())
        , attributes(pmr())
        , children(pmr())
    {
    }

    void add_child(DOMNode* child) {
        if (!child) return;
        child->parent = this;
        children.push_back(child);
    }
};
// Рекурсивный вывод DOM-дерева в консоль для отладки
inline void print_dom(const DOMNode* node, int depth = 0) {
    if (!node) return;
    std::string indent(depth * 2, ' ');

    switch (node->type) {
    case NodeType::Element: {
        std::cout << indent << "<" << node->tag_name;
        for (const auto& [attr, val] : node->attributes) {
            std::cout << " " << attr;
            if (!val.empty()) std::cout << "=\"" << val << "\"";
        }
        std::cout << ">\n";
        break;
    }
    case NodeType::Text:
        std::cout << indent << "\"" << node->text_content << "\"\n";
        break;
    case NodeType::Comment:
        std::cout << indent << "<!--" << node->text_content << "-->\n";
        break;
    case NodeType::Doctype:
        std::cout << indent << "<!DOCTYPE " << node->text_content << ">\n";
        break;
    case NodeType::Document:
        break;
    }

    for (const auto* child : node->children) {
        print_dom(child, depth + 1);
    }

    if (node->type == NodeType::Element && !node->children.empty()) {
        std::cout << indent << "</" << node->tag_name << ">\n";
    }
}