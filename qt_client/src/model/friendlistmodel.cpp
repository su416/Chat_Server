#include "model/friendlistmodel.h"

FriendListModel::FriendListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int FriendListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return static_cast<int>(m_friends.size());
}

QVariant FriendListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_friends.size())
        return {};

    const auto &f = m_friends[index.row()];

    switch (role) {
    case Qt::DisplayRole:
        return QString("%1 %2 [%3]").arg(f.id).arg(f.name).arg(f.state);
    case FriendIdRole:
        return f.id;
    case FriendNameRole:
        return f.name;
    case FriendStateRole:
        return f.state;
    default:
        return {};
    }
}

void FriendListModel::addFriend(int id, const QString &name, const QString &state)
{
    // 防重复
    if (contains(id)) return;

    int row = static_cast<int>(m_friends.size());
    beginInsertRows(QModelIndex(), row, row);
    m_friends.append({id, name, state});
    endInsertRows();
}

void FriendListModel::updateState(int id, const QString &state)
{
    for (int i = 0; i < m_friends.size(); ++i) {
        if (m_friends[i].id == id) {
            m_friends[i].state = state;
            emit dataChanged(index(i), index(i), {Qt::DisplayRole, FriendStateRole});
            return;
        }
    }
}

void FriendListModel::clear()
{
    if (m_friends.isEmpty()) return;
    beginRemoveRows(QModelIndex(), 0, m_friends.size() - 1);
    m_friends.clear();
    endRemoveRows();
}

bool FriendListModel::contains(int id) const
{
    for (const auto &f : m_friends) {
        if (f.id == id) return true;
    }
    return false;
}

int FriendListModel::friendIdAt(int row) const
{
    if (row < 0 || row >= m_friends.size()) return -1;
    return m_friends[row].id;
}

QString FriendListModel::friendNameAt(int row) const
{
    if (row < 0 || row >= m_friends.size()) return {};
    return m_friends[row].name;
}

#include "moc_friendlistmodel.cpp"

