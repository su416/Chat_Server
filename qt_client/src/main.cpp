#include <QApplication>
#include <QFile>
#include <QSettings>

#include "ui/loginwindow.h"
#include "ui/mainwindow.h"
#include "ui/registerdialog.h"
#include "ui/addfrienddialog.h"
#include "ui/creategroupdialog.h"
#include "ui/joingroupdialog.h"
#include "ui/chatwindow.h"
#include "ui/groupchatwindow.h"
#include "network/chatclient.h"
#include "service/sessionmanager.h"
#include "controller/clientcontroller.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("ChatClient");
    app.setOrganizationName("ChatApp");

    // 加载 QSS 样式表
    QFile styleFile("style.qss");
    if (styleFile.open(QFile::ReadOnly))
    {
        app.setStyleSheet(styleFile.readAll());
        styleFile.close();
    }

    LoginWindow loginWindow;
    MainWindow mainWindow;
    RegisterDialog registerDialog;
    AddFriendDialog addFriendDialog;
    CreateGroupDialog createGroupDialog;
    JoinGroupDialog joinGroupDialog;
    ChatWindow chatWindow;
    GroupChatWindow groupChatWindow;
    ChatClient client;
    SessionManager session;

    ClientController controller(loginWindow,
                                mainWindow,
                                registerDialog,
                                addFriendDialog,
                                createGroupDialog,
                                joinGroupDialog,
                                chatWindow,
                                groupChatWindow,
                                client,
                                session);

    // 恢复上次窗口位置/大小
    QSettings settings;
    if (settings.contains("mainwindow/geometry"))
        mainWindow.restoreGeometry(settings.value("mainwindow/geometry").toByteArray());

    loginWindow.show();

    int ret = app.exec();

    // 退出时保存窗口位置
    settings.setValue("mainwindow/geometry", mainWindow.saveGeometry());

    return ret;
}
