#pragma once
#include <vector>
#include <memory>
#include <cstdint>
#include <string>

enum class BoxType {
    Block,
    Inline,
    AnonymousBlock,
    Text
};

struct LayoutGeometry {
    float x{ 0.0f }, y{ 0.0f };
    float width{ 0.0f }, height{ 0.0f };

    float margin_top{ 0.0f }, margin_right{ 0.0f }, margin_bottom{ 0.0f }, margin_left{ 0.0f };
    float padding_top{ 0.0f }, padding_right{ 0.0f }, padding_bottom{ 0.0f }, padding_left{ 0.0f };
    float border_top{ 0.0f }, border_right{ 0.0f }, border_bottom{ 0.0f }, border_left{ 0.0f };
};

struct LayoutNode {
    // UINT32_MAX (== INVALID_INDEX) для AnonymousBlock и Text:
    // стили берутся у ближайшего реального предка.
    uint32_t style_soa_idx{ UINT32_MAX };
    BoxType type{ BoxType::Block };

    LayoutGeometry geometry;
    std::string text_content;   // заполняется только для BoxType::Text

    LayoutNode* parent{ nullptr };
    std::vector<std::unique_ptr<LayoutNode>> children;

    void add_child(std::unique_ptr<LayoutNode> child) {
        child->parent = this;
        children.push_back(std::move(child));
    }

    bool is_text()         const { return type == BoxType::Text; }
    bool is_anonymous()    const { return type == BoxType::AnonymousBlock; }
    bool is_block_level()  const { return type == BoxType::Block || type == BoxType::AnonymousBlock; }
    bool is_inline_level() const { return type == BoxType::Inline || type == BoxType::Text; }
};