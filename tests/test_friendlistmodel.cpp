// 测试模块：FriendListModel（Qt QAbstractListModel）
// 覆盖：增删查、重复防重、角色数据、行数变化

#include <gtest/gtest.h>
#include <QCoreApplication>
#include "model/friendlistmodel.h"

// 全局 QCoreApplication（Qt 元对象系统需要）
static int g_argc = 1;
static char g_appName[] = "test";
static char* g_argv[] = { g_appName, nullptr };

TEST(FriendListModelTest, EmptyModelHasZeroRows)
{
    QCoreApplication app(g_argc, g_argv);
    FriendListModel model;
    EXPECT_EQ(model.rowCount(), 0);
}

TEST(FriendListModelTest, AddFriendIncreasesRowCount)
{
    QCoreApplication app(g_argc, g_argv);
    FriendListModel model;

    model.addFriend(1, "alice", "online");
    EXPECT_EQ(model.rowCount(), 1);

    model.addFriend(2, "bob", "offline");
    EXPECT_EQ(model.rowCount(), 2);
}

TEST(FriendListModelTest, DisplayRoleShowsFormattedString)
{
    QCoreApplication app(g_argc, g_argv);
    FriendListModel model;
    model.addFriend(1, "alice", "online");

    QModelIndex idx = model.index(0);
    QString text = model.data(idx, Qt::DisplayRole).toString();
    EXPECT_TRUE(text.contains("alice"));
    EXPECT_TRUE(text.contains("1"));
    EXPECT_TRUE(text.contains("online"));
}

TEST(FriendListModelTest, CustomRolesReturnCorrectValues)
{
    QCoreApplication app(g_argc, g_argv);
    FriendListModel model;
    model.addFriend(42, "charlie", "offline");

    QModelIndex idx = model.index(0);
    EXPECT_EQ(model.data(idx, FriendListModel::FriendIdRole).toInt(), 42);
    EXPECT_EQ(model.data(idx, FriendListModel::FriendNameRole).toString(), "charlie");
    EXPECT_EQ(model.data(idx, FriendListModel::FriendStateRole).toString(), "offline");
}

TEST(FriendListModelTest, DuplicateAddIgnored)
{
    QCoreApplication app(g_argc, g_argv);
    FriendListModel model;

    model.addFriend(1, "alice", "online");
    model.addFriend(1, "alice_dup", "offline");  // 同 id → 应忽略

    EXPECT_EQ(model.rowCount(), 1);
    EXPECT_EQ(model.friendNameAt(0), "alice");     // 保留原始名字
}

TEST(FriendListModelTest, ContainsReturnsCorrectly)
{
    QCoreApplication app(g_argc, g_argv);
    FriendListModel model;
    model.addFriend(1, "alice", "online");

    EXPECT_TRUE(model.contains(1));
    EXPECT_FALSE(model.contains(999));
}

TEST(FriendListModelTest, ClearResetsModel)
{
    QCoreApplication app(g_argc, g_argv);
    FriendListModel model;
    model.addFriend(1, "alice", "online");
    model.addFriend(2, "bob", "offline");
    EXPECT_EQ(model.rowCount(), 2);

    model.clear();
    EXPECT_EQ(model.rowCount(), 0);
}

TEST(FriendListModelTest, InvalidIndexReturnsDefault)
{
    QCoreApplication app(g_argc, g_argv);
    FriendListModel model;

    // 空 model 下 index(0) 无效
    QVariant data = model.data(model.index(0), Qt::DisplayRole);
    EXPECT_FALSE(data.isValid());
}
