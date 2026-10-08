#include "./headers/BrowserToolBar.h"

#include <QHBoxLayout>

BrowserToolbar::BrowserToolbar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(36);

    back_ = mkButton(QStyle::SP_ArrowBack, "Back");
    forward_ = mkButton(QStyle::SP_ArrowForward, "Forward");
    reload_ = mkButton(QStyle::SP_BrowserReload, "Reload");
    home_ = mkButton(QStyle::SP_DirHomeIcon, "Home");

    forward_->setEnabled(false);
    home_->setEnabled(false);

    url_ = new QLineEdit(this);
    url_->setPlaceholderText("Enter URL or search…");
    url_->setClearButtonEnabled(true);
    url_->setMinimumWidth(200);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(4);
    layout->addWidget(back_);
    layout->addWidget(forward_);
    layout->addWidget(reload_);
    layout->addWidget(home_);
    layout->addSpacing(6);
    layout->addWidget(url_, /*stretch=*/1);

    connect(back_, &QToolButton::clicked, this, &BrowserToolbar::onBack);
    connect(forward_, &QToolButton::clicked, this, &BrowserToolbar::onForward);
    connect(reload_, &QToolButton::clicked, this, &BrowserToolbar::onReload);
    connect(home_, &QToolButton::clicked, this, &BrowserToolbar::onHome);
    connect(url_, &QLineEdit::returnPressed, this, &BrowserToolbar::onUrlEntered);
}

void BrowserToolbar::setUrl(const QString& u) { url_->setText(u); }
QString BrowserToolbar::url() const { return url_->text(); }

void BrowserToolbar::setBackEnabled(bool e) { back_->setEnabled(e); }
void BrowserToolbar::setForwardEnabled(bool e) { forward_->setEnabled(e); }
void BrowserToolbar::setHomeEnabled(bool e) { home_->setEnabled(e); }

void BrowserToolbar::onBack() { emit backRequested(); }
void BrowserToolbar::onForward() { emit forwardRequested(); }
void BrowserToolbar::onReload() { emit reloadRequested(); }
void BrowserToolbar::onHome() { emit homeRequested(); }
void BrowserToolbar::onUrlEntered() { emit urlEntered(url_->text()); }

QToolButton* BrowserToolbar::mkButton(QStyle::StandardPixmap icon,
    const QString& tip) {
    auto* b = new QToolButton(this);
    b->setIcon(style()->standardIcon(icon));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setIconSize(QSize(18, 18));
    return b;
}