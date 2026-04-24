#include "ui/mainwindow.h"
#include <QListWidgetItem>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_userInfoLabel(nullptr),
      m_statusLabel(nullptr),
      m_friendListWidget(nullptr),
      m_groupListWidget(nullptr),
      m_addFriendButton(nullptr),
      m_createGroupButton(nullptr),
      m_joinGroupButton(nullptr),
      m_logoutButton(nullptr)
{
    initUi();
}

void MainWindow::initUi()
{
    setWindowTitle("Qt Chat Client - Main");
    resize(820, 520);

    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    m_userInfoLabel = new QLabel("Current User: not logged in", this);
    m_statusLabel = new QLabel("Login success. Main window initialized.", this);

    m_friendListWidget = new QListWidget(this);
    m_groupListWidget = new QListWidget(this);

    m_addFriendButton = new QPushButton("Add Friend", this);
    m_createGroupButton = new QPushButton("Create Group", this);
    m_joinGroupButton = new QPushButton("Join Group", this);
    m_logoutButton = new QPushButton("Logout", this);

    auto *friendGroup = new QGroupBox("Friends", this);
    auto *groupGroup = new QGroupBox("Groups", this);

    auto *friendLayout = new QVBoxLayout();
    friendLayout->addWidget(m_friendListWidget);
    friendGroup->setLayout(friendLayout);

    auto *groupLayout = new QVBoxLayout();
    groupLayout->addWidget(m_groupListWidget);
    groupGroup->setLayout(groupLayout);

    auto *listLayout = new QHBoxLayout();
    listLayout->addWidget(friendGroup);
    listLayout->addWidget(groupGroup);

    auto *buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(m_addFriendButton);
    buttonLayout->addWidget(m_createGroupButton);
    buttonLayout->addWidget(m_joinGroupButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_logoutButton);

    auto *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->addWidget(m_userInfoLabel);
    mainLayout->addLayout(listLayout);
    mainLayout->addLayout(buttonLayout);
    mainLayout->addWidget(m_statusLabel);

      connect(m_addFriendButton, &QPushButton::clicked, this, [this]() {
        emit addFriendRequested();
    });

       connect(m_createGroupButton, &QPushButton::clicked, this, [this]() {
        emit createGroupRequested();
    });

        connect(m_joinGroupButton, &QPushButton::clicked, this, [this]() {
        emit joinGroupRequested();
    }); 

   connect(m_logoutButton, &QPushButton::clicked, this, [this]() {
    emit logoutRequested();
});

    connect(m_friendListWidget, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) {
        if (!item)
        {
            return;
        }

        int friendId = item->data(Qt::UserRole).toInt();
        QString friendName = item->data(Qt::UserRole + 1).toString();

        if (friendId > 0)
        {
            emit friendChatRequested(friendId, friendName);
        }
    });

        connect(m_groupListWidget, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) {
        if (!item)
        {
            return;
        }

        int groupId = item->data(Qt::UserRole).toInt();
        QString groupName = item->data(Qt::UserRole + 1).toString();

        if (groupId > 0)
        {
            emit groupChatRequested(groupId, groupName);
        }
    });
}

QString MainWindow::parseFriendItem(const QJsonValue &value) const
{
    QJsonObject obj;

    if (value.isString())
    {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(value.toString().toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
        {
            obj = doc.object();
        }
    }
    else if (value.isObject())
    {
        obj = value.toObject();
    }

    if (obj.isEmpty())
    {
        return value.toString();
    }

    int id = obj.value("id").toInt();
    QString name = obj.value("name").toString();
    QString state = obj.value("state").toString();

    if (state.isEmpty())
    {
        return QString("%1 %2").arg(id).arg(name);
    }

    return QString("%1 %2 %3").arg(id).arg(name).arg(state);
}

QString MainWindow::parseGroupItem(const QJsonValue &value) const
{
    QJsonObject obj;

    if (value.isString())
    {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(value.toString().toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
        {
            obj = doc.object();
        }
    }
    else if (value.isObject())
    {
        obj = value.toObject();
    }

    if (obj.isEmpty())
    {
        return value.toString();
    }

    int id = obj.contains("id") ? obj.value("id").toInt()
                                : obj.value("groupid").toInt();
    QString groupName = obj.value("groupname").toString();
    QString groupDesc = obj.value("groupdesc").toString();

    if (groupDesc.isEmpty())
    {
        return QString("%1 %2").arg(id).arg(groupName);
    }

    return QString("%1 %2 - %3").arg(id).arg(groupName).arg(groupDesc);
}

void MainWindow::loadFromLoginResponse(const QJsonObject &response)
{
    int id = response.value("id").toInt();
    QString name = response.value("name").toString();

    m_userInfoLabel->setText(QString("Current User: id=%1, name=%2").arg(id).arg(name));
    m_statusLabel->setText("Login success. Friend list and group list loaded.");

    m_friendListWidget->clear();
    m_groupListWidget->clear();

    if (response.contains("friends") && response.value("friends").isArray())
    {
        QJsonArray arr = response.value("friends").toArray();
        for (const auto &item : arr)
        {
            QJsonObject obj;

            if (item.isString())
            {
                QJsonParseError err;
                QJsonDocument doc = QJsonDocument::fromJson(item.toString().toUtf8(), &err);
                if (err.error == QJsonParseError::NoError && doc.isObject())
                {
                    obj = doc.object();
                }
            }
            else if (item.isObject())
            {
                obj = item.toObject();
            }

            QString displayText = parseFriendItem(item);
            auto *listItem = new QListWidgetItem(displayText, m_friendListWidget);

            int id = obj.value("id").toInt();
            QString name = obj.value("name").toString();

            listItem->setData(Qt::UserRole, id);
            listItem->setData(Qt::UserRole + 1, name);
        }
    }
       if (response.contains("groups") && response.value("groups").isArray())
    {
        QJsonArray arr = response.value("groups").toArray();
        for (const auto &item : arr)
        {
            QJsonObject obj;

            if (item.isString())
            {
                QJsonParseError err;
                QJsonDocument doc = QJsonDocument::fromJson(item.toString().toUtf8(), &err);
                if (err.error == QJsonParseError::NoError && doc.isObject())
                {
                    obj = doc.object();
                }
            }
            else if (item.isObject())
            {
                obj = item.toObject();
            }

            QString displayText = parseGroupItem(item);
            auto *listItem = new QListWidgetItem(displayText, m_groupListWidget);

            int id = obj.contains("id") ? obj.value("id").toInt()
                                        : obj.value("groupid").toInt();
            QString name = obj.value("groupname").toString();

            listItem->setData(Qt::UserRole, id);
            listItem->setData(Qt::UserRole + 1, name);
        }
    }
}
void MainWindow::setStatusMessage(const QString &text)
{
    m_statusLabel->setText(text);
}

bool MainWindow::hasFriendItem(int friendId) const
{
    for (int i = 0; i < m_friendListWidget->count(); ++i)
    {
        QListWidgetItem *item = m_friendListWidget->item(i);
        if (item && item->data(Qt::UserRole).toInt() == friendId)
        {
            return true;
        }
    }
    return false;
}

bool MainWindow::hasGroupItem(int groupId) const
{
    for (int i = 0; i < m_groupListWidget->count(); ++i)
    {
        QListWidgetItem *item = m_groupListWidget->item(i);
        if (item && item->data(Qt::UserRole).toInt() == groupId)
        {
            return true;
        }
    }
    return false;
}

void MainWindow::addFriendItem(int friendId, const QString &friendName, const QString &state)
{
    if (hasFriendItem(friendId))
    {
        return;
    }

    QString text = QString("%1 %2 %3").arg(friendId).arg(friendName).arg(state);
    auto *item = new QListWidgetItem(text, m_friendListWidget);
    item->setData(Qt::UserRole, friendId);
    item->setData(Qt::UserRole + 1, friendName);
}

void MainWindow::addGroupItem(int groupId, const QString &groupName, const QString &groupDesc)
{
    if (hasGroupItem(groupId))
    {
        return;
    }

    QString text = groupDesc.isEmpty()
        ? QString("%1 %2").arg(groupId).arg(groupName)
        : QString("%1 %2 - %3").arg(groupId).arg(groupName).arg(groupDesc);

    auto *item = new QListWidgetItem(text, m_groupListWidget);
    item->setData(Qt::UserRole, groupId);
    item->setData(Qt::UserRole + 1, groupName);
}