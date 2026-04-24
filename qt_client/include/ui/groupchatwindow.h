#ifndef GROUPCHATWINDOW_H
#define GROUPCHATWINDOW_H

#include <QWidget>

class QLabel;
class QTextEdit;
class QLineEdit;
class QPushButton;

class GroupChatWindow : public QWidget
{
    Q_OBJECT

public:
    explicit GroupChatWindow(QWidget *parent = nullptr);

    void setGroupInfo(int groupId, const QString &groupName);
    int groupId() const;
    void appendOutgoingMessage(const QString &msg, const QString &time);
    void appendIncomingMessage(const QString &senderName, const QString &msg, const QString &time);
    void clearMessages();

signals:
    void sendGroupMessageRequested(int groupId, const QString &text);

private slots:
    void onSendClicked();

private:
    void initUi();

private:
    int m_groupId;
    QString m_groupName;

    QLabel *m_titleLabel;
    QTextEdit *m_messageView;
    QLineEdit *m_inputEdit;
    QPushButton *m_sendButton;
};

#endif
