#include "Page.h"

#include "parsers/html_parser/headers/html_parser.h"
#include "parsers/css_parser/headers/css_parser.h"
#include "parsers/selector_matcher/style_tree_builder.h"
#include "parsers/selector_matcher/user_agent_stylesheet.h"
#include "parsers/selector_matcher/style_origin.h"
#include "parsers/selector_matcher/display_list_builder.h"
#include "layout/headers/layout_tree_builder.h"

#include <QRectF>
#include <QDebug>
#include <functional>

// ---- Вспомогательные обходы DOM ----
namespace {

    void collectStyleText(DOMNode* n, std::string& out) {
        if (!n) return;
        if (n->type == NodeType::Element && n->tag_name == "style") {
            for (DOMNode* c : n->children)
                if (c->type == NodeType::Text) out += c->text_content;
            return;
        }
        for (DOMNode* c : n->children) collectStyleText(c, out);
    }

    void collectScripts(DOMNode* n, std::vector<DOMNode*>& out) {
        if (!n) return;
        if (n->type == NodeType::Element && n->tag_name == "script") {
            if (n->attributes.find("src") == n->attributes.end())
                out.push_back(n);
            return;
        }
        for (DOMNode* c : n->children) collectScripts(c, out);
    }

    DOMNode* firstElement(DOMNode* n) {
        if (!n) return nullptr;
        if (n->type == NodeType::Element) return n;
        for (DOMNode* c : n->children)
            if (auto* e = firstElement(c)) return e;
        return nullptr;
    }

} // namespace

// ============================================================
//  Жизненный цикл
// ============================================================

Page::Page() = default;

Page::~Page() {
    releaseListeners();
}

void Page::releaseListeners() {
    // Важно: освобождаем JS-значения, пока ctx_ жив.
    if (js_ && js_->ctx()) {
        JSContext* ctx = js_->ctx();
        for (auto& [node, vec] : listeners_) {
            for (auto& l : vec) {
                if (!JS_IsUndefined(l.handler))
                    JS_FreeValue(ctx, l.handler);
            }
        }
    }
    listeners_.clear();
}

// ============================================================
//  loadHtml
// ============================================================

void Page::loadHtml(const std::string& html,
    float viewport_w, float viewport_h,
    const LayoutTreeBuilder::TextMetrics& tm)
{
    viewport_w_ = viewport_w;
    viewport_h_ = viewport_h;
    text_metrics_ = tm;

    // 1. Сначала освобождаем JS-значения старого контекста, потом
    //    сносим сам контекст — иначе получим use-after-free.
    releaseListeners();

    // 2. Полный сброс страницы.
    arena_.reset();
    document_ = nullptr;
    js_.reset();
    storage_ = {};
    layout_.reset();
    display_list_.clear();
    dom_dirty_ = false;

    // 3. Парсим новый HTML.
    HTMLParser hp(arena_);
    DOMNode* doc = hp.parse(html);
    if (!doc) return;
    document_ = firstElement(doc);
    if (!document_) return;

    // 4. JS-движок + DOM-глобалы.
    js_ = std::make_unique<JsEngine>();
    js_->installGlobals(this, document_);
    // ДИАГНОСТИКА — удалить после проверки
    js_->run("console.log('typeof document =', typeof document);", "<diag>");
    js_->run("console.log('typeof __elementClassId =', typeof __elementClassId);", "<diag>");
    js_->run("console.log('typeof console =', typeof console);", "<diag>");
    js_->run("console.log('typeof __page =', typeof __page);", "<diag>");

    // 5. Выполняем скрипты до layout'а — они могут менять DOM.
    runInlineScripts();

    // 6. Первый расчёт layout и display list.
    rebuild();
}

// ============================================================
//  Скрипты
// ============================================================

void Page::runInlineScripts() {
    std::vector<DOMNode*> scripts;
    collectScripts(document_, scripts);

    for (DOMNode* s : scripts) {
        std::string code;
        for (DOMNode* c : s->children)
            if (c->type == NodeType::Text) code += c->text_content;
        if (!code.empty())
            js_->run(code, "<inline>");
    }
}

// ============================================================
//  Layout pipeline
// ============================================================

void Page::rebuild() {
    if (!document_) return;

    // 1. CSS: UA + все <style>.
    std::string author_css;
    collectStyleText(document_, author_css);

    CSSParser cp(arena_);
    StyleSheet* ua = cp.parse(ua_css::kSource);
    StyleSheet* author = cp.parse(author_css);

    std::vector<StyleRuleIndex::SheetRef> sources = {
        { ua,     Origin::UserAgent },
        { author, Origin::Author    },
    };

    // 2. Каскад в SoA.
    storage_ = StyleTreeBuilder::build(document_, sources);

    // ДИАГНОСТИКА — удалить после проверки
    for (uint32_t i = 0; i < storage_.size(); ++i) {
        auto* d = storage_.dom_nodes[i];
        if (d && d->tag_name == "h1") {
            qDebug() << "[rebuild] h1 children:";
            for (auto* c : d->children) {
                qDebug() << "  type:" << int(c->type)
                    << "text:" << QString::fromStdString(c->text_content);
            }
        }
    }

    // 3. Layout с текстовыми метриками (Qt или эвристика).
    layout_ = LayoutTreeBuilder::build(storage_);
    if (layout_) {
        LayoutTreeBuilder::Viewport vp{ viewport_w_, viewport_h_ };
        LayoutTreeBuilder::compute_layout(layout_.get(), vp, storage_,
            text_metrics_);
    }

    // 4. Display list.
    if (layout_)
        display_list_ = DisplayListBuilder::build(layout_.get(), storage_);

    dom_dirty_ = false;
}

void Page::relayout() {
    if (dom_dirty_) rebuild();
}

// ============================================================
//  Слушатели
// ============================================================

void Page::addListener(DOMNode* node, const std::string& type, JSValue handler) {
    if (!node || !js_) return;

    JsListener l;
    l.type = type;
    l.handler = JS_DupValue(js_->ctx(), handler);
    listeners_[node].push_back(std::move(l));
}

// ============================================================
//  Обход DOM для bindings
// ============================================================

DOMNode* Page::findFirst(DOMNode* root, bool (*pred)(const DOMNode*, void*),
    void* ctx)
{
    if (!root) return nullptr;
    if (root->type == NodeType::Element && pred(root, ctx)) return root;
    for (DOMNode* c : root->children)
        if (auto* r = findFirst(c, pred, ctx)) return r;
    return nullptr;
}

void Page::collectAll(DOMNode* root, bool (*pred)(const DOMNode*, void*),
    void* ctx, std::vector<DOMNode*>& out)
{
    if (!root) return;
    if (root->type == NodeType::Element && pred(root, ctx))
        out.push_back(root);
    for (DOMNode* c : root->children) collectAll(c, pred, ctx, out);
}

// ============================================================
//  События
// ============================================================

void Page::dispatchClick(float x, float y) {
    if (!layout_ || !js_) return;

    // Hit test: ищем самый глубокий LayoutNode, содержащий точку,
    // у которого есть реальный DOM-элемент.
    DOMNode* target = nullptr;

    std::function<void(const LayoutNode*, float, float)> hit =
        [&](const LayoutNode* n, float ox, float oy)
        {
            if (!n) return;
            const float ax = ox + n->geometry.x;
            const float ay = oy + n->geometry.y;
            const QRectF box(ax, ay, n->geometry.width, n->geometry.height);
            if (!box.contains(x, y)) return;

            if (n->style_soa_idx != UINT32_MAX) {
                target = storage_.dom_nodes[n->style_soa_idx];
            }
            for (const auto& c : n->children) hit(c.get(), ax, ay);
        };
    hit(layout_.get(), 0.0f, 0.0f);

    if (!target) return;

    // Bubbling: от target вверх по DOM-дереву.
    for (DOMNode* n = target; n; n = n->parent) {
        auto it = listeners_.find(n);
        if (it == listeners_.end()) continue;

        for (const auto& l : it->second) {
            if (l.type != "click") continue;

            JSContext* ctx = js_->ctx();
            JSValue    global = JS_GetGlobalObject(ctx);
            JSValue    event = JS_NewObject(ctx);

            // Минимальный Event-объект.
            JS_SetPropertyStr(ctx, event, "type",
                JS_NewString(ctx, "click"));
            JS_SetPropertyStr(ctx, event, "target", JS_UNDEFINED);
            JS_SetPropertyStr(ctx, event, "preventDefault",
                JS_NewCFunction(ctx,
                    [](JSContext*, JSValueConst, int, JSValueConst*) {
                        return JS_UNDEFINED;
                    }, "preventDefault", 0));

            JSValue ret = JS_Call(ctx, l.handler, global, 1, &event);
            if (JS_IsException(ret)) {
                JSValue exc = JS_GetException(ctx);
                const char* s = JS_ToCString(ctx, exc);
                qWarning() << "[js] click handler error:" << (s ? s : "?");
                if (s) JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, exc);
            }

            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, event);
            JS_FreeValue(ctx, global);
        }
    }

    relayout();
}