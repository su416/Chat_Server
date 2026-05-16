cmake_minimum_required(VERSION 3.16)
project(QtChatClient LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/include/network
    ${CMAKE_CURRENT_SOURCE_DIR}/include/service
    ${CMAKE_CURRENT_SOURCE_DIR}/include/controller
    ${CMAKE_CURRENT_SOURCE_DIR}/include/model
)

add_executable(QtChatClient
    src/main.cpp
    src/ui/loginwindow.cpp
    src/ui/mainwindow.cpp
    src/ui/registerdialog.cpp
    src/ui/addfrienddialog.cpp
    src/ui/creategroupdialog.cpp
    src/ui/joingroupdialog.cpp
    src/ui/chatwindow.cpp
    src/ui/groupchatwindow.cpp
    src/network/chatclient.cpp
    src/controller/clientcontroller.cpp
    src/service/sessionmanager.cpp
    src/model/friendlistmodel.cpp
    src/model/grouplistmodel.cpp
    include/ui/loginwindow.h
    include/ui/mainwindow.h
    include/ui/registerdialog.h
    include/ui/addfrienddialog.h
    include/ui/creategroupdialog.h
    include/ui/joingroupdialog.h
    include/ui/chatwindow.h
    include/ui/groupchatwindow.h
    include/network/chatclient.h
    include/controller/clientcontroller.h
    include/service/sessionmanager.h
    include/model/friendlistmodel.h
    include/model/grouplistmodel.h
)

target_link_libraries(QtChatClient PRIVATE Qt6::Widgets Qt6::Network)
