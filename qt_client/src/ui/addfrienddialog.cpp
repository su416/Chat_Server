#include "ui/addfrienddialog.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

AddFriendDialog::AddFriendDialog(QWidget *parent)
    : QDialog(parent),
      m_titleLabel(nullptr),
      m_friendIdLabel(nullptr),
      m_statusLabel(nullptr),
      m_friendIdEdit(nullptr),
      m_submitButton(nullptr),
      m_cancelButton(nullptr)
{
    initUi();
}

void AddFriendDialog::initUi()
{
    setWindowTitle("Add Friend");
    resize(340, 180);
    setModal(true);

    m_titleLabel = new QLabel("Add New Friend", this);
    QFont titleFont;
    titleFont.setPointSize(14);
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);
    m_titleLabel->setAlignment(Qt::AlignCenter);

    m_friendIdLabel = new QLabel("Friend ID:", this);
    m_statusLabel = new QLabel("Please enter friend id.", this);

    m_friendIdEdit = new QLineEdit(this);
    m_friendIdEdit->setPlaceholderText("Enter friend id");

    m_submitButton = new QPushButton("Submit", this);
    m_cancelButton = new QPushButton("Cancel", this);

    auto *mainLayout = new QVBoxLayout(this);
    auto *inputLayout = new QHBoxLayout();
    auto *buttonLayout = new QHBoxLayout();

    inputLayout->addWidget(m_friendIdLabel);
    inputLayout->addWidget(m_friendIdEdit);

    buttonLayout->addStretch();
    buttonLayout->addWidget(m_submitButton);
    buttonLayout->addWidget(m_cancelButton);
    buttonLayout->addStretch();

    mainLayout->addWidget(m_titleLabel);
    mainLayout->addSpacing(15);
    mainLayout->addLayout(inputLayout);
    mainLayout->addSpacing(10);
    mainLayout->addLayout(buttonLayout);
    mainLayout->addWidget(m_statusLabel);

    connect(m_submitButton, &QPushButton::clicked, this, &AddFriendDialog::onSubmitClicked);
    connect(m_cancelButton, &QPushButton::clicked, this, &AddFriendDialog::reject);
}

void AddFriendDialog::setStatusMessage(const QString &text)
{
    m_statusLabel->setText(text);
}

void AddFriendDialog::onSubmitClicked()
{
    bool ok = false;
    int friendId = m_friendIdEdit->text().trimmed().toInt(&ok);

    if (!ok || friendId <= 0)
    {
        m_statusLabel->setText("Friend ID must be a positive number.");
        return;
    }

    m_statusLabel->setText("Sending add friend request...");
    emit addFriendSubmitted(friendId);
}

void AddFriendDialog::clearInputs()
{
    m_friendIdEdit->clear();
}

