#ifndef JOINGROUPDIALOG_H
#define JOINGROUPDIALOG_H

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

class JoinGroupDialog : public QDialog
{
    Q_OBJECT

public:
    explicit JoinGroupDialog(QWidget *parent = nullptr);
    void setStatusMessage(const QString &text);
        void clearInputs();

signals:
    void joinGroupSubmitted(int groupId);

private slots:
    void onSubmitClicked();

private:
    void initUi();

private:
    QLabel *m_titleLabel;
    QLabel *m_groupIdLabel;
    QLabel *m_statusLabel;

    QLineEdit *m_groupIdEdit;

    QPushButton *m_submitButton;
    QPushButton *m_cancelButton;
};

#endif
