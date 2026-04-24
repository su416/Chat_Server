#include "ui/creategroupdialog.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

CreateGroupDialog::CreateGroupDialog(QWidget *parent)
    : QDialog(parent),
      m_titleLabel(nullptr),
      m_groupNameLabel(nullptr),
      m_groupDescLabel(nullptr),
      m_statusLabel(nullptr),
      m_groupNameEdit(nullptr),
      m_groupDescEdit(nullptr),
      m_submitButton(nullptr),
      m_cancelButton(nullptr)
{
    initUi();
}

void CreateGroupDialog::initUi()
{
    setWindowTitle("Create Group");
    resize(420, 220);
    setModal(true);

    m_titleLabel = new QLabel("Create New Group", this);
    QFont titleFont;
    titleFont.setPointSize(14);
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);
    m_titleLabel->setAlignment(Qt::AlignCenter);

    m_groupNameLabel = new QLabel("Group Name:", this);
    m_groupDescLabel = new QLabel("Group Desc:", this);
    m_statusLabel = new QLabel("Please enter group information.", this);

    m_groupNameEdit = new QLineEdit(this);
    m_groupNameEdit->setPlaceholderText("Enter group name");

    m_groupDescEdit = new QLineEdit(this);
    m_groupDescEdit->setPlaceholderText("Enter group description");

    m_submitButton = new QPushButton("Submit", this);
    m_cancelButton = new QPushButton("Cancel", this);

    auto *mainLayout = new QVBoxLayout(this);
    auto *nameLayout = new QHBoxLayout();
    auto *descLayout = new QHBoxLayout();
    auto *buttonLayout = new QHBoxLayout();

    nameLayout->addWidget(m_groupNameLabel);
    nameLayout->addWidget(m_groupNameEdit);

    descLayout->addWidget(m_groupDescLabel);
    descLayout->addWidget(m_groupDescEdit);

    buttonLayout->addStretch();
    buttonLayout->addWidget(m_submitButton);
    buttonLayout->addWidget(m_cancelButton);
    buttonLayout->addStretch();

    mainLayout->addWidget(m_titleLabel);
    mainLayout->addSpacing(15);
    mainLayout->addLayout(nameLayout);
    mainLayout->addLayout(descLayout);
    mainLayout->addSpacing(10);
    mainLayout->addLayout(buttonLayout);
    mainLayout->addWidget(m_statusLabel);

    connect(m_submitButton, &QPushButton::clicked, this, &CreateGroupDialog::onSubmitClicked);
    connect(m_cancelButton, &QPushButton::clicked, this, &CreateGroupDialog::reject);
}

void CreateGroupDialog::setStatusMessage(const QString &text)
{
    m_statusLabel->setText(text);
}

void CreateGroupDialog::onSubmitClicked()
{
    const QString groupName = m_groupNameEdit->text().trimmed();
    const QString groupDesc = m_groupDescEdit->text().trimmed();

    if (groupName.isEmpty())
    {
        m_statusLabel->setText("Group name cannot be empty.");
        return;
    }

    m_statusLabel->setText("Sending create group request...");
    emit createGroupSubmitted(groupName, groupDesc);
}

void CreateGroupDialog::clearInputs()
{
    m_groupNameEdit->clear();
    m_groupDescEdit->clear();
}