#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>

enum class NodeType {
    Document,
    Element,
    Text,
    Comment,
    Doctype
};

struct DOMNode {
    NodeType type = NodeType::Element;
    std::string tag_name;         // дл€ Element Ч им€ тега (в нижнем регистре)
    std::string text_content;     // дл€ Text Ч текст, дл€ Comment Ч тело, дл€ Doctype Ч содержимое
    std::unordered_map<std::string, std::string> attributes;

    DOMNode* parent = nullptr;
    std::vector<DOMNode*> children;

    void add_child(DOMNode* child) {
        if (!child) return;
        child->parent = this;
        children.push_back(child);
    }
};

// –екурсивный вывод DOM-дерева в консоль дл€ отладки
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