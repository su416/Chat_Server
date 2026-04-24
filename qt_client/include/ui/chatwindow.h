#ifndef CHATWINDOW_H
#define CHATWINDOW_H

#include <QWidget>

class QLabel;
class QTextEdit;
class QLineEdit;
class QPushButton;

class ChatWindow : public QWidget
{
    Q_OBJECT

public:
    explicit ChatWindow(QWidget *parent = nullptr);

    void setChatPeer(int peerId, const QString &peerName);
    int peerId() const;
    void appendOutgoingMessage(const QString &msg, const QString &time);
    void appendIncomingMessage(const QString &senderName, const QString &msg, const QString &time);
    void clearMessages();


signals:
    void sendMessageRequested(int peerId, const QString &text);

private slots:
    void onSendClicked();

private:
    void initUi();

private:
    int m_peerId;
    QString m_peerName;

    QLabel *m_titleLabel;
    QTextEdit *m_messageView;
    QLineEdit *m_inputEdit;
    QPushButton *m_sendButton;
};

#endif

