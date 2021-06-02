#pragma once

#include <optional>

#include <Interpreters/Set.h>
#include <Core/SortDescription.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTFunction.h>
#include <Storages/SelectQueryInfo.h>


namespace DB
{

class Context;
class IFunction;
using FunctionBasePtr = std::shared_ptr<IFunctionBase>;
class ExpressionActions;
using ExpressionActionsPtr = std::shared_ptr<ExpressionActions>;

/** A field, that can be stored in two representations:
  * - A standalone field.
  * - A field with reference to its position in a block.
  *   It's needed for execution of functions on ranges during
  *   index analysis. If function was executed once for field,
  *   its result would be cached for whole block for which field's reference points to.
  */
struct FieldRef : public Field
{
    FieldRef() = default;

    /// Create as explicit field without block.
    template <typename T>
    FieldRef(T && value) : Field(std::forward<T>(value)) {}

    /// Create as reference to field in block.
    FieldRef(ColumnsWithTypeAndName * columns_, size_t row_idx_, size_t column_idx_)
        : Field((*(*columns_)[column_idx_].column)[row_idx_]),
          columns(columns_), row_idx(row_idx_), column_idx(column_idx_) {}

    bool isExplicit() const { return columns == nullptr; }

    ColumnsWithTypeAndName * columns = nullptr;
    size_t row_idx = 0;
    size_t column_idx = 0;
};

/** Range with open or closed ends; possibly unbounded.
  */
struct Range
{
private:
    static bool equals(const Field & lhs, const Field & rhs);
    static bool less(const Field & lhs, const Field & rhs);

public:
    FieldRef left;                       /// the left border, if any
    FieldRef right;                      /// the right border, if any
    bool left_bounded = false;        /// bounded at the left
    bool right_bounded = false;       /// bounded at the right
    bool left_included = false;       /// includes the left border, if any
    bool right_included = false;      /// includes the right border, if any

    /// The whole unversum.
    Range() {}

    /// One point.
    Range(const FieldRef & point)
        : left(point), right(point), left_bounded(true), right_bounded(true), left_included(true), right_included(true) {}

    /// A bounded two-sided range.
    Range(const FieldRef & left_, bool left_included_, const FieldRef & right_, bool right_included_)
        : left(left_), right(right_),
        left_bounded(true), right_bounded(true),
        left_included(left_included_), right_included(right_included_)
    {
        shrinkToIncludedIfPossible();
    }

    static Range createRightBounded(const FieldRef & right_point, bool right_included)
    {
        Range r;
        r.right = right_point;
        r.right_bounded = true;
        r.right_included = right_included;
        r.shrinkToIncludedIfPossible();
        return r;
    }

    static Range createLeftBounded(const FieldRef & left_point, bool left_included)
    {
        Range r;
        r.left = left_point;
        r.left_bounded = true;
        r.left_included = left_included;
        r.shrinkToIncludedIfPossible();
        return r;
    }

    /** Optimize the range. If it has an open boundary and the Field type is "loose"
      * - then convert it to closed, narrowing by one.
      * That is, for example, turn (0,2) into [1].
      */
    void shrinkToIncludedIfPossible()
    {
        if (left.isExplicit() && left_bounded && !left_included)
        {
            if (left.getType() == Field::Types::UInt64 && left.get<UInt64>() != std::numeric_limits<UInt64>::max())
            {
                ++left.get<UInt64 &>();
                left_included = true;
            }
            if (left.getType() == Field::Types::Int64 && left.get<Int64>() != std::numeric_limits<Int64>::max())
            {
                ++left.get<Int64 &>();
                left_included = true;
            }
        }
        if (right.isExplicit() && right_bounded && !right_included)
        {
            if (right.getType() == Field::Types::UInt64 && right.get<UInt64>() != std::numeric_limits<UInt64>::min())
            {
                --right.get<UInt64 &>();
                right_included = true;
            }
            if (right.getType() == Field::Types::Int64 && right.get<Int64>() != std::numeric_limits<Int64>::min())
            {
                --right.get<Int64 &>();
                right_included = true;
            }
        }
    }

    bool empty() const
    {
        return left_bounded && right_bounded
            && (less(right, left)
                || ((!left_included || !right_included) && !less(left, right)));
    }

    /// x contained in the range
    bool contains(const FieldRef & x) const
    {
        return !leftThan(x) && !rightThan(x);
    }

    /// x is to the left
    bool rightThan(const FieldRef & x) const
    {
        return (left_bounded
            ? !(less(left, x) || (left_included && equals(x, left)))
            : false);
    }

    /// x is to the right
    bool leftThan(const FieldRef & x) const
    {
        return (right_bounded
            ? !(less(x, right) || (right_included && equals(x, right)))
            : false);
    }

    bool intersectsRange(const Range & r) const
    {
        /// r to the left of me.
        if (r.right_bounded
            && left_bounded
            && (less(r.right, left)
                || ((!left_included || !r.right_included)
                    && equals(r.right, left))))
            return false;

        /// r to the right of me.
        if (r.left_bounded
            && right_bounded
            && (less(right, r.left)                          /// ...} {...
                || ((!right_included || !r.left_included)    /// ...) [... or ...] (...
                    && equals(r.left, right))))
            return false;

        return true;
    }

    bool containsRange(const Range & r) const
    {
        /// r starts to the left of me.
        if (left_bounded
            && (!r.left_bounded
                || less(r.left, left)
                || (r.left_included
                    && !left_included
                    && equals(r.left, left))))
            return false;

        /// r ends right of me.
        if (right_bounded
            && (!r.right_bounded
                || less(right, r.right)
                || (r.right_included
                    && !right_included
                    && equals(r.right, right))))
            return false;

        return true;
    }

    void swapLeftAndRight()
    {
        std::swap(left, right);
        std::swap(left_bounded, right_bounded);
        std::swap(left_included, right_included);
    }

    String toString() const;
};

/** Condition on the index.
  *
  * Consists of the conditions for the key belonging to all possible ranges or sets,
  *  as well as logical operators AND/OR/NOT above these conditions.
  *
  * Constructs a reverse polish notation from these conditions
  *  and can calculate (interpret) its satisfiability over key ranges.
  */
class KeyCondition
{
public:
    /// Does not take into account the SAMPLE section. all_columns - the set of all columns of the table.
    KeyCondition(
        const SelectQueryInfo & query_info,
        ContextPtr context,
        const Names & key_column_names,
        const ExpressionActionsPtr & key_expr,
        bool single_point_ = false,
        bool strict_ = false);

    /// Whether the condition and its negation are feasible in the direct product of single column ranges specified by `hyperrectangle`.
    BoolMask checkInHyperrectangle(
        const std::vector<Range> & hyperrectangle,
        const DataTypes & data_types) const;

    /// Whether the condition and its negation are (independently) feasible in the key range.
    /// left_key and right_key must contain all fields in the sort_descr in the appropriate order.
    /// data_types - the types of the key columns.
    /// Argument initial_mask is used for early exiting the implementation when we do not care about
    /// one of the resulting mask components (see BoolMask::consider_only_can_be_XXX).
    BoolMask checkInRange(
        size_t used_key_size,
        const FieldRef * left_key,
        const FieldRef* right_key,
        const DataTypes & data_types,
        BoolMask initial_mask = BoolMask(false, false)) const;

    /// Are the condition and its negation valid in a semi-infinite (not limited to the right) key range.
    /// left_key must contain all the fields in the sort_descr in the appropriate order.
    BoolMask checkAfter(
        size_t used_key_size,
        const FieldRef * left_key,
        const DataTypes & data_types,
        BoolMask initial_mask = BoolMask(false, false)) const;

    /// Same as checkInRange, but calculate only may_be_true component of a result.
    /// This is more efficient than checkInRange(...).can_be_true.
    bool mayBeTrueInRange(
        size_t used_key_size,
        const FieldRef * left_key,
        const FieldRef * right_key,
        const DataTypes & data_types) const;

    /// Same as checkAfter, but calculate only may_be_true component of a result.
    /// This is more efficient than checkAfter(...).can_be_true.
    bool mayBeTrueAfter(
        size_t used_key_size,
        const FieldRef * left_key,
        const DataTypes & data_types) const;

    /// Checks that the index can not be used
    /// FUNCTION_UNKNOWN will be AND'ed (if any).
    bool alwaysUnknownOrTrue() const;
    /// Checks that the index can not be used
    /// Does not allow any FUNCTION_UNKNOWN (will instantly return true).
    bool anyUnknownOrAlwaysTrue() const;

    /// Get the maximum number of the key element used in the condition.
    size_t getMaxKeyColumn() const;

    bool hasMonotonicFunctionsChain() const;

    /// Impose an additional condition: the value in the column `column` must be in the range `range`.
    /// Returns whether there is such a column in the key.
    bool addCondition(const String & column, const Range & range);

    String toString() const;

    /// Condition description for EXPLAIN query.
    struct Description
    {
        /// Which columns from PK were used, in PK order.
        std::vector<std::string> used_keys;
        /// Condition which was applied, mostly human-readable.
        std::string condition;
    };

    Description getDescription() const;

    /** A chain of possibly monotone functions.
      * If the key column is wrapped in functions that can be monotonous in some value ranges
      * (for example: -toFloat64(toDayOfWeek(date))), then here the functions will be located: toDayOfWeek, toFloat64, negate.
      */
    using MonotonicFunctionsChain = std::vector<FunctionBasePtr>;

    /** Computes value of constant expression and its data type.
      * Returns false, if expression isn't constant.
      */
    static bool getConstant(
            const ASTPtr & expr, Block & block_with_constants, Field & out_value, DataTypePtr & out_type);

    static Block getBlockWithConstants(
        const ASTPtr & query, const TreeRewriterResultPtr & syntax_analyzer_result, ContextPtr context);

    static std::optional<Range> applyMonotonicFunctionsChainToRange(
        Range key_range,
        const MonotonicFunctionsChain & functions,
        DataTypePtr current_type,
        bool single_point = false);

    bool matchesExactContinuousRange() const;

private:
    /// The expression is stored as Reverse Polish Notation.
    struct RPNElement
    {
        enum Function
        {
            /// Atoms of a Boolean expression.
            FUNCTION_IN_RANGE,
            FUNCTION_NOT_IN_RANGE,
            FUNCTION_IN_SET,
            FUNCTION_NOT_IN_SET,
            FUNCTION_UNKNOWN, /// Can take any value.
            /// Operators of the logical expression.
            FUNCTION_NOT,
            FUNCTION_AND,
            FUNCTION_OR,
            /// Constants
            ALWAYS_FALSE,
            ALWAYS_TRUE,
        };

        RPNElement() {}
        RPNElement(Function function_) : function(function_) {}
        RPNElement(Function function_, size_t key_column_) : function(function_), key_column(key_column_) {}
        RPNElement(Function function_, size_t key_column_, const Range & range_)
            : function(function_), range(range_), key_column(key_column_) {}

        String toString() const;
        String toString(const std::string_view & column_name, bool print_constants) const;

        Function function = FUNCTION_UNKNOWN;

        /// For FUNCTION_IN_RANGE and FUNCTION_NOT_IN_RANGE.
        Range range;
        size_t key_column = 0;
        /// For FUNCTION_IN_SET, FUNCTION_NOT_IN_SET
        using MergeTreeSetIndexPtr = std::shared_ptr<const MergeTreeSetIndex>;
        MergeTreeSetIndexPtr set_index;

        MonotonicFunctionsChain monotonic_functions_chain;
    };

    using RPN = std::vector<RPNElement>;
    using ColumnIndices = std::map<String, size_t>;

    using AtomMap = std::unordered_map<std::string, bool(*)(RPNElement & out, const Field & value)>;

public:
    static const AtomMap atom_map;

private:

    class Tree;
    class FunctionTree;

    BoolMask checkInRange(
        size_t used_key_size,
        const FieldRef * left_key,
        const FieldRef * right_key,
        const DataTypes & data_types,
        bool right_bounded,
        BoolMask initial_mask) const;

    void traverseAST(const Tree & node, ContextPtr context, Block & block_with_constants);
    bool tryParseAtomFromAST(const Tree & node, ContextPtr context, Block & block_with_constants, RPNElement & out);
    static bool tryParseLogicalOperatorFromAST(const FunctionTree & func, RPNElement & out);

    /** Is node the key column
      *  or expression in which column of key is wrapped by chain of functions,
      *  that can be monotonic on certain ranges?
      * If these conditions are true, then returns number of column in key, type of resulting expression
      *  and fills chain of possibly-monotonic functions.
      */
    bool isKeyPossiblyWrappedByMonotonicFunctions(
        const Tree & node,
        ContextPtr context,
        size_t & out_key_column_num,
        DataTypePtr & out_key_res_column_type,
        MonotonicFunctionsChain & out_functions_chain);

    bool isKeyPossiblyWrappedByMonotonicFunctionsImpl(
        const Tree & node,
        size_t & out_key_column_num,
        DataTypePtr & out_key_column_type,
        std::vector<FunctionTree> & out_functions_chain);

    bool canConstantBeWrappedByMonotonicFunctions(
        const ASTPtr & node,
        size_t & out_key_column_num,
        DataTypePtr & out_key_column_type,
        Field & out_value,
        DataTypePtr & out_type);

    bool canConstantBeWrappedByFunctions(
        const ASTPtr & ast, size_t & out_key_column_num, DataTypePtr & out_key_column_type, Field & out_value, DataTypePtr & out_type);

    /// Check if ASTPtr node, passed to canConstantBeWrappedBy*, can be used by them for further checks.
    /// Always call this method at start of other methods, which require key comparison, because it also checks if adjusted
    /// key expression can also be used (with substitution from modulo to moduloLegacy). This is needed because partition key
    /// is always modified, when passed into keyCondition, - with recursive substitution from modulo to moduloLegacy.
    bool canConstantBeWrapped(const ASTPtr & node, const String & expr_name, String & result_expr_name);

    /// If it's possible to make an RPNElement
    /// that will filter values (possibly tuples) by the content of 'prepared_set',
    /// do it and return true.
    bool tryPrepareSetIndex(
        const ASTs & args,
        ContextPtr context,
        RPNElement & out,
        size_t & out_key_column_num);

    /// Checks that the index can not be used.
    ///
    /// If unknown_any is false (used by alwaysUnknownOrTrue()), then FUNCTION_UNKNOWN can be AND'ed,
    /// otherwise (anyUnknownOrAlwaysTrue()) first FUNCTION_UNKNOWN will return true (index cannot be used).
    ///
    /// Consider the following example:
    ///
    ///     CREATE TABLE test(p DateTime, k int) ENGINE MergeTree PARTITION BY toDate(p) ORDER BY k;
    ///     INSERT INTO test VALUES ('2020-09-01 00:01:02', 1), ('2020-09-01 20:01:03', 2), ('2020-09-02 00:01:03', 3);
    ///
    /// - SELECT count() FROM test WHERE toDate(p) >= '2020-09-01' AND p <= '2020-09-01 00:00:00'
    ///   In this case rpn will be (FUNCTION_IN_RANGE, FUNCTION_UNKNOWN (due to strict), FUNCTION_AND)
    ///   and for optimize_trivial_count_query we cannot use index if there is at least one FUNCTION_UNKNOWN.
    ///   since there is no post processing and return count() based on only the first predicate is wrong.
    ///
    /// - SELECT * FROM test WHERE toDate(p) >= '2020-09-01' AND p <= '2020-09-01 00:00:00'
    ///   In this case will be (FUNCTION_IN_RANGE, FUNCTION_IN_RANGE (due to non-strict), FUNCTION_AND)
    ///   so it will prune everything out and nothing will be read.
    ///
    /// - SELECT * FROM test WHERE toDate(p) >= '2020-09-01' AND toUnixTimestamp(p)%5==0
    ///   In this case will be (FUNCTION_IN_RANGE, FUNCTION_UNKNOWN, FUNCTION_AND)
    ///   and all, two, partitions will be scanned, but due to filtering later none of rows will be matched.
    bool unknownOrAlwaysTrue(bool unknown_any) const;

    RPN rpn;

    ColumnIndices key_columns;
    ExpressionActionsPtr key_expr;
    PreparedSets prepared_sets;

    // If true, always allow key_expr to be wrapped by function
    bool single_point;
    // If true, do not use always_monotonic information to transform constants
    bool strict;
};

}
