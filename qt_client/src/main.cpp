#include <QApplication>

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

    loginWindow.show();
    return app.exec();
}