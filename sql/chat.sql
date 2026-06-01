create database if not exists chat charset=utf8mb4;
use chat;

-- 用户表
create table if not exists user (
    id int primary key auto_increment,
    name varchar(50) not null unique,
    password varchar(128) not null,
    state varchar(20) not null default 'offline'
) engine=InnoDB default charset=utf8mb4;

-- 好友关系表
create table if not exists friend (
    userid int not null,
    friendid int not null,
    primary key (userid, friendid)
) engine=InnoDB default charset=utf8mb4;

-- 群组表
create table if not exists allgroup (
    id int primary key auto_increment,
    groupname varchar(50) not null,
    groupdesc varchar(200) default ''
) engine=InnoDB default charset=utf8mb4;

-- 群成员表（统一使用小写 groupuser）
create table if not exists groupuser (
    groupid int not null,
    userid int not null,
    grouprole varchar(20) not null default 'normal',
    primary key (groupid, userid)
) engine=InnoDB default charset=utf8mb4;

-- 离线消息已合并到 chatmessage 表（status=2 表示离线待投递）
-- chatmessage 表定义见 sql/chat_history.sql
