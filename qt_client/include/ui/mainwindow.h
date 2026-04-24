#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QJsonObject>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

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
    QString parseFriendItem(const QJsonValue &value) const;
    QString parseGroupItem(const QJsonValue &value) const;
    bool hasFriendItem(int friendId) const;
    bool hasGroupItem(int groupId) const;


private:
    QLabel *m_userInfoLabel;
    QLabel *m_statusLabel;

    QListWidget *m_friendListWidget;
    QListWidget *m_groupListWidget;

    QPushButton *m_addFriendButton;
    QPushButton *m_createGroupButton;
    QPushButton *m_joinGroupButton;
    QPushButton *m_logoutButton;
};

#endif
