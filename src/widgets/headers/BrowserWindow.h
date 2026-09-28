#pragma once

#include <QMainWindow>
#include <memory>
#include "BrowserToolbar.h"
#include "BrowserView.h"
#include "../net/NetworkBackend.h"
#include "../net/NetworkBackendFactory.h"

class BrowserWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit BrowserWindow(std::unique_ptr<net::NetworkBackend> backend = nullptr,
        QWidget* parent = nullptr); 

    void navigateTo(const QString& input);
    void setContent(const DisplayList& list, const QString& url = {});

private slots:
    void onUrlEntered(const QString& text);

private:
    void onBack();
    void onForward();
    void onReload();
    void onHome();

    void installBackendCallbacks();
    void fetchCurrent();
    void pushHistory(const net::Url& url);   // ← добавить
    void updateNavigationButtons();          // ← добавить
    void onHttpResponse(const net::HttpResponse& resp);   // ← добавить
    void onHttpFailure(const net::Url& url, int status,
        const std::string& error);          // ← добавить
    void showErrorPage(const QString& title, const QString& details); // ← добавить

    BrowserToolbar* toolbar_{};
    BrowserView* view_{};
    QLabel* status_label_{};

    std::unique_ptr<net::NetworkBackend> net_;      // неполный тип — ок, unique_ptr

    std::vector<net::Url> history_;                 // ← <vector> и полный Url
    int history_pos_ = -1;
    QString home_url_{ "about:blank" };
};