#include "Page.h"

#include "parsers/html_parser/headers/html_parser.h"
#include "parsers/css_parser/headers/css_parser.h"
#include "parsers/selector_matcher/style_tree_builder.h"
#include "parsers/selector_matcher/user_agent_stylesheet.h"
#include "parsers/selector_matcher/style_origin.h"
#include "parsers/selector_matcher/display_list_builder.h"
#include "layout/headers/layout_tree_builder.h"
#include <QImage>
#include <QUrl>
#include <QRectF>
#include <QDebug>
#include <QEventLoop>
#include <QByteArray>
#include <functional>
#include <fstream>

// ---- Вспомогательные обходы DOM ----
namespace {

    void collect_link_tags(DOMNode* n, std::vector<DOMNode*>& out) {
        if (!n) return;
        if (n->type == NodeType::Element && n->tag_name == "link") {
            out.push_back(n);
        }
        for (DOMNode* c : n->children) collect_link_tags(c, out);
    }

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

    std::string resolvePath(const std::string& base_dir,
        const std::string& src) {
        if (src.empty()) return {};
        if (src.size() >= 2 && src[1] == ':') return src;        // Windows
        if (src[0] == '/' || src[0] == '\\') return src;         // Unix/abs
        if (base_dir.empty()) return src;
        std::string out = base_dir;
        if (!out.empty() && out.back() != '/' && out.back() != '\\')
            out += '/';
        out += src;
        return out;
    }

    LayoutTreeBuilder::ImageMetrics makeImageMetrics(
        const std::string& base_dir)
    {
        LayoutTreeBuilder::ImageMetrics m;
        if (base_dir.empty()) return m;
        m.intrinsic_size = [base_dir](const std::string& src)
            -> std::pair<float, float>
            {
                const std::string path = resolvePath(base_dir, src);
                QImage img;
                if (!img.load(QString::fromStdString(path)))
                    return { 0.0f, 0.0f };
                return { float(img.width()), float(img.height()) };
            };
        return m;
    }

    void collect_image_srcs(DOMNode* n, std::vector<std::string>& out) {
        if (!n) return;
        if (n->type == NodeType::Element && n->tag_name == "img") {
            for (const auto& kv : n->attributes) {
                if (std::string_view(kv.first.data(), kv.first.size()) == "src") {
                    out.push_back(std::string(kv.second));
                }
            }
        }
        for (DOMNode* c : n->children) collect_image_srcs(c, out);
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
    const std::string& base_url,
    std::function<std::string(const std::string&)> fetch_resource,
    AsyncImageFetcher fetch_image,
    float viewport_w, float viewport_h,
    const LayoutTreeBuilder::TextMetrics& tm)
{
    viewport_w_ = viewport_w;
    viewport_h_ = viewport_h;
    text_metrics_ = tm;

    releaseListeners();

    arena_.reset();
    document_ = nullptr;
    js_.reset();
    storage_ = {};
    layout_.reset();
    display_list_.clear();
    dom_dirty_ = false;
    external_css_.clear(); // Сброс внешнего CSS
    base_url_ = base_url;
    image_cache_.clear(); // Очищаем кэш при новой загрузке

    HTMLParser hp(arena_);
    DOMNode* doc = hp.parse(html);
    if (!doc) return;
    document_ = firstElement(doc);
    if (!document_) return;


    std::ofstream out("wikipedia_dump.html");
    out << html;
    out.close();


    // --- НОВОЕ: Загрузка внешних CSS ---
    if (fetch_resource && !base_url.empty()) {
        std::vector<DOMNode*> links;
        collect_link_tags(document_, links);
        qDebug() << "[Page] Found <link> tags:" << links.size(); // Лог 1: сколько тегов нашли

        for (DOMNode* link : links) {
            std::string rel, href;
            for (const auto& kv : link->attributes) {
                std::string_view k(kv.first.data(), kv.first.size());
                if (k == "rel") rel = kv.second;
                if (k == "href") href = kv.second;
            }
            qDebug() << "[Page] Link tag: rel=" << QString::fromStdString(rel) << " href=" << QString::fromStdString(href); // Лог 2: что внутри тега

            if (rel.find("stylesheet") != std::string::npos && !href.empty()) {
                qDebug() << "[Page] Fetching CSS:" << QString::fromStdString(href);
                std::string css = fetch_resource(href);
                if (!css.empty()) {
                    external_css_ += css;
                    external_css_ += "\n";
                }
            }
        }
    }
    if (fetch_image && !base_url.empty()) {
        std::vector<std::string> srcs;
        collect_image_srcs(document_, srcs);
        for (const std::string& src : srcs) {
            QUrl base_qurl(QString::fromStdString(base_url));
            QUrl abs_qurl = base_qurl.resolved(QUrl(QString::fromStdString(src)));
            std::string abs_url = abs_qurl.toString().toStdString();

            if (image_cache_.find(abs_url) == image_cache_.end()) {
                fetch_image(abs_url, [this, abs_url](QImage img) {
                    this->addImage(abs_url, img);
                    });
            }
        }
    }

    qDebug() << "[Page] Total external CSS size:" << external_css_.size();
    // ----------------------------------

    js_ = std::make_unique<JsEngine>();
    js_->installGlobals(this, document_);
    // ... диагностика ...

    runInlineScripts();
    rebuild();
}

// ============================================================
//  Скрипты
// ============================================================
void Page::addImage(const std::string& url, const QImage& img) {
    image_cache_[url] = std::make_shared<QImage>(img);
    // Если страница уже отрисована, пересчитываем layout
    if (layout_) {
        rebuild();
    }
}

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
    std::string author_css = external_css_; // <-- Начинаем с внешнего CSS
    collectStyleText(document_, author_css); // <-- Добавляем <style> из HTML
    CSSParser cp(arena_);
    StyleSheet* ua = cp.parse(ua_css::kSource);
    StyleSheet* author = cp.parse(author_css);
    qDebug() << "UA CSS rules parsed:" << ua->rules.size();
    qDebug() << "Author CSS rules parsed:" << author->rules.size();
    qDebug() << "Author CSS errors:" << cp.errors().size();
    for (const auto& err : cp.errors()) {
        qDebug() << "CSS Error:" << QString::fromStdString(err);
    }
    std::vector<StyleRuleIndex::SheetRef> sources = {
        { ua,     Origin::UserAgent },
        { author, Origin::Author    },
    };

    // 2. Каскад в SoA.
    qDebug() << "Author CSS preview:" << QString::fromStdString(author_css.substr(0, 200));

    storage_ = StyleTreeBuilder::build(document_, sources, viewport_w_, viewport_h_);



    // 3. Layout с текстовыми метриками (Qt или эвристика).
        // 3. Layout с картинками из кэша
    LayoutTreeBuilder::ImageMetrics img_metrics;
    img_metrics.intrinsic_size = [this](const std::string& src) -> std::pair<float, float> {
        QUrl base_qurl(QString::fromStdString(base_url_));
        QUrl abs_qurl = base_qurl.resolved(QUrl(QString::fromStdString(src)));
        std::string abs_url = abs_qurl.toString().toStdString();

        auto it = image_cache_.find(abs_url);
        if (it != image_cache_.end() && it->second && !it->second->isNull()) {
            return { float(it->second->width()), float(it->second->height()) };
        }
        // Если не в кэше, пробуем локальный файл
        const std::string path = resolvePath(base_dir_, src);
        QImage img;
        if (img.load(QString::fromStdString(path)))
            return { float(img.width()), float(img.height()) };

        return { 0.0f, 0.0f };
        };

    layout_ = LayoutTreeBuilder::build(storage_, img_metrics);

    if (layout_) {
        LayoutTreeBuilder::Viewport vp{ viewport_w_, viewport_h_ };
        LayoutTreeBuilder::compute_layout(layout_.get(), vp, storage_,
            text_metrics_);
    }

    // 4. Display list.
    if (layout_)
        display_list_ = DisplayListBuilder::build(layout_.get(), storage_, base_url_, image_cache_);

    dom_dirty_ = false;

    if (on_layout_updated_) on_layout_updated_();
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

    std::function<void(const LayoutNode*)> hit =
        [&](const LayoutNode* n)
        {
            if (!n) return;
            const float ax = n->geometry.x;
            const float ay = n->geometry.y;
            const QRectF box(ax, ay, n->geometry.width, n->geometry.height);
            if (!box.contains(x, y)) return;

            if (n->style_soa_idx != UINT32_MAX) {
                target = storage_.dom_nodes[n->style_soa_idx];
            }
            for (const auto& c : n->children) hit(c.get());
        };
    hit(layout_.get());

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