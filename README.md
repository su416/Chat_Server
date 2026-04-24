# ChatServer

基于 Muduo + MySQL + Redis + CMake + Qt** 的 C++ 聊天系统。

本项目在传统课程聊天室项目基础上进行了工程化整理与功能扩展，支持：

- 用户注册、登录、注销
- 好友添加与单聊
- 群组创建、加群与群聊
- 离线消息存储
- 单聊 / 群聊历史消息查询
- Redis 发布订阅转发消息
- 基于 Qt Widgets 的图形客户端

---

## 1. 项目特点

### 服务端
- 使用 Muduo 实现高并发 TCP 服务端
- 使用 MySQL 存储用户、好友、群组、离线消息和聊天记录
- 使用 Redis 发布订阅支持跨服务器消息转发
- 使用 JSON 配置文件管理服务端、MySQL、Redis 参数
- 支持单聊与群聊历史消息查询
- 支持命令行客户端与 Qt 图形客户端接入

### Qt 客户端
- 登录 / 注册
- 主界面展示
- 好友列表 / 群组列表展示
- 添加好友
- 创建群组
- 加入群组
- 单聊窗口
- 群聊窗口
- 单聊 / 群聊历史消息加载
- logout

---

## 2. 技术栈

- C++
- Muduo
- MySQL
- Redis / hiredis
- nlohmann/json
- CMake
- Qt Widgets / QTcpSocket
- Linux / Ubuntu

---

## 3. 目录结构

```text
ChatServer/
├── CMakeLists.txt
├── conf/                  # 配置文件
├── docs/                  # 文档与截图
├── include/               # 服务端头文件
├── proto/                 # Protobuf 定义
├── qt_client/             # Qt 图形客户端
├── script/                # 构建与运行脚本
├── sql/                   # 数据库初始化脚本
├── src/                   # 服务端 / 命令行客户端源码
├── test/                  # 测试代码
└── thirdparty/            # 第三方头文件
Qt 客户端结构
qt_client/
├── CMakeLists.txt
├── include
│   ├── controller
│   │   └── clientcontroller.h
│   ├── network
│   │   └── chatclient.h
│   ├── service
│   │   └── sessionmanager.h
│   └── ui
│       ├── addfrienddialog.h
│       ├── chatwindow.h
│       ├── creategroupdialog.h
│       ├── groupchatwindow.h
│       ├── joingroupdialog.h
│       ├── loginwindow.h
│       ├── mainwindow.h
│       └── registerdialog.h
└── src
    ├── controller
    │   └── clientcontroller.cpp
    ├── network
    │   └── chatclient.cpp
    ├── service
    │   └── sessionmanager.cpp
    ├── ui
    │   ├── addfrienddialog.cpp
    │   ├── chatwindow.cpp
    │   ├── creategroupdialog.cpp
    │   ├── groupchatwindow.cpp
    │   ├── joingroupdialog.cpp
    │   ├── loginwindow.cpp
    │   ├── mainwindow.cpp
    │   └── registerdialog.cpp
    └── main.cpp
4. 环境依赖

建议环境：

Ubuntu 20.04 / 22.04 / 24.04
g++ 9+
CMake 3.10+
MySQL 8.x
Redis 6.x / 7.x
Muduo
protobuf-compiler / libprotobuf-dev
Qt 6
常用安装命令
sudo apt update
sudo apt install -y g++ cmake make
sudo apt install -y mysql-server libmysqlclient-dev
sudo apt install -y redis-server libhiredis-dev
sudo apt install -y protobuf-compiler libprotobuf-dev
服务检查
sudo systemctl status mysql
sudo systemctl status redis-server
5. 配置文件说明

配置文件路径：

conf/chatserver.json

当前示例配置如下：

{
  "server": {
    "ip": "127.0.0.1",
    "port": 6000,
    "thread_num": 4,
    "name": "ChatServer"
  },
  "mysql": {
    "host": "127.0.0.1",
    "port": 3306,
    "user": "root",
    "password": "123456",
    "dbname": "chat",
    "charset": "utf8"
  },
  "redis": {
    "host": "127.0.0.1",
    "port": 6379
  },
  "log": {
    "level": "INFO"
  },
  "business": {
    "max_history_count": 50
  }
}
配置项说明
server
ip：服务端监听 IP
port：服务端监听端口
thread_num：Muduo I/O 线程数
name：服务名
mysql
host：MySQL 地址
port：MySQL 端口
user：用户名
password：密码
dbname：数据库名
charset：字符集
redis
host：Redis 地址
port：Redis 端口
business
max_history_count：历史消息默认查询上限（当前预留）
6. 数据库初始化
6.1 初始化基础表
mysql -u root -p < sql/chat.sql
6.2 初始化聊天记录表
mysql -u root -p < sql/chat_history.sql
6.3 校验表结构
use chat;
show tables;
desc user;
desc friend;
desc allgroup;
desc groupuser;
desc offlinemessage;
desc chatmessage;
7. 构建与运行
7.1 构建服务端 / 命令行客户端

推荐使用脚本：

./script/build.sh

脚本会自动完成：

删除旧 build/
创建新 build/
执行 cmake ..
执行 make -j$(nproc)

手动构建方式：

rm -rf build
mkdir build
cd build
cmake ..
make -j$(nproc)

构建完成后，可执行文件位于：

bin/ChatServer
bin/ChatClient
7.2 启动服务端

推荐：

./script/run_server.sh

或直接执行：

./bin/ChatServer
7.3 启动命令行客户端

推荐：

./script/run_client.sh

默认连接：

127.0.0.1:6000

也可以手动指定：

./script/run_client.sh 127.0.0.1 6000

或者：

./bin/ChatClient 127.0.0.1 6000
8. 命令行客户端使用说明
首页菜单

客户端启动后显示：

1. login
2. register
3. quit
登录后支持命令
help
chat:friendid:message
addfriend:friendid
creategroup:groupname:groupdesc
addgroup:groupid
groupchat:groupid:message
history:count
grouphistory:groupid:count
loginout
命令说明
help

显示所有支持的命令。

chat:friendid:message

向指定好友发送单聊消息。

示例：

chat:12:hello
addfriend:friendid

添加好友。

示例：

addfriend:12
creategroup:groupname:groupdesc

创建群组。

示例：

creategroup:testgroup:for_demo
addgroup:groupid

加入群组。

示例：

addgroup:1
groupchat:groupid:message

在指定群中发送消息。

示例：

groupchat:1:hello everyone
history:count

查询当前登录用户最近 count 条单聊历史消息。

示例：

history:5
grouphistory:groupid:count

查询某个群最近 count 条群聊历史消息。

示例：

grouphistory:1:5
loginout

注销当前用户。

9. Qt 客户端说明

项目新增了一套基于 Qt Widgets + QTcpSocket 的图形客户端，用于演示客户端与 C++ 服务端之间的完整交互流程。

当前 Qt 客户端已经完成以下能力：

用户登录
用户注册
主界面展示
好友列表展示
群组列表展示
添加好友
创建群组
加入群组
单聊窗口
群聊窗口
单聊历史消息加载
群聊历史消息加载
注销登录（logout）
Qt 客户端构建
cd qt_client
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_PREFIX_PATH=/home/ys/Qt/6.11.0/gcc_64/lib/cmake
make -j$(nproc)
Qt 客户端运行
cd qt_client/build
./QtChatClient
10. Qt 客户端架构说明

随着功能增加，Qt 客户端从最初所有逻辑集中在 main.cpp，逐步重构为更清晰的分层结构。

当前分层
1. UI 层

位于 qt_client/include/ui 和 qt_client/src/ui。

负责界面展示与交互，包括：

LoginWindow
RegisterDialog
MainWindow
AddFriendDialog
CreateGroupDialog
JoinGroupDialog
ChatWindow
GroupChatWindow
2. Network 层

位于 qt_client/include/network 和 qt_client/src/network。

核心类：

ChatClient

职责：

建立与服务端的 TCP 连接
发送 JSON 请求
接收并解析服务端响应
通过 Qt signal 向上层分发事件
3. Service 层

位于 qt_client/include/service 和 qt_client/src/service。

核心类：

SessionManager

职责：

保存当前登录用户信息
缓存好友名字映射
缓存群成员名字映射
提供会话状态访问接口
4. Controller 层

位于 qt_client/include/controller 和 qt_client/src/controller。

核心类：

ClientController

职责：

统一管理窗口与网络层之间的信号槽绑定
管理登录、单聊、群聊、关系操作、注销等流程
降低 main.cpp 复杂度
Qt 客户端重构过程
第一轮重构

将登录态和名字缓存从 main.cpp 中抽离，封装为 SessionManager。

第二轮重构

将原本集中在 main.cpp 中的大量 QObject::connect(...) 逻辑抽离到 ClientController。

第三轮重构

将 ClientController::setupConnections() 按功能进一步拆分为：

setupLoginFlow()
setupChatFlow()
setupGroupFlow()
setupFriendRelationFlow()
setupGroupRelationFlow()
setupLogoutFlow()
当前入口

当前 main.cpp 只负责：

创建 Qt 窗口对象
创建 ChatClient
创建 SessionManager
创建 ClientController
启动 Qt 应用

这使客户端结构更接近实际项目中的模块化组织方式。

11. Qt 客户端界面展示

下面展示 Qt 图形客户端的主要界面与功能效果。

1. 登录界面

用于输入用户 id 和密码，向服务端发送登录请求。

2. 主界面

登录成功后进入主界面，展示当前用户信息、好友列表和群组列表，并提供添加好友、创建群组、加入群组、注销等操作。

3. 添加好友对话框

输入好友 id 后向服务端发送添加好友请求，成功后主界面好友列表即时刷新。

4. 单聊窗口

支持单聊消息发送，并自动加载最近单聊历史消息。

5. 群聊窗口

支持群聊消息发送，并自动加载最近群聊历史消息。

6. 注册对话框

用于新用户注册，服务端返回新分配的用户 id。

7. 创建群组对话框

输入群名称和群描述，创建成功后主界面群组列表即时刷新。

8. 加入群组对话框

输入群 id，加入成功后主界面群组列表即时刷新。

12. 功能验证
12.1 单聊落库验证
启动服务端
启动客户端并登录
向好友发送消息，例如：
chat:12:hello
查询数据库：
mysql -u root -p -e "use chat; select * from chatmessage order by id desc limit 10;"
12.2 带单引号消息验证

发送：

chat:12:I'm fine

然后查询：

mysql -u root -p -e "use chat; select * from chatmessage order by id desc limit 10;"
mysql -u root -p -e "use chat; select * from offlinemessage;"

如果修复成功，数据库中应能看到完整消息，而不是 SQL 插入失败。

12.3 单聊历史查询验证

客户端登录后输入：

history:5

如果已有历史消息，客户端会输出最近 5 条单聊历史。

12.4 群聊历史查询验证

客户端登录后输入：

grouphistory:1:5

如果群聊历史存在，客户端会输出最近 5 条群聊历史。

13. 回归测试记录

详细测试记录见：

docs/test.md

建议至少覆盖以下场景：

注册
登录
添加好友
创建群组
加入群组
单聊发送
群聊发送
单聊历史加载
群聊历史加载
logout
添加自己为好友
添加不存在的好友 id
创建重复群名
加入不存在的群 id
14. 核心模块说明
ChatServer

负责：

启动 Muduo TCP 服务
处理连接建立 / 断开
接收客户端消息
将消息交给 ChatService
ChatService

负责：

业务分发
登录 / 注册 / 注销
单聊 / 群聊
添加好友 / 建群 / 加群
历史消息查询
离线消息处理
Redis 订阅消息处理
Config

负责：

读取 conf/chatserver.json
提供服务端 / MySQL / Redis 相关配置访问接口
MySQL 封装

负责：

建立 MySQL 连接
执行更新 SQL
执行查询 SQL
ChatHistoryModel

负责：

单聊历史落库
群聊历史落库
最近单聊历史查询
最近群聊历史查询
15. 已完成的工程化改进
项目目录结构整理
增加 conf/、docs/、proto/、sql/、script/
增加统一构建与运行脚本
服务端配置改为 JSON 文件读取
新增聊天记录表与历史消息查询
修复消息内容包含单引号时的 SQL 插入失败问题
增加 Protobuf 生成脚本与编译接入
增加 Qt 图形客户端
将 Qt 客户端中的会话状态抽取为 SessionManager
将 Qt 客户端中的信号槽绑定抽取为 ClientController
将 Qt 控制器按功能流程进一步拆分
16. 当前边界与后续方向

当前项目仍可继续优化：

将字符串拼接 SQL 进一步替换为 prepared statement
引入数据库连接池
正式将 JSON 协议逐步迁移为 Protobuf
增加性能分析与轻量压测
完善部署文档与自动化测试
增强 Qt 客户端体验，例如未读消息、成员列表、消息气泡等
17. 简历描述参考

基于 Muduo 的 C++ 聊天系统优化版

使用 Muduo 实现高并发 TCP 服务端
使用 MySQL 存储用户、好友、群组、离线消息和聊天记录
使用 Redis 发布订阅支持跨服务器消息转发
将服务端配置改造为 JSON 文件驱动
新增聊天记录持久化与历史消息查询功能
实现 Qt 图形客户端，支持好友聊天、群聊与会话管理
对项目进行目录重构、脚本化构建和模块化整理
18. 致谢

本项目基于原始课程聊天室项目继续整理与迭代，主要用于：

C++ 后端开发学习
Linux 网络编程训练
MySQL / Redis 实践
求职项目展示
