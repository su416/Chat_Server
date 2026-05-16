#include "ui/groupchatwindow.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShortcut>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>

GroupChatWindow::GroupChatWindow(QWidget *parent)
    : QWidget(parent),
      m_groupId(-1),
      m_titleLabel(nullptr),
      m_messageView(nullptr),
      m_inputEdit(nullptr),
      m_sendButton(nullptr)
{
    initUi();
}

void GroupChatWindow::initUi()
{
    setWindowTitle("Group Chat Window");
    resize(560, 460);

    m_titleLabel = new QLabel("Group Chat: ", this);

    m_messageView = new QTextEdit(this);
    m_messageView->setReadOnly(true);

    m_inputEdit = new QLineEdit(this);
    m_inputEdit->setPlaceholderText("Enter group message...");

    m_sendButton = new QPushButton("Send", this);

    auto *inputLayout = new QHBoxLayout();
    inputLayout->addWidget(m_inputEdit);
    inputLayout->addWidget(m_sendButton);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(m_titleLabel);
    mainLayout->addWidget(m_messageView);
    mainLayout->addLayout(inputLayout);

    connect(m_sendButton, &QPushButton::clicked, this, &GroupChatWindow::onSendClicked);

    // Ctrl+Enter 发送
    auto *shortcut = new QShortcut(QKeySequence("Ctrl+Return"), this);
    connect(shortcut, &QShortcut::activated, this, &GroupChatWindow::onSendClicked);
}

void GroupChatWindow::setGroupInfo(int groupId, const QString &groupName)
{
    m_groupId = groupId;
    m_groupName = groupName;

    setWindowTitle(QString("Group Chat - %1(%2)").arg(groupName).arg(groupId));
    m_titleLabel->setText(QString("Group Chat: %1 (id=%2)").arg(groupName).arg(groupId));
}

int GroupChatWindow::groupId() const
{
    return m_groupId;
}

void GroupChatWindow::appendOutgoingMessage(const QString &msg, const QString &time)
{
    m_messageView->append(QString("[%1] Me: %2").arg(time, msg));
}

void GroupChatWindow::appendIncomingMessage(const QString &senderName, const QString &msg, const QString &time)
{
    m_messageView->append(QString("[%1] %2: %3").arg(time, senderName, msg));
}

void GroupChatWindow::onSendClicked()
{
    QString text = m_inputEdit->text().trimmed();
    if (text.isEmpty() || m_groupId < 0)
    {
        return;
    }

    emit sendGroupMessageRequested(m_groupId, text);
    m_inputEdit->clear();
}

void GroupChatWindow::clearMessages()
{
    m_messageView->clear();
}