#pragma once

#include <QMainWindow>
#include <QString>
#include <memory>
#include <vector>

#include "BrowserToolbar.h"
#include "BrowserView.h"
#include "../net/NetworkBackend.h"
#include "../net/NetworkBackendFactory.h"
#include "Page.h"

class QLabel;

class BrowserWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit BrowserWindow(std::unique_ptr<net::NetworkBackend> backend = nullptr,
        QWidget* parent = nullptr);

    void navigateTo(const QString& input);
    void setContent(const DisplayList& list, const QString& url = {});

private slots:
    void onUrlEntered(const QString& text);
    void onCanvasClicked(float x, float y);

private:
    void onBack();
    void onForward();
    void onReload();
    void onHome();

    void installBackendCallbacks();
    void fetchCurrent();
    void pushHistory(const net::Url& url);
    void updateNavigationButtons();
    void onHttpResponse(const net::HttpResponse& resp);
    void onHttpFailure(const net::Url& url, int status,
        const std::string& error);
    void showErrorPage(const QString& title, const QString& details);

    BrowserToolbar* toolbar_{};
    BrowserView* view_{};
    QLabel* status_label_{};

    std::unique_ptr<net::NetworkBackend> net_;

    std::vector<net::Url> history_;
    int  history_pos_ = -1;
    QString home_url_{ "about:blank" };

    std::unique_ptr<Page> page_;
};