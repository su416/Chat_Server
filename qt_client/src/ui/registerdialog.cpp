#include "ui/registerdialog.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

RegisterDialog::RegisterDialog(QWidget *parent)
    : QDialog(parent),
      m_titleLabel(nullptr),
      m_nameLabel(nullptr),
      m_passwordLabel(nullptr),
      m_statusLabel(nullptr),
      m_nameEdit(nullptr),
      m_passwordEdit(nullptr),
      m_submitButton(nullptr),
      m_cancelButton(nullptr)
{
    initUi();
}

void RegisterDialog::initUi()
{
    setWindowTitle("Register");
    resize(380, 220);
    setModal(true);

    m_titleLabel = new QLabel("Create New User", this);
    QFont titleFont;
    titleFont.setPointSize(14);
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);
    m_titleLabel->setAlignment(Qt::AlignCenter);

    m_nameLabel = new QLabel("User Name:", this);
    m_passwordLabel = new QLabel("Password:", this);
    m_statusLabel = new QLabel("Please enter registration information.", this);

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText("Enter user name");

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setPlaceholderText("Enter password");
    m_passwordEdit->setEchoMode(QLineEdit::Password);

    m_submitButton = new QPushButton("Submit", this);
    m_cancelButton = new QPushButton("Cancel", this);

    auto *mainLayout = new QVBoxLayout(this);
    auto *nameLayout = new QHBoxLayout();
    auto *passwordLayout = new QHBoxLayout();
    auto *buttonLayout = new QHBoxLayout();

    nameLayout->addWidget(m_nameLabel);
    nameLayout->addWidget(m_nameEdit);

    passwordLayout->addWidget(m_passwordLabel);
    passwordLayout->addWidget(m_passwordEdit);

    buttonLayout->addStretch();
    buttonLayout->addWidget(m_submitButton);
    buttonLayout->addWidget(m_cancelButton);
    buttonLayout->addStretch();

    mainLayout->addWidget(m_titleLabel);
    mainLayout->addSpacing(15);
    mainLayout->addLayout(nameLayout);
    mainLayout->addLayout(passwordLayout);
    mainLayout->addSpacing(10);
    mainLayout->addLayout(buttonLayout);
    mainLayout->addWidget(m_statusLabel);

    connect(m_submitButton, &QPushButton::clicked, this, &RegisterDialog::onSubmitClicked);
    connect(m_cancelButton, &QPushButton::clicked, this, &RegisterDialog::reject);
}

void RegisterDialog::setStatusMessage(const QString &text)
{
    m_statusLabel->setText(text);
}

void RegisterDialog::onSubmitClicked()
{
    const QString name = m_nameEdit->text().trimmed();
    const QString password = m_passwordEdit->text();

    if (name.isEmpty() || password.isEmpty())
    {
        m_statusLabel->setText("User name or password cannot be empty.");
        return;
    }

    m_statusLabel->setText("Sending register request...");
    emit registerSubmitted(name, password);
}


void RegisterDialog::clearInputs()
{
    m_nameEdit->clear();
    m_passwordEdit->clear();
}
