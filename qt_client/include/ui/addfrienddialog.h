#ifndef ADDFRIENDDIALOG_H
#define ADDFRIENDDIALOG_H

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

class AddFriendDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AddFriendDialog(QWidget *parent = nullptr);
    void setStatusMessage(const QString &text);
    void clearInputs();

signals:
    void addFriendSubmitted(int friendId);

private slots:
    void onSubmitClicked();

private:
    void initUi();

private:
    QLabel *m_titleLabel;
    QLabel *m_friendIdLabel;
    QLabel *m_statusLabel;

    QLineEdit *m_friendIdEdit;

    QPushButton *m_submitButton;
    QPushButton *m_cancelButton;
};

#endif
