#ifndef FRIENDLISTMODEL_H
#define FRIENDLISTMODEL_H

#include <QAbstractListModel>
#include <QVector>
#include <QString>

// 好友列表 Model：驱动 QListView，替代原来的 QListWidget + setData
class FriendListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        FriendIdRole = Qt::UserRole,
        FriendNameRole,
        FriendStateRole
    };

    struct FriendInfo {
        int id;
        QString name;
        QString state;   // "online" / "offline"
    };

    explicit FriendListModel(QObject *parent = nullptr);

    // QAbstractListModel 接口
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    // 数据操作
    void addFriend(int id, const QString &name, const QString &state);
    void updateState(int id, const QString &state);
    void clear();
    bool contains(int id) const;
    int friendIdAt(int row) const;
    QString friendNameAt(int row) const;

private:
    QVector<FriendInfo> m_friends;
};

#endif
