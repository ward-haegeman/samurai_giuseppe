#pragma once

#include <iostream>

namespace mure
{
    /** An interval with storage index.
     *
     * @tparam TValue   Coordinate type (must be signed)
     * @tparam TIndex   Index type
     */
    template<typename TValue, typename TIndex = std::size_t>
    struct Interval
    {
        static_assert(std::is_signed<TValue>::value, "Coordinate type must be signed");

        using value_t = TValue;
        using index_t = TIndex;

        value_t start = 0;  ///< Interval start
        value_t end   = 0;  ///< Interval end + 1
        index_t index = 0;  ///< Storage index where start the interval's content.

        Interval() = default;

        Interval(value_t start, value_t end, index_t index = 0): start{start}, end{end}, index{index}
        {}

        inline bool contains(value_t x) const
        {
            return (x >= start && x < end);
        }

        inline value_t size() const
        {
            return (end - start);
        }

        inline bool is_valid() const
        {
            return (start < end);
        }
    };

    template<typename value_t, typename index_t>
    std::ostream& operator<<(std::ostream& out, const Interval<value_t, index_t>& interval)
    {
        out << "[" << interval.start << "," << interval.end << "[@" << interval.index;
        return out;
    }
}
