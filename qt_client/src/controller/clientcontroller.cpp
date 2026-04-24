#include "controller/clientcontroller.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QTimer>

#include "ui/loginwindow.h"
#include "ui/mainwindow.h"
#include "ui/registerdialog.h"
#include "ui/addfrienddialog.h"
#include "ui/creategroupdialog.h"
#include "ui/joingroupdialog.h"
#include "ui/chatwindow.h"
#include "ui/groupchatwindow.h"
#include "network/chatclient.h"
#include "service/sessionmanager.h"

ClientController::ClientController(LoginWindow &loginWindow,
                                   MainWindow &mainWindow,
                                   RegisterDialog &registerDialog,
                                   AddFriendDialog &addFriendDialog,
                                   CreateGroupDialog &createGroupDialog,
                                   JoinGroupDialog &joinGroupDialog,
                                   ChatWindow &chatWindow,
                                   GroupChatWindow &groupChatWindow,
                                   ChatClient &client,
                                   SessionManager &session,
                                   QObject *parent)
    : QObject(parent),
      m_loginWindow(loginWindow),
      m_mainWindow(mainWindow),
      m_registerDialog(registerDialog),
      m_addFriendDialog(addFriendDialog),
      m_createGroupDialog(createGroupDialog),
      m_joinGroupDialog(joinGroupDialog),
      m_chatWindow(chatWindow),
      m_groupChatWindow(groupChatWindow),
      m_client(client),
      m_session(session)
{
    setupConnections();
}

QString ClientController::mapAddFriendError(const QString &reason) const
{
    if (reason == "cannot add yourself")
        return "You cannot add yourself.";
    if (reason == "friend user does not exist")
        return "The friend ID does not exist.";
    if (reason == "add friend failed")
        return "Failed to add friend. It may already exist.";
    return "Add friend failed: " + reason;
}

QString ClientController::mapCreateGroupError(const QString &reason) const
{
    if (reason == "group name cannot be empty")
        return "Group name cannot be empty.";
    if (reason == "group name already exists")
        return "This group name already exists.";
    if (reason == "create group owner relation failed")
        return "Group was created, but adding the creator failed.";
    if (reason == "create group failed")
        return "Failed to create group.";
    return "Create group failed: " + reason;
}

QString ClientController::mapJoinGroupError(const QString &reason) const
{
    if (reason == "group does not exist")
        return "The group ID does not exist.";
    if (reason == "already in group")
        return "You are already in this group.";
    if (reason == "join group failed")
        return "Failed to join group.";
    return "Join group failed: " + reason;
}

void ClientController::resetUiAfterLogout()
{
    m_session.clear();

    m_registerDialog.close();
    m_addFriendDialog.close();
    m_createGroupDialog.close();
    m_joinGroupDialog.close();
    m_chatWindow.close();
    m_groupChatWindow.close();

    m_loginWindow.setStatusMessage("Logged out. Please login again.");
    m_mainWindow.hide();
    m_loginWindow.show();
    m_loginWindow.raise();
    m_loginWindow.activateWindow();
}

void ClientController::setupConnections()
{
    setupLoginFlow();
    setupChatFlow();
    setupGroupFlow();
    setupFriendRelationFlow();
    setupGroupRelationFlow();
    setupLogoutFlow();
}

void ClientController::setupLoginFlow()
{
    QObject::connect(&m_loginWindow, &LoginWindow::loginRequested,
                     this,
                     [this](const QString &userId, const QString &password) {
        bool ok = false;
        int id = userId.toInt(&ok);
        if (!ok)
        {
            m_loginWindow.setStatusMessage("User ID must be a number.");
            return;
        }

        m_client.connectToServer("127.0.0.1", 6000);
        m_loginWindow.setStatusMessage("Connected. Sending login request...");
        m_client.sendLoginRequest(id, password);
    });

    QObject::connect(&m_loginWindow, &LoginWindow::registerRequested,
                     this,
                     [this]() {
        m_registerDialog.setStatusMessage("Please enter registration information.");
        m_registerDialog.show();
        m_registerDialog.raise();
        m_registerDialog.activateWindow();
    });

    QObject::connect(&m_registerDialog, &RegisterDialog::registerSubmitted,
                     this,
                     [this](const QString &name, const QString &password) {
        m_client.connectToServer("127.0.0.1", 6000);
        m_registerDialog.setStatusMessage("Connected. Sending register request...");
        m_client.sendRegisterRequest(name, password);
    });

    QObject::connect(&m_client, &ChatClient::connected,
                     this,
                     [this]() {
        m_loginWindow.setStatusMessage("Server connected.");
    });

    QObject::connect(&m_client, &ChatClient::disconnected,
                     this,
                     [this]() {
        m_loginWindow.setStatusMessage("Disconnected from server.");
    });

    QObject::connect(&m_client, &ChatClient::networkError,
                     this,
                     [this](const QString &reason) {
        m_loginWindow.setStatusMessage("Network error: " + reason);
        m_registerDialog.setStatusMessage("Network error: " + reason);
    });

    QObject::connect(&m_client, &ChatClient::loginFailed,
                     this,
                     [this](const QString &reason) {
        m_loginWindow.setStatusMessage("Login failed: " + reason);
    });

    QObject::connect(&m_client, &ChatClient::loginSucceeded,
                     this,
                     [this](const QJsonObject &obj) {
        m_session.setCurrentUser(obj.value("id").toInt(), obj.value("name").toString());
        m_session.rebuildFromLoginResponse(obj);

        m_loginWindow.setStatusMessage("Login success.");
        m_mainWindow.loadFromLoginResponse(obj);
        m_mainWindow.show();
        m_loginWindow.hide();
    });

    QObject::connect(&m_client, &ChatClient::registerFailed,
                     this,
                     [this](const QString &reason) {
        m_registerDialog.setStatusMessage("Register failed: " + reason);
    });

    QObject::connect(&m_client, &ChatClient::registerSucceeded,
                     this,
                     [this](int userId) {
        m_registerDialog.setStatusMessage(
            QString("Register success. New user id = %1").arg(userId));
        m_registerDialog.clearInputs();
    });
}

void ClientController::setupChatFlow()
{
    QObject::connect(&m_mainWindow, &MainWindow::friendChatRequested,
                     this,
                     [this](int friendId, const QString &friendName) {
        m_chatWindow.setChatPeer(friendId, friendName);
        m_chatWindow.clearMessages();
        m_chatWindow.show();
        m_chatWindow.raise();
        m_chatWindow.activateWindow();

        if (m_session.userId() > 0)
        {
            m_client.sendQueryOneChatHistoryRequest(m_session.userId(), 20);
        }
    });

    QObject::connect(&m_chatWindow, &ChatWindow::sendMessageRequested,
                     this,
                     [this](int peerId, const QString &text) {
        if (m_session.userId() <= 0)
        {
            return;
        }

        QString now = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
        m_chatWindow.appendOutgoingMessage(text, now);
        m_client.sendOneChatRequest(m_session.userId(), peerId, m_session.userName(), text, now);
    });

    QObject::connect(&m_client, &ChatClient::oneChatMessageReceived,
                     this,
                     [this](int fromId, const QString &name, const QString &msg, const QString &time) {
        if (m_chatWindow.peerId() == fromId)
        {
            m_chatWindow.appendIncomingMessage(name, msg, time);
            m_chatWindow.show();
            m_chatWindow.raise();
            m_chatWindow.activateWindow();
        }
    });

    QObject::connect(&m_client, &ChatClient::oneChatHistoryReceived,
                     this,
                     [this](const QJsonArray &history) {
        const int peerId = m_chatWindow.peerId();
        if (peerId <= 0)
        {
            return;
        }

        QList<QJsonObject> filtered;
        for (const auto &item : history)
        {
            if (!item.isString())
            {
                continue;
            }

            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(item.toString().toUtf8(), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject())
            {
                continue;
            }

            QJsonObject obj = doc.object();
            int fromId = obj.value("fromid").toInt();
            int toId = obj.value("toid").toInt();

            bool matched =
                (fromId == m_session.userId() && toId == peerId) ||
                (fromId == peerId && toId == m_session.userId());

            if (matched)
            {
                filtered.push_back(obj);
            }
        }

        for (int i = filtered.size() - 1; i >= 0; --i)
        {
            const QJsonObject &obj = filtered[i];
            int fromId = obj.value("fromid").toInt();
            QString msg = obj.value("msg").toString();
            QString time = obj.value("send_time").toString();

            if (fromId == m_session.userId())
            {
                m_chatWindow.appendOutgoingMessage(msg, time);
            }
            else
            {
                QString peerName = m_session.friendName(peerId, "Peer");
                m_chatWindow.appendIncomingMessage(peerName, msg, time);
            }
        }
    });
}

void ClientController::setupGroupFlow()
{
    QObject::connect(&m_mainWindow, &MainWindow::groupChatRequested,
                     this,
                     [this](int groupId, const QString &groupName) {
        m_groupChatWindow.setGroupInfo(groupId, groupName);
        m_groupChatWindow.clearMessages();
        m_groupChatWindow.show();
        m_groupChatWindow.raise();
        m_groupChatWindow.activateWindow();

        m_client.sendQueryGroupChatHistoryRequest(groupId, 20);
    });

    QObject::connect(&m_groupChatWindow, &GroupChatWindow::sendGroupMessageRequested,
                     this,
                     [this](int groupId, const QString &text) {
        if (m_session.userId() <= 0)
        {
            return;
        }

        QString now = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
        m_groupChatWindow.appendOutgoingMessage(text, now);
        m_client.sendGroupChatRequest(m_session.userId(), groupId, m_session.userName(), text, now);
    });

    QObject::connect(&m_client, &ChatClient::groupChatMessageReceived,
                     this,
                     [this](int fromId, int groupId, const QString &name,
                            const QString &msg, const QString &time) {
        Q_UNUSED(fromId);

        if (m_groupChatWindow.groupId() == groupId)
        {
            m_groupChatWindow.appendIncomingMessage(name, msg, time);
            m_groupChatWindow.show();
            m_groupChatWindow.raise();
            m_groupChatWindow.activateWindow();
        }
    });

    QObject::connect(&m_client, &ChatClient::groupChatHistoryReceived,
                     this,
                     [this](int groupId, const QJsonArray &history) {
        if (m_groupChatWindow.groupId() != groupId)
        {
            return;
        }

        QList<QJsonObject> filtered;
        for (const auto &item : history)
        {
            if (!item.isString())
            {
                continue;
            }

            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(item.toString().toUtf8(), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject())
            {
                continue;
            }

            filtered.push_back(doc.object());
        }

        for (int i = filtered.size() - 1; i >= 0; --i)
        {
            const QJsonObject &obj = filtered[i];
            int fromId = obj.value("fromid").toInt();
            QString msg = obj.value("msg").toString();
            QString time = obj.value("send_time").toString();

            if (fromId == m_session.userId())
            {
                m_groupChatWindow.appendOutgoingMessage(msg, time);
            }
            else
            {
                QString senderName = m_session.groupMemberName(
                    groupId,
                    fromId,
                    QString("User%1").arg(fromId));

                m_groupChatWindow.appendIncomingMessage(senderName, msg, time);
            }
        }
    });
}

void ClientController::setupFriendRelationFlow()
{
    QObject::connect(&m_mainWindow, &MainWindow::addFriendRequested,
                     this,
                     [this]() {
        m_addFriendDialog.clearInputs();
        m_addFriendDialog.setStatusMessage("Please enter friend id.");
        m_addFriendDialog.show();
        m_addFriendDialog.raise();
        m_addFriendDialog.activateWindow();
    });

    QObject::connect(&m_addFriendDialog, &AddFriendDialog::addFriendSubmitted,
                     this,
                     [this](int friendId) {
        if (m_session.userId() <= 0)
        {
            m_addFriendDialog.setStatusMessage("Current user is invalid.");
            return;
        }

        m_client.sendAddFriendRequest(m_session.userId(), friendId);
        m_addFriendDialog.setStatusMessage(
            QString("Add friend request sent. friend id = %1").arg(friendId));
    });

    QObject::connect(&m_client, &ChatClient::addFriendSucceeded,
                     this,
                     [this](int friendId, const QString &friendName, const QString &state) {
        m_session.cacheFriend(friendId, friendName);

        m_mainWindow.addFriendItem(friendId, friendName, state);
        m_mainWindow.setStatusMessage(
            QString("Add friend success. friend id = %1").arg(friendId));
        m_addFriendDialog.setStatusMessage(
            QString("Add friend success. friend id = %1").arg(friendId));
        m_addFriendDialog.clearInputs();
        m_addFriendDialog.accept();
    });

    QObject::connect(&m_client, &ChatClient::addFriendFailed,
                     this,
                     [this](const QString &reason) {
        QString msg = mapAddFriendError(reason);
        m_mainWindow.setStatusMessage(msg);
        m_addFriendDialog.setStatusMessage(msg);
        m_addFriendDialog.clearInputs();
        m_addFriendDialog.raise();
        m_addFriendDialog.activateWindow();
    });
}

void ClientController::setupGroupRelationFlow()
{
    QObject::connect(&m_mainWindow, &MainWindow::createGroupRequested,
                     this,
                     [this]() {
        m_createGroupDialog.clearInputs();
        m_createGroupDialog.setStatusMessage("Please enter group information.");
        m_createGroupDialog.show();
        m_createGroupDialog.raise();
        m_createGroupDialog.activateWindow();
    });

    QObject::connect(&m_createGroupDialog, &CreateGroupDialog::createGroupSubmitted,
                     this,
                     [this](const QString &groupName, const QString &groupDesc) {
        if (m_session.userId() <= 0)
        {
            m_createGroupDialog.setStatusMessage("Current user is invalid.");
            return;
        }

        m_client.sendCreateGroupRequest(m_session.userId(), groupName, groupDesc);
        m_createGroupDialog.setStatusMessage(
            QString("Create group request sent. group name = %1").arg(groupName));
    });

    QObject::connect(&m_client, &ChatClient::createGroupSucceeded,
                     this,
                     [this](int groupId, const QString &groupName, const QString &groupDesc) {
        m_session.cacheGroupMember(groupId, m_session.userId(), m_session.userName());

        m_mainWindow.addGroupItem(groupId, groupName, groupDesc);
        m_mainWindow.setStatusMessage(
            QString("Create group success. group id = %1").arg(groupId));
        m_createGroupDialog.setStatusMessage(
            QString("Create group success. group id = %1").arg(groupId));
        m_createGroupDialog.clearInputs();
        m_createGroupDialog.accept();
    });

    QObject::connect(&m_client, &ChatClient::createGroupFailed,
                     this,
                     [this](const QString &reason) {
        QString msg = mapCreateGroupError(reason);
        m_mainWindow.setStatusMessage(msg);
        m_createGroupDialog.setStatusMessage(msg);
        m_createGroupDialog.clearInputs();
        m_createGroupDialog.raise();
        m_createGroupDialog.activateWindow();
    });

    QObject::connect(&m_mainWindow, &MainWindow::joinGroupRequested,
                     this,
                     [this]() {
        m_joinGroupDialog.clearInputs();
        m_joinGroupDialog.setStatusMessage("Please enter group id.");
        m_joinGroupDialog.show();
        m_joinGroupDialog.raise();
        m_joinGroupDialog.activateWindow();
    });

    QObject::connect(&m_joinGroupDialog, &JoinGroupDialog::joinGroupSubmitted,
                     this,
                     [this](int groupId) {
        if (m_session.userId() <= 0)
        {
            m_joinGroupDialog.setStatusMessage("Current user is invalid.");
            return;
        }

        m_client.sendJoinGroupRequest(m_session.userId(), groupId);
        m_joinGroupDialog.setStatusMessage(
            QString("Join group request sent. group id = %1").arg(groupId));
    });

    QObject::connect(&m_client, &ChatClient::joinGroupSucceeded,
                     this,
                     [this](int groupId, const QString &groupName, const QString &groupDesc) {
        m_session.cacheGroupMember(groupId, m_session.userId(), m_session.userName());

        m_mainWindow.addGroupItem(groupId, groupName, groupDesc);
        m_mainWindow.setStatusMessage(
            QString("Join group success. group id = %1").arg(groupId));
        m_joinGroupDialog.setStatusMessage(
            QString("Join group success. group id = %1").arg(groupId));
        m_joinGroupDialog.clearInputs();
        m_joinGroupDialog.accept();
    });

    QObject::connect(&m_client, &ChatClient::joinGroupFailed,
                     this,
                     [this](const QString &reason) {
        QString msg = mapJoinGroupError(reason);
        m_mainWindow.setStatusMessage(msg);
        m_joinGroupDialog.setStatusMessage(msg);
        m_joinGroupDialog.clearInputs();
        m_joinGroupDialog.raise();
        m_joinGroupDialog.activateWindow();
    });
}

void ClientController::setupLogoutFlow()
{
    QObject::connect(&m_mainWindow, &MainWindow::logoutRequested,
                     this,
                     [this]() {
        if (m_session.userId() <= 0)
        {
            m_mainWindow.setStatusMessage("Current user is invalid.");
            return;
        }

        m_mainWindow.setStatusMessage("Logging out...");
        m_client.sendLogoutRequest(m_session.userId());

        QTimer::singleShot(100, this, [this]() {
            m_client.disconnectFromServer();
            resetUiAfterLogout();
        });
    });
}