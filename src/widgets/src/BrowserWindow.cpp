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
#include <QImage>
#include <QFontDatabase>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QSvgRenderer>
#include <QPainter>

namespace {

    // Текстовые метрики на основе Qt. Используют тот же шрифт,
    // что и BrowserCanvas для растеризации — layout и painter
    // совпадают по метрикам глифов.
    LayoutTreeBuilder::TextMetrics makeQtTextMetrics(const QWidget* widget) {
        LayoutTreeBuilder::TextMetrics m;
        const QFont base = widget->font();

        auto make_font = [base](float fs, const std::string& family, bool bold, bool italic) {
            QFont f;
            if (!family.empty()) {
                QStringList families = QString::fromStdString(family).split(",", Qt::SkipEmptyParts);
                for (QString& fam : families) {
                    fam = fam.trimmed();
                    if (fam.startsWith("\"") && fam.endsWith("\""))
                        fam = fam.mid(1, fam.size() - 2);
                    else if (fam.startsWith("'") && fam.endsWith("'"))
                        fam = fam.mid(1, fam.size() - 2);
                }
                QString chosen = "";
                for (const QString& fam : families) {
                    if (fam.compare("sans-serif", Qt::CaseInsensitive) == 0) { chosen = "Arial"; break; }
                    if (fam.compare("serif", Qt::CaseInsensitive) == 0) { chosen = "Times New Roman"; break; }
                    if (fam.compare("monospace", Qt::CaseInsensitive) == 0) { chosen = "Courier New"; break; }
                    if (QFontDatabase::families().contains(fam, Qt::CaseInsensitive)) {
                        chosen = fam; break;
                    }
                }
                if (chosen.isEmpty() && !families.isEmpty()) chosen = families[0];
                if (!chosen.isEmpty()) f.setFamily(chosen);
                else f = base;
            }
            else {
                f = base;
            }
            f.setPixelSize(std::max(1, int(fs)));
            f.setBold(bold);
            f.setItalic(italic);
            return f;
            };

        m.measure_width = [make_font](const std::string& text, float fs, const std::string& family, bool bold, bool italic) -> float {
            QFont f = make_font(fs, family, bold, italic);
            return float(QFontMetricsF(f).horizontalAdvance(QString::fromStdString(text)));
            };
        m.line_spacing = [make_font](float fs, const std::string& family, bool bold) -> float {
            return float(QFontMetricsF(make_font(fs, family, bold, false)).lineSpacing());
            };
        m.ascent = [make_font](float fs, const std::string& family, bool bold) -> float {
            return float(QFontMetricsF(make_font(fs, family, bold, false)).ascent());
            };
        m.descent = [make_font](float fs, const std::string& family, bool bold) -> float {
            return float(QFontMetricsF(make_font(fs, family, bold, false)).descent());
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
    net_manager_ = new QNetworkAccessManager(this);
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
    const auto tm = makeQtTextMetrics(view_->canvas());

    std::string base_url = "";
    if (history_pos_ >= 0 && history_pos_ < (int)history_.size()) {
        base_url = history_[history_pos_].text;
    }

    // Лямбда, которая скачивает CSS
    auto fetch_resource = [base_url](const std::string& href) -> std::string {
        QUrl base_qurl(QString::fromStdString(base_url));
        QUrl abs_qurl = base_qurl.resolved(QUrl(QString::fromStdString(href)));
        std::string abs_url = abs_qurl.toString().toStdString();

        qDebug() << "[CSS] Fetching:" << QString::fromStdString(abs_url);

        // Создаем локальный менеджер, чтобы избежать конфликтов
        QNetworkAccessManager nam;
        QNetworkRequest req(QUrl(QString::fromStdString(abs_url)));
        req.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");

        QNetworkReply* reply = nam.get(req);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        if (reply->error() == QNetworkReply::NoError) {
            QByteArray data = reply->readAll();
            qDebug() << "[CSS] Downloaded bytes:" << data.size();
            reply->deleteLater();
            return std::string(data.constData(), data.size());
        }

        qDebug() << "[CSS] Error:" << reply->errorString();
        reply->deleteLater();
        return "";
        };


    // Лямбда для асинхронной загрузки картинок
    auto fetch_image_async = [this](const std::string& url, std::function<void(QImage)> on_loaded) {
        QNetworkRequest req(QUrl(QString::fromStdString(url)));
        req.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
        QNetworkReply* reply = net_manager_->get(req);

        QObject::connect(reply, &QNetworkReply::finished, [reply, on_loaded, url]() {
            if (reply->error() == QNetworkReply::NoError) {
                QByteArray data = reply->readAll();
                QImage img;

                // 1. Пытаемся загрузить как обычную картинку (PNG, JPG)
                if (img.loadFromData(data)) {
                    qDebug() << "[IMG] Loaded standard:" << QString::fromStdString(url) << img.width() << "x" << img.height();
                    on_loaded(img);
                }
                // 2. Если не вышло и это SVG — используем QSvgRenderer
                else if (url.find(".svg") != std::string::npos) {
                    QSvgRenderer renderer(data);
                    if (renderer.isValid()) {
                        QSize size = renderer.defaultSize();
                        if (size.isEmpty()) size = QSize(18, 18);

                        // ИСПОЛЬЗУЕМ PREMULTIPLIED ДЛЯ ПРАВИЛЬНОЙ ПРОЗРАЧНОСТИ
                        img = QImage(size, QImage::Format_ARGB32_Premultiplied);
                        img.fill(Qt::transparent);

                        QPainter p(&img);
                        p.setCompositionMode(QPainter::CompositionMode_Source);
                        p.fillRect(img.rect(), Qt::transparent);
                        p.setCompositionMode(QPainter::CompositionMode_SourceOver);

                        renderer.render(&p);
                        p.end();

                        qDebug() << "[IMG] Loaded SVG:" << QString::fromStdString(url) << img.width() << "x" << img.height();
                        on_loaded(img);
                    }
                }
                // 3. Если картинка битая (например, s.gif), отдаём прозрачный пиксель
                else {
                    qDebug() << "[IMG] Failed to parse:" << QString::fromStdString(url) << " - creating transparent placeholder.";
                    img = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
                    img.fill(Qt::transparent);
                    on_loaded(img);
                }
            }
            else {
                qDebug() << "[IMG] Network Error:" << reply->errorString() << QString::fromStdString(url);
                QImage img(1, 1, QImage::Format_ARGB32_Premultiplied);
                img.fill(Qt::transparent);
                on_loaded(img);
            }
            reply->deleteLater();
            });
        };

    page_ = std::make_unique<Page>();
    // ДОБАВЬ ЭТУ СТРОКУ:
    page_->on_layout_updated_ = [this]() {
        if (page_) {
            view_->setDisplayList(page_->displayList());
        }
        };

    page_->loadHtml(html, base_url, fetch_resource, fetch_image_async, float(vw), float(vh), tm);

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
    page_->loadHtml(html, "", nullptr, nullptr, float(vw), float(vh), tm);

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