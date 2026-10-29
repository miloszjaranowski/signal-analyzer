#include "ChannelTable.hpp"

#include <QHeaderView>
#include <QSignalBlocker>

#include <algorithm>

ChannelTable::ChannelTable(const QStringList& columns, QWidget* parent) : QTableWidget(parent) {
    setColumnCount(int(columns.size()) + 1);
    setHorizontalHeaderLabels(QStringList{tr("Ch")} + columns);
    verticalHeader()->hide();
    horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    horizontalHeader()->setStretchLastSection(true);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    fitHeight();

    connect(this, &QTableWidget::itemSelectionChanged, this, [this] {
        if (const auto channel = selectedChannel()) {
            emit channelSelected(*channel);
        }
    });
}

void ChannelTable::setRows(const std::vector<Row>& rows) {
    const auto selected = selectedChannel();
    const QSignalBlocker blocker(this);

    if (rowCount() != int(rows.size())) {
        setRowCount(int(rows.size()));
        fitHeight();
    }
    for (int r = 0; r < int(rows.size()); ++r) {
        const Row& row = rows[std::size_t(r)];
        setCell(r, 0, QString::number(row.channel));
        item(r, 0)->setData(Qt::UserRole, row.channel);
        for (int c = 0; c < int(row.cells.size()) && c + 1 < columnCount(); ++c) {
            setCell(r, c + 1, row.cells[c]);
        }
    }
    if (selected) {
        selectChannel(*selected);  // a new channel may have shifted the rows
    }
}

std::optional<std::uint16_t> ChannelTable::selectedChannel() const {
    const auto rows = selectionModel() ? selectionModel()->selectedRows() : QModelIndexList{};
    if (rows.isEmpty()) {
        return std::nullopt;
    }
    const QTableWidgetItem* first = item(rows.front().row(), 0);
    if (!first) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(first->data(Qt::UserRole).toUInt());
}

void ChannelTable::selectChannel(std::uint16_t channel) {
    for (int r = 0; r < rowCount(); ++r) {
        const QTableWidgetItem* first = item(r, 0);
        if (first && first->data(Qt::UserRole).toUInt() == channel) {
            if (!selectionModel()->isRowSelected(r, QModelIndex())) {
                selectRow(r);
            }
            return;
        }
    }
}

void ChannelTable::setCell(int row, int column, const QString& text) {
    if (QTableWidgetItem* existing = item(row, column)) {
        if (existing->text() != text) {
            existing->setText(text);
        }
    } else {
        setItem(row, column, new QTableWidgetItem(text));
    }
}

void ChannelTable::fitHeight() {
    const int rows = std::clamp(rowCount(), 1, kVisibleRows);
    setFixedHeight(horizontalHeader()->sizeHint().height() +
                   rows * verticalHeader()->defaultSectionSize() + 2 * frameWidth());
}
