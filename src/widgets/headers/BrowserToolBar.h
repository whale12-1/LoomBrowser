#pragma once

#include <QWidget>
#include <QToolButton>
#include <QLineEdit>
#include <QStyle>

class BrowserToolbar : public QWidget {
    Q_OBJECT
public:
    explicit BrowserToolbar(QWidget* parent = nullptr);

    void    setUrl(const QString& u);
    QString url() const;

    void setBackEnabled(bool e);
    void setForwardEnabled(bool e);
    void setHomeEnabled(bool e);

signals:
    void backRequested();
    void forwardRequested();
    void reloadRequested();
    void homeRequested();
    void urlEntered(const QString& url);

private slots:
    void onBack();
    void onForward();
    void onReload();
    void onHome();
    void onUrlEntered();

private:
    QToolButton* mkButton(QStyle::StandardPixmap icon, const QString& tip);

    QToolButton* back_{};
    QToolButton* forward_{};
    QToolButton* reload_{};
    QToolButton* home_{};
    QLineEdit* url_{};
};