#ifndef CLIENTCONTROLLER_H
#define CLIENTCONTROLLER_H

#include <QObject>
#include <QString>

class LoginWindow;
class MainWindow;
class RegisterDialog;
class AddFriendDialog;
class CreateGroupDialog;
class JoinGroupDialog;
class ChatWindow;
class GroupChatWindow;
class ChatClient;
class SessionManager;

class ClientController : public QObject
{
    Q_OBJECT

public:
    explicit ClientController(LoginWindow &loginWindow,
                              MainWindow &mainWindow,
                              RegisterDialog &registerDialog,
                              AddFriendDialog &addFriendDialog,
                              CreateGroupDialog &createGroupDialog,
                              JoinGroupDialog &joinGroupDialog,
                              ChatWindow &chatWindow,
                              GroupChatWindow &groupChatWindow,
                              ChatClient &client,
                              SessionManager &session,
                              QObject *parent = nullptr);

private:
    void setupConnections();

    void setupLoginFlow();
    void setupChatFlow();
    void setupGroupFlow();
    void setupFriendRelationFlow();
    void setupGroupRelationFlow();
    void setupLogoutFlow();

    void resetUiAfterLogout();

    QString mapAddFriendError(const QString &reason) const;
    QString mapCreateGroupError(const QString &reason) const;
    QString mapJoinGroupError(const QString &reason) const;

private:
    LoginWindow &m_loginWindow;
    MainWindow &m_mainWindow;
    RegisterDialog &m_registerDialog;
    AddFriendDialog &m_addFriendDialog;
    CreateGroupDialog &m_createGroupDialog;
    JoinGroupDialog &m_joinGroupDialog;
    ChatWindow &m_chatWindow;
    GroupChatWindow &m_groupChatWindow;
    ChatClient &m_client;
    SessionManager &m_session;
};

#endif