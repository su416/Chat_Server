#include "ui/loginwindow.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

LoginWindow::LoginWindow(QWidget *parent)
    : QWidget(parent),
      m_titleLabel(nullptr),
      m_userIdLabel(nullptr),
      m_passwordLabel(nullptr),
      m_statusLabel(nullptr),
      m_userIdEdit(nullptr),
      m_passwordEdit(nullptr),
      m_loginButton(nullptr),
      m_registerButton(nullptr)
{
    initUi();
}

void LoginWindow::setStatusMessage(const QString &text)
{
    m_statusLabel->setText(text);
}

void LoginWindow::initUi()
{
    setWindowTitle("Qt Chat Client - Login");
    resize(420, 260);

    m_titleLabel = new QLabel("Chat Client Login", this);
    QFont titleFont;
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);
    m_titleLabel->setAlignment(Qt::AlignCenter);

    m_userIdLabel = new QLabel("User ID:", this);
    m_passwordLabel = new QLabel("Password:", this);
    m_statusLabel = new QLabel("Please enter your account information.", this);

    m_userIdEdit = new QLineEdit(this);
    m_userIdEdit->setPlaceholderText("Enter user id");

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setPlaceholderText("Enter password");
    m_passwordEdit->setEchoMode(QLineEdit::Password);

    m_loginButton = new QPushButton("Login", this);
    m_registerButton = new QPushButton("Register", this);

    auto *mainLayout = new QVBoxLayout(this);
    auto *userLayout = new QHBoxLayout();
    auto *passwordLayout = new QHBoxLayout();
    auto *buttonLayout = new QHBoxLayout();

    userLayout->addWidget(m_userIdLabel);
    userLayout->addWidget(m_userIdEdit);

    passwordLayout->addWidget(m_passwordLabel);
    passwordLayout->addWidget(m_passwordEdit);

    buttonLayout->addStretch();
    buttonLayout->addWidget(m_loginButton);
    buttonLayout->addWidget(m_registerButton);
    buttonLayout->addStretch();

    mainLayout->addSpacing(10);
    mainLayout->addWidget(m_titleLabel);
    mainLayout->addSpacing(20);
    mainLayout->addLayout(userLayout);
    mainLayout->addLayout(passwordLayout);
    mainLayout->addSpacing(10);
    mainLayout->addLayout(buttonLayout);
    mainLayout->addSpacing(10);
    mainLayout->addWidget(m_statusLabel);
    mainLayout->addStretch();

    setLayout(mainLayout);

    connect(m_loginButton, &QPushButton::clicked, this, &LoginWindow::onLoginClicked);
    connect(m_registerButton, &QPushButton::clicked, this, &LoginWindow::onRegisterClicked);
}

void LoginWindow::onLoginClicked()
{
    const QString userId = m_userIdEdit->text().trimmed();
    const QString password = m_passwordEdit->text();

    if (userId.isEmpty() || password.isEmpty())
    {
        m_statusLabel->setText("User ID or password cannot be empty.");
        return;
    }
m_statusLabel->setText("Connecting to server...");
    emit loginRequested(userId, password);
}

void LoginWindow::onRegisterClicked()
{
    m_statusLabel->setText("Register dialog will be added next.");
    emit registerRequested();
}
