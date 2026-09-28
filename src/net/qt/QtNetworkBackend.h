#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkDiskCache>

#include "../NetworkBackend.h"

namespace net {

    class QtNetworkBackend : public QObject, public NetworkBackend {
        Q_OBJECT
    public:
        explicit QtNetworkBackend(QObject* parent = nullptr);
        ~QtNetworkBackend() override;

        // --- NetworkBackend ---
        void setCallbacks(NetworkCallbacks cbs) override;
        void get(const Url& url) override;
        void request(const HttpRequest& req) override;
        void abort() override;
        void setUserAgent(const std::string& ua) override;
        void setCacheSize(uint64_t bytes) override;
        const char* backendName() const override { return "qt"; }

    private slots:
        void onReplyFinished();
        void onReplyProgress(qint64 received, qint64 total);

    private:
        void sendRequest(const HttpRequest& req);
        static QUrl toQUrl(const net::Url& u);
        static std::string parseCharsetFromContentType(const std::string& ct);

        QNetworkAccessManager* nam_{};
        QNetworkDiskCache* cache_{};
        QNetworkReply* current_{};
        NetworkCallbacks cbs_{};
        std::string userAgent_;
    };

} // namespace net