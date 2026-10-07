#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "parsers/arena_memory_allocator/headers/arena.h"
#include "parsers/html_parser/headers/dom.h"
#include "parsers/selector_matcher/style_storage_soa.h"
#include "parsers/selector_matcher/display_list.h"
#include "layout/headers/layout_node.h"
#include "layout/headers/layout_tree_builder.h"   // для TextMetrics
#include "js/JsEngine.h"

// Слушатель события, привязанный к DOMNode.
struct JsListener {
    std::string type;
    JSValue     handler;

    JsListener() : handler(JS_UNDEFINED) {}
    JsListener(std::string t, JSValue h)
        : type(std::move(t)), handler(h) {
    }
};

class Page {
public:
    Page();
    ~Page();

    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    // Парсит HTML, выполняет inline-скрипты, делает layout.
    // tm — текстовые метрики для точного совпадения с Qt painter'ом.
    // Если не заданы — layout использует эвристику (0.55*fs на символ).
    void loadHtml(const std::string& html,
        float viewport_w, float viewport_h,
        const LayoutTreeBuilder::TextMetrics& tm = {});

    // Пересчёт layout + display list после мутаций DOM.
    void relayout();

    // Актуальный кадр.
    const DisplayList& displayList() const { return display_list_; }

    // События из UI.
    void dispatchClick(float x, float y);

    // Доступ для bindings.
    ArenaAllocator& arena() { return arena_; }
    DOMNode* document() { return document_; }

    // Регистрация listener'а из JS addEventListener.
    void addListener(DOMNode* node, const std::string& type, JSValue handler);

    // Проверка, изменился ли DOM с последнего relayout.
    void markDirty() { dom_dirty_ = true; }

    // Обход DOM с предикатом (для bindings).
    DOMNode* findFirst(DOMNode* root, bool (*pred)(const DOMNode*, void*),
        void* ctx);
    void collectAll(DOMNode* root, bool (*pred)(const DOMNode*, void*),
        void* ctx, std::vector<DOMNode*>& out);

private:
    void runInlineScripts();
    void rebuild();

    // Освобождение JS-хэндлеров. Должно вызываться ДО js_.reset().
    void releaseListeners();

    // Порядок членов критичен: arena_ разрушится последней.
    ArenaAllocator              arena_;
    DOMNode* document_ = nullptr;
    std::unique_ptr<JsEngine>   js_;
    std::unordered_map<DOMNode*, std::vector<JsListener>> listeners_;
    StyleStorageSoA             storage_;
    std::unique_ptr<LayoutNode> layout_;
    DisplayList                 display_list_;
    bool                        dom_dirty_ = false;

    float viewport_w_ = 1024.0f;
    float viewport_h_ = 768.0f;

    LayoutTreeBuilder::TextMetrics text_metrics_;
};