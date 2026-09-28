#include "QtNetworkBackend.h"

#include <QNetworkRequest>
#include <QNetworkCookieJar>
#include <QStandardPaths>
#include <QRegularExpression>

#include <algorithm>
#include <cctype>

namespace net {

    namespace {
        constexpr uint64_t kDefaultCacheSize = 50ull * 1024 * 1024;
        const char* kDefaultUA =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
            "AppleWebKit/537.36 (KHTML, like Gecko) "
            "MiniBrowser/0.1 Safari/537.36";
    }

    QtNetworkBackend::QtNetworkBackend(QObject* parent)
        : QObject(parent), userAgent_(kDefaultUA)
    {
        nam_ = new QNetworkAccessManager(this);

        cache_ = new QNetworkDiskCache(this);
        cache_->setCacheDirectory(
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
            + "/http");
        cache_->setMaximumCacheSize(kDefaultCacheSize);
        nam_->setCache(cache_);
        nam_->setCookieJar(new QNetworkCookieJar(nam_));
    }

    QtNetworkBackend::~QtNetworkBackend() { abort(); }

    void QtNetworkBackend::setCallbacks(NetworkCallbacks cbs) {
        cbs_ = std::move(cbs);
    }

    void QtNetworkBackend::setUserAgent(const std::string& ua) {
        userAgent_ = ua;
    }

    void QtNetworkBackend::setCacheSize(uint64_t bytes) {
        if (cache_) cache_->setMaximumCacheSize(qint64(bytes));
    }

    void QtNetworkBackend::abort() {
        QNetworkReply* reply = current_;
        current_ = nullptr;              // null ДО abort(), чтобы слот
        // onReplyFinished не пытался ничего обработать
        if (reply) {
            reply->disconnect(this);     // отключаем наши сигналы — не нужен частичный callback
            reply->abort();
            reply->deleteLater();
        }
    }

    QUrl QtNetworkBackend::toQUrl(const net::Url& u) {
        // Используем канонический текст — так не теряем percent-encoding и т.п.
        QUrl q = QUrl::fromEncoded(QByteArray::fromStdString(u.text));
        if (!q.isValid()) q = QUrl(QString::fromStdString(u.text));
        return q;
    }

    void QtNetworkBackend::get(const Url& url) {
        HttpRequest req;
        req.url = url;
        req.method = HttpMethod::Get;
        sendRequest(req);
    }

    void QtNetworkBackend::request(const HttpRequest& req) {
        sendRequest(req);
    }

    void QtNetworkBackend::sendRequest(const HttpRequest& req) {
        if (!req.url.valid()) {
            if (cbs_.onFailure)
                cbs_.onFailure(req.url, 0, "Invalid URL");
            return;
        }
        abort();

        QNetworkRequest q;
        q.setUrl(toQUrl(req.url));

        q.setHeader(QNetworkRequest::UserAgentHeader,
            QString::fromStdString(userAgent_));
        q.setRawHeader("Accept",
            "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
        q.setRawHeader("Accept-Language", "en-US,en;q=0.9,ru;q=0.8");

        // Пользовательские заголовки поверх дефолтных
        for (const auto& [k, v] : req.headers) {
            q.setRawHeader(QByteArray::fromStdString(k),
                QByteArray::fromStdString(v));
        }

        q.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
            QNetworkRequest::PreferCache);
        q.setAttribute(QNetworkRequest::CacheSaveControlAttribute, true);
        q.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
            QNetworkRequest::NoLessSafeRedirectPolicy);
        q.setMaximumRedirectsAllowed(int(req.max_redirects));

        if (req.timeout_ms > 0) q.setTransferTimeout(int(req.timeout_ms));

        switch (req.method) {
        case HttpMethod::Get:    current_ = nam_->get(q); break;
        case HttpMethod::Head:   current_ = nam_->head(q); break;
        case HttpMethod::Delete: current_ = nam_->deleteResource(q); break;
        case HttpMethod::Post:
        case HttpMethod::Put:
        case HttpMethod::Patch: {
            const QByteArray body(
                reinterpret_cast<const char*>(req.body.data()),
                qsizetype(req.body.size()));
            if (req.method == HttpMethod::Post)  current_ = nam_->post(q, body);
            else if (req.method == HttpMethod::Put) current_ = nam_->put(q, body);
            else current_ = nam_->sendCustomRequest(
                q, "PATCH", body);
            break;
        }
        }

        connect(current_, &QNetworkReply::finished,
            this, &QtNetworkBackend::onReplyFinished);
        connect(current_, &QNetworkReply::downloadProgress,
            this, &QtNetworkBackend::onReplyProgress);

        if (cbs_.onStarted) cbs_.onStarted(req.url);
    }

    void QtNetworkBackend::onReplyProgress(qint64 received, qint64 total) {
        if (cbs_.onProgress && total > 0)
            cbs_.onProgress(uint64_t(received), uint64_t(total));
    }

    std::string QtNetworkBackend::parseCharsetFromContentType(const std::string& ct) {
        static const QRegularExpression re(
            R"re(charset\s*=\s*"?([\w\-]+)"?)re",
            QRegularExpression::CaseInsensitiveOption);
            auto m = re.match(QString::fromStdString(ct));
            if (m.hasMatch()) return m.captured(1).toLower().toStdString();
            return "utf-8";
    }

    void QtNetworkBackend::onReplyFinished() {
        QNetworkReply* reply = current_;
        if (!reply) return;
        current_ = nullptr;

        HttpResponse resp;
        resp.status_code = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        resp.status_text = reply->attribute(
            QNetworkRequest::HttpReasonPhraseAttribute).toString().toStdString();
        resp.final_url.text = reply->url().toString().toStdString();

        // Заголовки
        for (const auto& pair : reply->rawHeaderPairs()) {
            resp.headers.emplace_back(
                pair.first.toStdString(),
                pair.second.toStdString());
        }

        // Content-Type / charset
        if (const std::string* ct = resp.findHeader("Content-Type")) {
            const size_t semi = ct->find(';');
            resp.content_type = (semi == std::string::npos)
                ? *ct
                : ct->substr(0, semi);
            // trim и lowercase
            while (!resp.content_type.empty() &&
                std::isspace((unsigned char)resp.content_type.back()))
                resp.content_type.pop_back();
            for (char& c : resp.content_type)
                c = (char)std::tolower((unsigned char)c);
            resp.charset = parseCharsetFromContentType(*ct);
        }

        // Тело
        const QByteArray body = reply->readAll();
        resp.body.assign(body.begin(), body.end());

        const auto netError = reply->error();
        const int http = resp.status_code;

        reply->deleteLater();

        // 4xx/5xx: приносим тело, если оно есть — рендерер покажет страницу ошибки.
        if (netError != QNetworkReply::NoError) {
            if (http >= 400 && !resp.body.empty()) {
                if (cbs_.onResponse) cbs_.onResponse(resp);
                return;
            }
            if (cbs_.onFailure)
                cbs_.onFailure(resp.final_url, http,
                    reply->errorString().toStdString());
            return;
        }

        if (cbs_.onResponse) cbs_.onResponse(resp);
    }

} // namespace net