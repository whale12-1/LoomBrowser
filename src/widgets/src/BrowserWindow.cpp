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

    // Текстовые метрики на основе Qt. Используют тот же шрифт,
    // что и BrowserCanvas для растеризации — layout и painter
    // совпадают по метрикам глифов.
    LayoutTreeBuilder::TextMetrics makeQtTextMetrics(const QWidget* widget) {
        LayoutTreeBuilder::TextMetrics m;
        const QFont base = widget->font();

        m.measure_width = [base](const std::string& text,
            float fs, bool bold) -> float
            {
                QFont f = base;
                f.setPixelSize(std::max(1, int(fs)));
                f.setBold(bold);
                QFontMetricsF fm(f);
                return float(fm.horizontalAdvance(QString::fromStdString(text)));
            };

        m.line_spacing = [base](float fs, bool bold) -> float {
            QFont f = base;
            f.setPixelSize(std::max(1, int(fs)));
            f.setBold(bold);
            QFontMetricsF fm(f);
            return float(fm.lineSpacing());
            };

        return m;
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
    connect(view_->canvas(), &BrowserCanvas::clicked,
        this, &BrowserWindow::onCanvasClicked);

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

    // Метрики берём от того же шрифта, что и canvas рисует —
    // тогда layout и painter не разойдутся по метрикам глифов.
    const auto tm = makeQtTextMetrics(view_->canvas());

    page_ = std::make_unique<Page>();
    page_->loadHtml(html, float(vw), float(vh), tm);

    view_->setDisplayList(page_->displayList());
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

    page_ = std::make_unique<Page>();
    page_->loadHtml(html, float(vw), float(vh), tm);

    view_->setDisplayList(page_->displayList());
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
void BrowserWindow::onCanvasClicked(float x, float y) {
    if (!page_) return;

    // dispatchClick может пометить DOM как dirty и вызвать relayout
    page_->dispatchClick(x, y);

    // Забираем актуальный кадр (даже если ничего не менялось —
    // это дешёвая операция, DisplayList уже в Page).
    view_->setDisplayList(page_->displayList());
}

void BrowserWindow::onReload() { fetchCurrent(); }
void BrowserWindow::onHome() { navigateTo(home_url_); }