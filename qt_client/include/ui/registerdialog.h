#ifndef REGISTERDIALOG_H
#define REGISTERDIALOG_H

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

class RegisterDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RegisterDialog(QWidget *parent = nullptr);
    void setStatusMessage(const QString &text);
        void clearInputs();

signals:
    void registerSubmitted(const QString &name, const QString &password);

private slots:
    void onSubmitClicked();

private:
    void initUi();

private:
    QLabel *m_titleLabel;
    QLabel *m_nameLabel;
    QLabel *m_passwordLabel;
    QLabel *m_statusLabel;

    QLineEdit *m_nameEdit;
    QLineEdit *m_passwordEdit;

    QPushButton *m_submitButton;
    QPushButton *m_cancelButton;
};

#endif
