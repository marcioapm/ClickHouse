#include <cstddef>
#include <memory>
#include <type_traits>
#include <vector>
#include <Processors/Transforms/MergeJoinTransform.h>
#include <base/logger_useful.h>
#include <IO/WriteHelpers.h>
#include <Interpreters/TableJoin.h>
#include <Core/SortDescription.h>
#include <boost/core/noncopyable.hpp>
#include <Columns/ColumnsNumber.h>
#include <Columns/IColumn.h>
#include <Columns/ColumnNullable.h>
#include <Core/SortCursor.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <base/defines.h>
#include <base/types.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int TOO_MANY_ROWS;
    extern const int LOGICAL_ERROR;
}

constexpr size_t EMPTY_VALUE_IDX = std::numeric_limits<size_t>::max();

template <bool has_left_nulls, bool has_right_nulls>
static int nullableCompareAt(const IColumn & left_column, const IColumn & right_column, size_t lhs_pos, size_t rhs_pos, int null_direction_hint = 1)
{
    if constexpr (has_left_nulls && has_right_nulls)
    {
        const auto * left_nullable = checkAndGetColumn<ColumnNullable>(left_column);
        const auto * right_nullable = checkAndGetColumn<ColumnNullable>(right_column);

        if (left_nullable && right_nullable)
        {
            int res = left_column.compareAt(lhs_pos, rhs_pos, right_column, null_direction_hint);
            if (res)
                return res;

            /// NULL != NULL case
            if (left_column.isNullAt(lhs_pos))
                return null_direction_hint;

            return 0;
        }
    }

    if constexpr (has_left_nulls)
    {
        if (const auto * left_nullable = checkAndGetColumn<ColumnNullable>(left_column))
        {
            if (left_column.isNullAt(lhs_pos))
                return null_direction_hint;
            return left_nullable->getNestedColumn().compareAt(lhs_pos, rhs_pos, right_column, null_direction_hint);
        }
    }

    if constexpr (has_right_nulls)
    {
        if (const auto * right_nullable = checkAndGetColumn<ColumnNullable>(right_column))
        {
            if (right_column.isNullAt(rhs_pos))
                return -null_direction_hint;
            return left_column.compareAt(lhs_pos, rhs_pos, right_nullable->getNestedColumn(), null_direction_hint);
        }
    }

    return left_column.compareAt(lhs_pos, rhs_pos, right_column, null_direction_hint);
}

FullMergeJoinCursor::FullMergeJoinCursor(const Block & block, const SortDescription & desc_)
    : impl(block, desc_)
    , sample_block(block)
{
}

bool ALWAYS_INLINE FullMergeJoinCursor::sameNext() const
{
    if (!impl.isValid() || impl.isLast())
        return false;

    for (size_t i = 0; i < impl.sort_columns_size; ++i)
    {
        const auto & col = *impl.sort_columns[i];
        int cmp = nullableCompareAt<true, true>(
            col, col, impl.getRow(), impl.getRow() + 1, 0);
        if (cmp != 0)
            return false;
    }
    return true;
}

bool FullMergeJoinCursor::sameUnitlEnd() const
{
    if (!impl.isValid() || impl.isLast())
        return true;

    for (size_t i = 0; i < impl.sort_columns_size; ++i)
    {
        const auto & col = *impl.sort_columns[i];
        int cmp = nullableCompareAt<true, true>(
            col, col, impl.getRow(), impl.rows - 1, 0);
        if (cmp != 0)
            return false;
    }

    return true;
}

size_t FullMergeJoinCursor::nextDistinct()
{
    if (sameUnitlEnd())
        return 0;

    size_t start_pos = impl.getRow();
    while (sameNext())
    {
        impl.next();
    }
    impl.next();
    return impl.getRow() - start_pos;
}

void FullMergeJoinCursor::reset()
{
    current_input = {};
    resetInternalCursor();
}

const Chunk & FullMergeJoinCursor::getCurrentChunk() const
{
    return current_input.chunk;
}

void FullMergeJoinCursor::setInput(IMergingAlgorithm::Input && input)
{
    if (input.skip_last_row)
        throw Exception("FullMergeJoinCursor does not support skipLastRow", ErrorCodes::NOT_IMPLEMENTED);

    if (current_input.permutation)
        throw DB::Exception("FullMergeJoinCursor: permutation is not supported", ErrorCodes::NOT_IMPLEMENTED);


    current_input = std::move(input);

    if (!current_input.chunk)
        fully_completed = true;

    resetInternalCursor();
}

void FullMergeJoinCursor::resetInternalCursor()
{
    if (current_input.chunk)
    {
        impl.reset(current_input.chunk.getColumns(), sample_block, current_input.permutation);
    }
    else
    {
        impl.reset(sample_block.cloneEmpty().getColumns(), sample_block);
    }
}

namespace
{

FullMergeJoinCursor createCursor(const Block & block, const Names & columns)
{
    SortDescription desc;
    desc.reserve(columns.size());
    for (const auto & name : columns)
        desc.emplace_back(name);
    return FullMergeJoinCursor(block, desc);
}

/// If on_pos == true, compare two columns at specified positions.
/// Otherwise, compare two columns at the current positions, `lpos` and `rpos` are ignored.
template <typename Cursor, bool on_pos = false>
int ALWAYS_INLINE compareCursors(const Cursor & lhs, const Cursor & rhs,
                                 [[ maybe_unused ]] size_t lpos = 0,
                                 [[ maybe_unused ]] size_t rpos = 0)
{
    for (size_t i = 0; i < lhs->sort_columns_size; ++i)
    {
        const auto & desc = lhs->desc[i];
        int direction = desc.direction;
        int nulls_direction = desc.nulls_direction;

        int cmp = direction * nullableCompareAt<true, true>(
            *lhs->sort_columns[i],
            *rhs->sort_columns[i],
            on_pos ? lpos : lhs->getRow(),
            on_pos ? rpos : rhs->getRow(),
            nulls_direction);
        if (cmp != 0)
            return cmp;
    }
    return 0;
}

bool ALWAYS_INLINE totallyLess(const FullMergeJoinCursor & lhs, const FullMergeJoinCursor & rhs)
{
    if (lhs->rows == 0 || rhs->rows == 0)
        return false;

    if (!lhs->isValid() || !rhs->isValid())
        return false;

    /// The last row of this cursor is no larger than the first row of the another cursor.
    int cmp = compareCursors<FullMergeJoinCursor, true>(lhs, rhs, lhs->rows - 1, 0);
    return cmp < 0;
}

int ALWAYS_INLINE totallyCompare(const FullMergeJoinCursor & lhs, const FullMergeJoinCursor & rhs)
{
    if (totallyLess(lhs, rhs))
        return -1;
    if (totallyLess(rhs, lhs))
        return 1;
    return 0;
}

void addIndexColumn(const Columns & columns, ColumnUInt64 & indices, Chunk & result, size_t start, size_t limit)
{
    for (const auto & col : columns)
    {
        if (indices.empty())
        {
            result.addColumn(col->cut(start, limit));
        }
        else
        {
            if (limit == 0)
                limit = indices.size();

            assert(limit == indices.size());

            auto tmp_col = col->cloneResized(col->size() + 1);
            ColumnPtr new_col = tmp_col->index(indices, limit);
            result.addColumn(std::move(new_col));
        }
    }
}

}

MergeJoinAlgorithm::MergeJoinAlgorithm(
    JoinPtr table_join_,
    const Blocks & input_headers)
    : table_join(table_join_)
    , log(&Poco::Logger::get("MergeJoinAlgorithm"))
{
    if (input_headers.size() != 2)
        throw Exception("MergeJoinAlgorithm requires exactly two inputs", ErrorCodes::LOGICAL_ERROR);

    if (table_join->getTableJoin().strictness() != ASTTableJoin::Strictness::Any)
        throw Exception("MergeJoinAlgorithm is not implemented for strictness != ANY", ErrorCodes::NOT_IMPLEMENTED);

    const auto & join_on = table_join->getTableJoin().getOnlyClause();

    cursors.push_back(createCursor(input_headers[0], join_on.key_names_left));
    cursors.push_back(createCursor(input_headers[1], join_on.key_names_right));
}


static void copyColumnsResized(const Chunk & chunk, size_t start, size_t size, Chunk & result_chunk)
{
    const auto & cols = chunk.getColumns();
    for (const auto & col : cols)
    {
        if (!start || start > col->size())
            result_chunk.addColumn(col->cloneResized(size));
        else
        {
            assert(size <= col->size());
            result_chunk.addColumn(col->cut(start, size));
        }
    }
}

void MergeJoinAlgorithm::initialize(Inputs inputs)
{
    if (inputs.size() != 2)
        throw Exception("MergeJoinAlgorithm requires exactly two inputs", ErrorCodes::LOGICAL_ERROR);

    LOG_DEBUG(log, "MergeJoinAlgorithm initialize, number of inputs: {}", inputs.size());

    for (size_t i = 0; i < inputs.size(); ++i)
    {
        copyColumnsResized(inputs[i].chunk, 0, 0, sample_chunks.emplace_back());
        consume(inputs[i], i);
    }
}

static void prepareChunk(Chunk & chunk)
{
    auto num_rows = chunk.getNumRows();
    auto columns = chunk.detachColumns();
    for (auto & column : columns)
        column = column->convertToFullColumnIfConst();

    chunk.setColumns(std::move(columns), num_rows);
}

void MergeJoinAlgorithm::consume(Input & input, size_t source_num)
{
    LOG_DEBUG(log, "TODO: remove. Consume from {} chunk: {}", source_num, bool(input.chunk));

    prepareChunk(input.chunk);

    if (input.chunk.getNumRows() >= EMPTY_VALUE_IDX)
        throw Exception("Too many rows in input", ErrorCodes::TOO_MANY_ROWS);

    if (input.chunk)
        stat.num_blocks[source_num] += 1;

    cursors[source_num].setInput(std::move(input));
}

using JoinKind = ASTTableJoin::Kind;

template <JoinKind kind>
static std::optional<size_t> anyJoin(FullMergeJoinCursor & left_cursor, FullMergeJoinCursor & right_cursor, PaddedPODArray<UInt64> & left_map, PaddedPODArray<UInt64> & right_map)
{
    static_assert(kind == JoinKind::Left || kind == JoinKind::Right || kind == JoinKind::Inner, "Invalid join kind");

    size_t num_rows = kind == JoinKind::Left ? left_cursor->rowsLeft() :
                      kind == JoinKind::Right ? right_cursor->rowsLeft() :
                      std::min(left_cursor->rowsLeft(), right_cursor->rowsLeft());

    constexpr bool is_left_or_inner = kind == JoinKind::Left || kind == JoinKind::Inner;
    constexpr bool is_right_or_inner = kind == JoinKind::Right || kind == JoinKind::Inner;

    if constexpr (is_left_or_inner)
        right_map.reserve(num_rows);

    if constexpr (is_right_or_inner)
        left_map.reserve(num_rows);

    while (left_cursor->isValid() && right_cursor->isValid())
    {
        int cmp = compareCursors(left_cursor, right_cursor);
        if (cmp == 0)
        {
            if constexpr (is_left_or_inner)
                right_map.emplace_back(right_cursor->getRow());

            if constexpr (is_right_or_inner)
                left_map.emplace_back(left_cursor->getRow());

            if constexpr (is_left_or_inner)
                left_cursor->next();

            if constexpr (is_right_or_inner)
                right_cursor->next();

        }
        else if (cmp < 0)
        {
            size_t num = left_cursor.nextDistinct();
            if (num == 0)
                return 0;

            if constexpr (kind == JoinKind::Left)
                right_map.resize_fill(right_map.size() + num, right_cursor->rows);
        }
        else
        {
            size_t num = right_cursor.nextDistinct();
            if (num == 0)
                return 1;

            if constexpr (kind == JoinKind::Right)
                left_map.resize_fill(left_map.size() + num, left_cursor->rows);
        }
    }
    return std::nullopt;
}

static Chunk createBlockWithDefaults(const Chunk & lhs, const Chunk & rhs, size_t start, size_t num_rows)
{
    Chunk result;
    copyColumnsResized(lhs, start, num_rows, result);
    copyColumnsResized(rhs, start, num_rows, result);
    return result;
}

static Chunk createBlockWithDefaults(const Chunk & lhs, FullMergeJoinCursor & rhs)
{
    auto res = createBlockWithDefaults(lhs, rhs.getCurrentChunk(), rhs->getRow(), rhs->rowsLeft());
    rhs.reset();
    return res;
}

static Chunk createBlockWithDefaults(FullMergeJoinCursor & lhs, const Chunk & rhs)
{
    auto res = createBlockWithDefaults(lhs.getCurrentChunk(), rhs, lhs->getRow(), lhs->rowsLeft());
    lhs.reset();
    return res;
}

static bool isFinished(const std::vector<FullMergeJoinCursor> & cursors, JoinKind kind)
{
    return (cursors[0].fullyCompleted() && cursors[1].fullyCompleted())
        || ((isLeft(kind) || isInner(kind)) && cursors[0].fullyCompleted())
        || ((isRight(kind) || isInner(kind)) && cursors[1].fullyCompleted());
}

IMergingAlgorithm::Status MergeJoinAlgorithm::merge()
{
    if (required_input.has_value())
    {
        size_t r = required_input.value();
        required_input = {};
        return Status(r);
    }

    if (!cursors[0]->isValid() && !cursors[0].fullyCompleted())
    {
        return Status(0);
    }

    if (!cursors[1]->isValid() && !cursors[1].fullyCompleted())
    {
        return Status(1);
    }

    JoinKind kind = table_join->getTableJoin().kind();

    if (isFinished(cursors, kind))
    {
        return Status({}, true);
    }

    if (cursors[0].fullyCompleted() && isRightOrFull(kind))
    {
        Chunk result = createBlockWithDefaults(sample_chunks[0], cursors[1]);
        return Status(std::move(result));
    }

    if (isLeftOrFull(kind) && cursors[1].fullyCompleted())
    {
        Chunk result = createBlockWithDefaults(cursors[0], sample_chunks[1]);
        return Status(std::move(result));
    }

    if (int cmp = totallyCompare(cursors[0], cursors[1]); cmp != 0)
    {
        if (cmp < 0)
        {
            if (cursors[0]->isValid() && isLeftOrFull(kind))
            {
                return Status(createBlockWithDefaults(cursors[0], sample_chunks[1]));
            }
            cursors[0].reset();
            return Status(0);
        }

        if (cmp > 0)
        {
            if (isRightOrFull(kind) && cursors[1]->isValid())
            {
                return Status(createBlockWithDefaults(sample_chunks[0], cursors[1]));
            }
            cursors[1].reset();
            return Status(1);
        }

        if (!isInner(kind) && !isLeft(kind) && !isRight(kind) && !isFull(kind))
            throw DB::Exception(ErrorCodes::NOT_IMPLEMENTED, "Not implemented for kind {}", kind);
    }

    auto left_map = ColumnUInt64::create();
    auto right_map = ColumnUInt64::create();
    std::pair<size_t, size_t> prev_pos = std::make_pair(cursors[0]->getRow(), cursors[1]->getRow());
    if (isInner(kind))
    {
        required_input = anyJoin<JoinKind::Inner>(cursors[0], cursors[1], left_map->getData(), right_map->getData());
    }
    else if (isLeft(kind))
    {
        required_input = anyJoin<JoinKind::Left>(cursors[0], cursors[1], left_map->getData(), right_map->getData());
    }
    else if (isRight(kind))
    {
        required_input = anyJoin<JoinKind::Right>(cursors[0], cursors[1], left_map->getData(), right_map->getData());
    }
    else
    {
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Unsupported join kind: \"{}\"", table_join->getTableJoin().kind());
    }

    assert(left_map->empty() || right_map->empty() || left_map->size() == right_map->size());

    Chunk result;
    size_t num_result_rows = std::max(left_map->size(), right_map->size());
    addIndexColumn(cursors[0].getCurrentChunk().getColumns(), *left_map, result, prev_pos.first, num_result_rows);
    addIndexColumn(cursors[1].getCurrentChunk().getColumns(), *right_map, result, prev_pos.second, num_result_rows);
    return Status(std::move(result), isFinished(cursors, kind));
}

MergeJoinTransform::MergeJoinTransform(
        JoinPtr table_join,
        const Blocks & input_headers,
        const Block & output_header,
        UInt64 limit_hint)
    : IMergingTransform<MergeJoinAlgorithm>(input_headers, output_header, true, limit_hint, table_join, input_headers)
    , log(&Poco::Logger::get("MergeJoinTransform"))
{
    LOG_TRACE(log, "Will use MergeJoinTransform");
}

void MergeJoinTransform::onFinish()
{
    algorithm.onFinish(total_stopwatch.elapsedSeconds());
}


}
