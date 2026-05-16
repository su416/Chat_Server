#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QJsonObject>

class QLabel;
class QListView;
class QPushButton;
class FriendListModel;
class GroupListModel;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    void loadFromLoginResponse(const QJsonObject &response);
    void setStatusMessage(const QString &text);
    void addFriendItem(int friendId, const QString &friendName, const QString &state);
    void addGroupItem(int groupId, const QString &groupName, const QString &groupDesc);

signals:
    void addFriendRequested();
    void createGroupRequested();
    void joinGroupRequested();
    void logoutRequested();
    void friendChatRequested(int friendId, const QString &friendName);
    void groupChatRequested(int groupId, const QString &groupName);

private:
    void initUi();

    QLabel *m_userInfoLabel;
    QLabel *m_statusLabel;

    QListView *m_friendListView;
    QListView *m_groupListView;
    FriendListModel *m_friendModel;
    GroupListModel *m_groupModel;

    QPushButton *m_addFriendButton;
    QPushButton *m_createGroupButton;
    QPushButton *m_joinGroupButton;
    QPushButton *m_logoutButton;
};

#endif
