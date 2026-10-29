#pragma once

#include <QStringList>
#include <QTableWidget>

#include <cstdint>
#include <optional>
#include <vector>

// One row per channel with its key metrics. Selecting a row picks the channel
// shown in the plots; updating the rows keeps that selection.
class ChannelTable : public QTableWidget {
    Q_OBJECT

public:
    struct Row {
        std::uint16_t channel = 0;
        QStringList cells;  // one per column after "Ch", in header order
    };

    explicit ChannelTable(const QStringList& columns, QWidget* parent = nullptr);

    void setRows(const std::vector<Row>& rows);  // does not emit channelSelected
    std::optional<std::uint16_t> selectedChannel() const;
    void selectChannel(std::uint16_t channel);

signals:
    void channelSelected(std::uint16_t channel);

private:
    void setCell(int row, int column, const QString& text);
    void fitHeight();  // shows up to kVisibleRows rows without a scroll bar

    static constexpr int kVisibleRows = 4;
};
