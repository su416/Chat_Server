#ifndef GROUPLISTMODEL_H
#define GROUPLISTMODEL_H

#include <QAbstractListModel>
#include <QVector>
#include <QString>

// 群列表 Model：驱动 QListView，替代原来的 QListWidget + setData
class GroupListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        GroupIdRole = Qt::UserRole,
        GroupNameRole,
        GroupDescRole
    };

    struct GroupInfo {
        int id;
        QString name;
        QString desc;
    };

    explicit GroupListModel(QObject *parent = nullptr);

    // QAbstractListModel 接口
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    // 数据操作
    void addGroup(int id, const QString &name, const QString &desc);
    void clear();
    bool contains(int id) const;
    int groupIdAt(int row) const;
    QString groupNameAt(int row) const;

private:
    QVector<GroupInfo> m_groups;
};

#endif
