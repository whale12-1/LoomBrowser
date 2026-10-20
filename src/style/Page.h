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
#include "js/JSEngine.h"

#include <QImage>

using AsyncImageFetcher = std::function<void(const std::string&, std::function<void(QImage)>)>;

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

    std::function<void()> on_layout_updated_;
    // Парсит HTML, выполняет inline-скрипты, делает layout.
    // tm — текстовые метрики для точного совпадения с Qt painter'ом.
    // Если не заданы — layout использует эвристику (0.55*fs на символ).
    void loadHtml(const std::string& html,
        const std::string& base_url,
        std::function<std::string(const std::string&)> fetch_css,
        AsyncImageFetcher fetch_image, // <--- Новый параметр
        float viewport_w, float viewport_h,
        const LayoutTreeBuilder::TextMetrics& tm);

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
    // Базовая директория для резолва относительных путей (src в <img>).
// Пустая — только абсолютные пути.
    void setBaseDir(std::string dir) { base_dir_ = std::move(dir); }

private:
    void runInlineScripts();
    void rebuild();

    // Освобождение JS-хэндлеров. Должно вызываться ДО js_.reset().
    void releaseListeners();

    void addImage(const std::string& url, const QImage& img);
    std::unordered_map<std::string, std::shared_ptr<QImage>> image_cache_;
    std::string base_url_;

    // Порядок членов критичен: arena_ разрушится последней.
    ArenaAllocator              arena_;
    DOMNode* document_ = nullptr;
    std::unique_ptr<JsEngine>   js_;
    std::unordered_map<DOMNode*, std::vector<JsListener>> listeners_;
    StyleStorageSoA             storage_;
    std::unique_ptr<LayoutNode> layout_;
    DisplayList                 display_list_;
    bool                        dom_dirty_ = false;
    std::string                 external_css_;

    float viewport_w_ = 1024.0f;
    float viewport_h_ = 768.0f;
    std::string base_dir_;
    LayoutTreeBuilder::TextMetrics text_metrics_;
};