#include "ui/chatwindow.h"

#include <QDateTime>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShortcut>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>

ChatWindow::ChatWindow(QWidget *parent)
    : QWidget(parent),
      m_peerId(-1),
      m_titleLabel(nullptr),
      m_messageView(nullptr),
      m_inputEdit(nullptr),
      m_sendButton(nullptr)
{
    initUi();
}

void ChatWindow::initUi()
{
    setWindowTitle("Chat Window");
    resize(560, 460);

    m_titleLabel = new QLabel("Chat with: ", this);

    m_messageView = new QTextEdit(this);
    m_messageView->setReadOnly(true);

    m_inputEdit = new QLineEdit(this);
    m_inputEdit->setPlaceholderText("Enter message...");

    m_sendButton = new QPushButton("Send", this);

    auto *inputLayout = new QHBoxLayout();
    inputLayout->addWidget(m_inputEdit);
    inputLayout->addWidget(m_sendButton);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(m_titleLabel);
    mainLayout->addWidget(m_messageView);
    mainLayout->addLayout(inputLayout);

    connect(m_sendButton, &QPushButton::clicked, this, &ChatWindow::onSendClicked);

    // Ctrl+Enter 发送
    auto *shortcut = new QShortcut(QKeySequence("Ctrl+Return"), this);
    connect(shortcut, &QShortcut::activated, this, &ChatWindow::onSendClicked);
}

void ChatWindow::setChatPeer(int peerId, const QString &peerName)
{
    m_peerId = peerId;
    m_peerName = peerName;

    setWindowTitle(QString("Chat - %1(%2)").arg(peerName).arg(peerId));
    m_titleLabel->setText(QString("Chat with: %1 (id=%2)").arg(peerName).arg(peerId));
}

int ChatWindow::peerId() const
{
    return m_peerId;
}


void ChatWindow::clearMessages()
{
    m_messageView->clear();
}

void ChatWindow::appendOutgoingMessage(const QString &msg, const QString &time)
{
    m_messageView->append(QString("[%1] Me: %2").arg(time, msg));
}

void ChatWindow::appendIncomingMessage(const QString &senderName, const QString &msg, const QString &time)
{
    m_messageView->append(QString("[%1] %2: %3").arg(time, senderName, msg));
}

void ChatWindow::onSendClicked()
{
    QString text = m_inputEdit->text().trimmed();
    if (text.isEmpty() || m_peerId < 0)
    {
        return;
    }

    emit sendMessageRequested(m_peerId, text);
    m_inputEdit->clear();
}
