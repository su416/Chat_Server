#include "ui/joingroupdialog.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

JoinGroupDialog::JoinGroupDialog(QWidget *parent)
    : QDialog(parent),
      m_titleLabel(nullptr),
      m_groupIdLabel(nullptr),
      m_statusLabel(nullptr),
      m_groupIdEdit(nullptr),
      m_submitButton(nullptr),
      m_cancelButton(nullptr)
{
    initUi();
}

void JoinGroupDialog::initUi()
{
    setWindowTitle("Join Group");
    resize(340, 180);
    setModal(true);

    m_titleLabel = new QLabel("Join Existing Group", this);
    QFont titleFont;
    titleFont.setPointSize(14);
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);
    m_titleLabel->setAlignment(Qt::AlignCenter);

    m_groupIdLabel = new QLabel("Group ID:", this);
    m_statusLabel = new QLabel("Please enter group id.", this);

    m_groupIdEdit = new QLineEdit(this);
    m_groupIdEdit->setPlaceholderText("Enter group id");

    m_submitButton = new QPushButton("Submit", this);
    m_cancelButton = new QPushButton("Cancel", this);

    auto *mainLayout = new QVBoxLayout(this);
    auto *inputLayout = new QHBoxLayout();
    auto *buttonLayout = new QHBoxLayout();

    inputLayout->addWidget(m_groupIdLabel);
    inputLayout->addWidget(m_groupIdEdit);

    buttonLayout->addStretch();
    buttonLayout->addWidget(m_submitButton);
    buttonLayout->addWidget(m_cancelButton);
    buttonLayout->addStretch();

    mainLayout->addWidget(m_titleLabel);
    mainLayout->addSpacing(15);
    mainLayout->addLayout(inputLayout);
    mainLayout->addSpacing(10);
    mainLayout->addLayout(buttonLayout);
    mainLayout->addWidget(m_statusLabel);

    connect(m_submitButton, &QPushButton::clicked, this, &JoinGroupDialog::onSubmitClicked);
    connect(m_cancelButton, &QPushButton::clicked, this, &JoinGroupDialog::reject);
}

void JoinGroupDialog::setStatusMessage(const QString &text)
{
    m_statusLabel->setText(text);
}

void JoinGroupDialog::onSubmitClicked()
{
    bool ok = false;
    int groupId = m_groupIdEdit->text().trimmed().toInt(&ok);

    if (!ok || groupId <= 0)
    {
        m_statusLabel->setText("Group ID must be a positive number.");
        return;
    }

    m_statusLabel->setText("Sending join group request...");
    emit joinGroupSubmitted(groupId);
}

void JoinGroupDialog::clearInputs()
{
    m_groupIdEdit->clear();
}