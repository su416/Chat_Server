#include "ui/mainwindow.h"
#include "model/friendlistmodel.h"
#include "model/grouplistmodel.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_userInfoLabel(nullptr),
      m_statusLabel(nullptr),
      m_friendListView(nullptr),
      m_groupListView(nullptr),
      m_friendModel(nullptr),
      m_groupModel(nullptr),
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

    // ---- Model + View 替代原来的 QListWidget ----

    m_friendModel = new FriendListModel(this);
    m_friendListView = new QListView(this);
    m_friendListView->setModel(m_friendModel);
    m_friendListView->setEditTriggers(QAbstractItemView::NoEditTriggers);

    m_groupModel = new GroupListModel(this);
    m_groupListView = new QListView(this);
    m_groupListView->setModel(m_groupModel);
    m_groupListView->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // ---- 按钮 ----

    m_addFriendButton = new QPushButton("Add Friend", this);
    m_createGroupButton = new QPushButton("Create Group", this);
    m_joinGroupButton = new QPushButton("Join Group", this);
    m_logoutButton = new QPushButton("Logout", this);
    m_logoutButton->setObjectName("logoutButton");

    // ---- 布局 ----

    auto *friendGroup = new QGroupBox("Friends", this);
    auto *friendLayout = new QVBoxLayout();
    friendLayout->addWidget(m_friendListView);
    friendGroup->setLayout(friendLayout);

    auto *groupGroup = new QGroupBox("Groups", this);
    auto *groupLayout = new QVBoxLayout();
    groupLayout->addWidget(m_groupListView);
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

    // ---- 信号连接 ----

    connect(m_addFriendButton, &QPushButton::clicked, this,
            [this]() { emit addFriendRequested(); });

    connect(m_createGroupButton, &QPushButton::clicked, this,
            [this]() { emit createGroupRequested(); });

    connect(m_joinGroupButton, &QPushButton::clicked, this,
            [this]() { emit joinGroupRequested(); });

    connect(m_logoutButton, &QPushButton::clicked, this,
            [this]() { emit logoutRequested(); });

    // 好友列表双击 → 打开单聊
    connect(m_friendListView, &QListView::doubleClicked, this,
            [this](const QModelIndex &index) {
        if (!index.isValid()) return;
        int friendId = m_friendModel->friendIdAt(index.row());
        QString friendName = m_friendModel->friendNameAt(index.row());
        if (friendId > 0)
            emit friendChatRequested(friendId, friendName);
    });

    // 群列表双击 → 打开群聊
    connect(m_groupListView, &QListView::doubleClicked, this,
            [this](const QModelIndex &index) {
        if (!index.isValid()) return;
        int groupId = m_groupModel->groupIdAt(index.row());
        QString groupName = m_groupModel->groupNameAt(index.row());
        if (groupId > 0)
            emit groupChatRequested(groupId, groupName);
    });
}

// ---- 从登录响应批量加载好友和群列表 ----

void MainWindow::loadFromLoginResponse(const QJsonObject &response)
{
    int id = response.value("id").toInt();
    QString name = response.value("name").toString();

    m_userInfoLabel->setText(QString("Current User: id=%1, name=%2").arg(id).arg(name));
    m_statusLabel->setText("Login success. Friend list and group list loaded.");

    m_friendModel->clear();
    m_groupModel->clear();

    // 解析好友列表
    if (response.contains("friends") && response.value("friends").isArray())
    {
        QJsonArray arr = response.value("friends").toArray();
        for (const auto &item : arr)
        {
            QJsonObject obj = item.toObject();
            int fid = obj.value("id").toInt();
            QString fname = obj.value("name").toString();
            QString fstate = obj.value("state").toString();
            m_friendModel->addFriend(fid, fname, fstate);
        }
    }

    // 解析群列表
    if (response.contains("groups") && response.value("groups").isArray())
    {
        QJsonArray arr = response.value("groups").toArray();
        for (const auto &item : arr)
        {
            QJsonObject obj = item.toObject();
            int gid = obj.contains("id") ? obj.value("id").toInt()
                                         : obj.value("groupid").toInt();
            QString gname = obj.value("groupname").toString();
            QString gdesc = obj.value("groupdesc").toString();
            m_groupModel->addGroup(gid, gname, gdesc);
        }
    }
}

void MainWindow::setStatusMessage(const QString &text)
{
    m_statusLabel->setText(text);
}

void MainWindow::addFriendItem(int friendId, const QString &friendName,
                                const QString &state)
{
    m_friendModel->addFriend(friendId, friendName, state);
}

void MainWindow::addGroupItem(int groupId, const QString &groupName,
                               const QString &groupDesc)
{
    m_groupModel->addGroup(groupId, groupName, groupDesc);
}
