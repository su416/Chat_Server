#ifndef LOGINWINDOW_H
#define LOGINWINDOW_H

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

class LoginWindow : public QWidget
{
    Q_OBJECT

public:
    explicit LoginWindow(QWidget *parent = nullptr);
    void setStatusMessage(const QString &text);
signals:
    void loginRequested(const QString &userId, const QString &password);
    void registerRequested();

private slots:
    void onLoginClicked();
    void onRegisterClicked();

private:
    void initUi();

private:
    QLabel *m_titleLabel;
    QLabel *m_userIdLabel;
    QLabel *m_passwordLabel;
    QLabel *m_statusLabel;

    QLineEdit *m_userIdEdit;
    QLineEdit *m_passwordEdit;

    QPushButton *m_loginButton;
    QPushButton *m_registerButton;
};

#endif
