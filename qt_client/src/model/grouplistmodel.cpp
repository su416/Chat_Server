#include "model/grouplistmodel.h"

GroupListModel::GroupListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int GroupListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return static_cast<int>(m_groups.size());
}

QVariant GroupListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_groups.size())
        return {};

    const auto &g = m_groups[index.row()];

    switch (role) {
    case Qt::DisplayRole:
        if (g.desc.isEmpty())
            return QString("%1 %2").arg(g.id).arg(g.name);
        return QString("%1 %2 - %3").arg(g.id).arg(g.name).arg(g.desc);
    case GroupIdRole:
        return g.id;
    case GroupNameRole:
        return g.name;
    case GroupDescRole:
        return g.desc;
    default:
        return {};
    }
}

void GroupListModel::addGroup(int id, const QString &name, const QString &desc)
{
    if (contains(id)) return;

    int row = static_cast<int>(m_groups.size());
    beginInsertRows(QModelIndex(), row, row);
    m_groups.append({id, name, desc});
    endInsertRows();
}

void GroupListModel::clear()
{
    if (m_groups.isEmpty()) return;
    beginRemoveRows(QModelIndex(), 0, m_groups.size() - 1);
    m_groups.clear();
    endRemoveRows();
}

bool GroupListModel::contains(int id) const
{
    for (const auto &g : m_groups) {
        if (g.id == id) return true;
    }
    return false;
}

int GroupListModel::groupIdAt(int row) const
{
    if (row < 0 || row >= m_groups.size()) return -1;
    return m_groups[row].id;
}

QString GroupListModel::groupNameAt(int row) const
{
    if (row < 0 || row >= m_groups.size()) return {};
    return m_groups[row].name;
}
