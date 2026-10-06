#include "../widgets/headers/BrowserWindow.h"
#include "../net/TextCodec.h"
#include "../parsers/selector_matcher/display_list_builder.h"
#include "../parsers/arena_memory_allocator/headers/arena.h"
#include "../parsers/css_parser/headers/css_parser.h"
#include "../parsers/html_parser/headers/html_parser.h"
#include "../parsers/selector_matcher/style_tree_builder.h"
#include "../parsers/selector_matcher/user_agent_stylesheet.h"   // в начале файла
#include "../parsers/selector_matcher/style_origin.h"
#include "../layout/headers/layout_tree_builder.h"
#include <functional>
#include <QVBoxLayout>
#include <QStatusBar>
#include <QLabel>
#include <QDebug>
namespace {

    LayoutTreeBuilder::TextMetrics makeQtTextMetrics(const QWidget* widget) {
        LayoutTreeBuilder::TextMetrics m;
        const QFont base = widget->font();

        m.measure_width = [base](const std::string& text,
            float fs, bool bold) -> float {
                QFont f = base;
                f.setPixelSize(std::max(1, int(fs)));              // без dpr
                f.setBold(bold);
                QFontMetricsF fm(f);
                return float(fm.horizontalAdvance(QString::fromStdString(text)));
            };

        m.line_spacing = [base](float fs, bool bold) -> float {
            QFont f = base;
            f.setPixelSize(std::max(1, int(fs)));              // без dpr
            f.setBold(bold);
            QFontMetricsF fm(f);
            return float(fm.lineSpacing());
            };

        return m;
    }

    // Первый Element в поддереве (пропускаем Document/Text/Comment)
    DOMNode* firstElement(DOMNode* n) {
        if (!n) return nullptr;
        if (n->type == NodeType::Element) return n;
        for (DOMNode* c : n->children)
            if (auto* e = firstElement(c)) return e;
        return nullptr;
    }

    // Собрать текст всех <style>…</style>
    void collectStyleText(DOMNode* n, std::string& out) {
        if (!n) return;
        if (n->type == NodeType::Element && n->tag_name == "style") {
            for (DOMNode* c : n->children)
                if (c->type == NodeType::Text) out += c->text_content;
            return;
        }
        for (DOMNode* c : n->children) collectStyleText(c, out);
    }

    // Полный пайплайн: HTML (+ встроенный CSS из <style>) → DisplayList.
    // Арена живёт внутри функции; DisplayList не держит ссылок на неё
    // (только heap-скопированные строки), поэтому безопасно возвращать
    DisplayList renderHtml(const std::string& html,
        float viewport_width,
        float viewport_height,
        const LayoutTreeBuilder::TextMetrics& tm)
    {
        ArenaAllocator arena;
        HTMLParser hp(arena);
        DOMNode* doc = hp.parse(html);
        DOMNode* root = firstElement(doc);
        if (!root) return {};

        std::string author_css;
        collectStyleText(doc, author_css);

#ifdef _DEBUG
        qDebug() << "[render] html:" << html.size()
            << "css:" << author_css.size();
#endif

        CSSParser cp(arena);
        StyleSheet* ua_sheet = cp.parse(ua_css::kSource);
        StyleSheet* author_sheet = cp.parse(author_css);

        std::vector<StyleRuleIndex::SheetRef> sources = {
            { ua_sheet,     Origin::UserAgent },
            { author_sheet, Origin::Author    },
        };
        StyleStorageSoA storage = StyleTreeBuilder::build(root, sources);

        for (uint32_t i = 0; i < storage.size(); ++i) {
            const auto* dom = storage.dom_nodes[i];
            if (!dom) continue;
            if (dom->tag_name == "h1" || dom->tag_name == "p" || dom->tag_name == "button") {
                qDebug() << "[style]" << QString::fromStdString(dom->tag_name)
                    << "font-size:" << storage.font_sizes[i]
                    << "font-weight:" << storage.font_weights[i];
            }
        }

        auto layout = LayoutTreeBuilder::build(storage);
        if (!layout) return {};

        LayoutTreeBuilder::Viewport vp{ viewport_width, viewport_height };
        LayoutTreeBuilder::compute_layout(layout.get(), vp, storage, tm);   // ← один вызов

#ifdef _DEBUG
        // ... dump (опционально)
#endif

        return DisplayListBuilder::build(layout.get(), storage);
    }

} // namespace


BrowserWindow::BrowserWindow(std::unique_ptr<net::NetworkBackend> backend,
    QWidget* parent) : QMainWindow(parent), net_(std::move(backend)) {
    if (!net_) {
        net_ = net::makeNetworkBackend("qt");
    }

    setWindowTitle("MiniBrowser");

    toolbar_ = new BrowserToolbar(this);
    view_ = new BrowserView(this);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(toolbar_);
    layout->addWidget(view_, /*stretch=*/1);
    setCentralWidget(central);

    status_label_ = new QLabel("Ready", this);
    statusBar()->addPermanentWidget(status_label_);

    connect(toolbar_, &BrowserToolbar::backRequested, this, &BrowserWindow::onBack);
    connect(toolbar_, &BrowserToolbar::forwardRequested, this, &BrowserWindow::onForward);
    connect(toolbar_, &BrowserToolbar::reloadRequested, this, &BrowserWindow::onReload);
    connect(toolbar_, &BrowserToolbar::homeRequested, this, &BrowserWindow::onHome);
    connect(toolbar_, &BrowserToolbar::urlEntered, this, &BrowserWindow::onUrlEntered);

    installBackendCallbacks();
    resize(1200, 800);
}

void BrowserWindow::setContent(const DisplayList& list, const QString& url) {
    view_->setDisplayList(list);
    if (!url.isEmpty()) toolbar_->setUrl(url);
    status_label_->setText(
        QString("Page: %1 × %2")
        .arg(int(list.document_width))
        .arg(int(list.document_height)));
}


void BrowserWindow::installBackendCallbacks() {
    net::NetworkCallbacks cbs;

    cbs.onStarted = [this](const net::Url& u) {
        status_label_->setText(
            QString("Loading %1 …").arg(QString::fromStdString(u.text)));
        };

    cbs.onProgress = [this](uint64_t received, uint64_t total) {
        status_label_->setText(
            QString("Loading %1 / %2 KB")
            .arg(received / 1024).arg(total / 1024));
        };

    cbs.onResponse = [this](const net::HttpResponse& resp) {
        onHttpResponse(resp);
        };

    cbs.onFailure = [this](const net::Url& u, int status, const std::string& err) {
        onHttpFailure(u, status, err);
        };

    net_->setCallbacks(std::move(cbs));
}

void BrowserWindow::onUrlEntered(const QString& text) {
    navigateTo(text);
}

void BrowserWindow::navigateTo(const QString& input) {
    net::Url u = net::Url::normalize(input.toStdString());
    if (!u.valid()) {
        status_label_->setText("Invalid URL");
        return;
    }
    pushHistory(u);
    fetchCurrent();
}

void BrowserWindow::fetchCurrent() {
    if (history_pos_ < 0) return;
    const net::Url& u = history_[history_pos_];
    toolbar_->setUrl(QString::fromStdString(u.text));
    setWindowTitle(QString::fromStdString(u.text) + " — MiniBrowser");
    net_->get(u);
}

void BrowserWindow::onHttpResponse(const net::HttpResponse& resp) {
    const std::string html = net::decodeToUtf8(resp.body, resp.charset);

    const int vw = view_->viewport()->width();
    const int vh = view_->viewport()->height();

    // Метрики берём от того же шрифта, что и canvas рисует.
    const auto tm = makeQtTextMetrics(view_->canvas());

    DisplayList list = renderHtml(html, float(vw), float(vh), tm);

    view_->setDisplayList(list);
    status_label_->setText(
        QString("Loaded %1 KB — HTTP %2")
        .arg(resp.body.size() / 1024)
        .arg(resp.status_code));
}


void BrowserWindow::onHttpFailure(const net::Url& url, int status,
    const std::string& error)
{
    const QString title = status > 0
        ? QString("HTTP %1").arg(status)
        : "Load error";
    showErrorPage(title,
        QString::fromStdString(error)
        + "\nURL: " + QString::fromStdString(url.text));
}


void BrowserWindow::showErrorPage(const QString& title, const QString& details) {
    const std::string html =
        "<html><head><style>"
        "body { font-family: sans-serif; padding: 2em; "
        "       background-color: #ffffff; }"
        "h1   { color: #c0392b; font-size: 24px; margin-bottom: 1em; }"
        "p    { color: #555555; white-space: pre-wrap; }"
        "</style></head><body>"
        "<h1>" + title.toStdString() + "</h1>"
        "<p>" + details.toHtmlEscaped().toStdString() + "</p>"
        "</body></html>";

    const int vw = view_->viewport()->width();
    const int vh = view_->viewport()->height();

    const auto tm = makeQtTextMetrics(view_->canvas());

    DisplayList list = renderHtml(html, float(vw), float(vh), tm);
    view_->setDisplayList(list);
    status_label_->setText(title);
}


void BrowserWindow::pushHistory(const net::Url& url) {
    // Обрезаем «future» — как в браузерах, когда после back делают новый переход
    if (history_pos_ + 1 < (int)history_.size()) {
        history_.resize(history_pos_ + 1);
    }
    history_.push_back(url);
    history_pos_ = (int)history_.size() - 1;
    updateNavigationButtons();
}

void BrowserWindow::updateNavigationButtons() {
    toolbar_->setBackEnabled(history_pos_ > 0);
    toolbar_->setForwardEnabled(history_pos_ + 1 < (int)history_.size());
    toolbar_->setHomeEnabled(!home_url_.isEmpty());
}


void BrowserWindow::onBack() {
    if (history_pos_ > 0) {
        --history_pos_;
        fetchCurrent();
        updateNavigationButtons();
    }
}

void BrowserWindow::onForward() {
    if (history_pos_ + 1 < (int)history_.size()) {
        ++history_pos_;
        fetchCurrent();
        updateNavigationButtons();
    }
}

void BrowserWindow::onReload() { fetchCurrent(); }
void BrowserWindow::onHome() { navigateTo(home_url_); }