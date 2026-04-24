#ifndef CREATEGROUPDIALOG_H
#define CREATEGROUPDIALOG_H

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

class CreateGroupDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CreateGroupDialog(QWidget *parent = nullptr);
    void setStatusMessage(const QString &text);
    void clearInputs();

signals:
    void createGroupSubmitted(const QString &groupName, const QString &groupDesc);

private slots:
    void onSubmitClicked();

private:
    void initUi();

private:
    QLabel *m_titleLabel;
    QLabel *m_groupNameLabel;
    QLabel *m_groupDescLabel;
    QLabel *m_statusLabel;

    QLineEdit *m_groupNameEdit;
    QLineEdit *m_groupDescEdit;

    QPushButton *m_submitButton;
    QPushButton *m_cancelButton;
};

#endif
