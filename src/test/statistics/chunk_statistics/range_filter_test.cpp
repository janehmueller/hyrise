#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base_test.hpp"
#include "gtest/gtest.h"

#include "utils/assert.hpp"

#include "statistics/chunk_statistics/min_max_filter.hpp"
#include "statistics/chunk_statistics/range_filter.hpp"
#include "statistics/empty_statistics_object.hpp"
#include "types.hpp"

namespace opossum {

template <typename T>
class RangeFilterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    _values = pmr_vector<T>{-1000, 2, 3, 4, 7, 8, 10, 17, 100, 101, 102, 103, 123456};
    _min_value = *std::min_element(std::begin(_values), std::end(_values));
    _max_value = *std::max_element(std::begin(_values), std::end(_values));

    // `_in_between` in a value in the largest gap of the test data.
    // When test data is changed, ensure that value is not part of a range in ranges unless |ranges| == 1.
    _in_between = static_cast<T>(_min_value + 0.5 * (_max_value - _min_value));

    _before_range = _min_value - 1;  // value smaller than the minimum
    _after_range = _max_value + 1;   // value larger than the maximum
  }

  std::shared_ptr<RangeFilter<T>> test_varying_range_filter_size(size_t gap_count, pmr_vector<T> value) {
    // RangeFilter constructor takes range count, not gap count
    auto filter = RangeFilter<T>::build_filter(_values, gap_count + 1);

    for (const auto& value : _values) {
      EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, {value}).type,
                EstimateType::MatchesApproximately);
    }

    // Find `gap_count` largest gaps. We use an std::{{set}} to discard repeated
    // values and directly iterate over an sorted order.
    auto value_set = std::set<T>(_values.begin(), _values.end(), std::less<T>());
    std::vector<std::pair<T, T>> begin_length_pairs;

    for (auto it = value_set.begin(); it != std::prev(value_set.end()); ++it) {
      auto begin = *it;
      auto end = *(std::next(it));
      begin_length_pairs.push_back(std::make_pair(begin, end - begin));
    }

    std::sort(begin_length_pairs.begin(), begin_length_pairs.end(),
              [](auto& left, auto& right) { return left.second > right.second; });

    for (auto gap_index = size_t{0}; gap_index < gap_count && gap_index < begin_length_pairs.size(); ++gap_index) {
      auto gap = begin_length_pairs[gap_index];
      auto begin = gap.first;
      auto length = gap.second;
      auto end = begin + length;
      EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, {begin}).type,
                EstimateType::MatchesApproximately);
      EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, {end}).type,
                EstimateType::MatchesApproximately);
      if constexpr (std::numeric_limits<T>::is_iec559) {
        auto value_in_gap = begin + 0.5 * length;
        EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, {value_in_gap}).type,
                  EstimateType::MatchesNone);
      } else if constexpr (std::is_integral_v<T>) {  // NOLINT
        if (length > 1) {
          EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, {++begin}).type,
                    EstimateType::MatchesNone);
        }
      }
    }

    // _in_between should always prune if we have more than one range
    if (gap_count > 1) {
      EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, _in_between).type, EstimateType::MatchesNone);
    }

    return filter;
  }

  pmr_vector<T> _values;
  T _before_range, _min_value, _max_value, _after_range, _in_between;
};

template <typename T>
T get_random_number(std::mt19937& rng, T min, T max) {
  if constexpr (std::is_same_v<T, int>) {
    std::uniform_int_distribution<T> uni(min, max);
    return uni(rng);
  } else {
    std::uniform_real_distribution<T> uni(min, max);
    return uni(rng);
  }
}

using FilterTypes = ::testing::Types<int, float, double>;
TYPED_TEST_CASE(RangeFilterTest, FilterTypes);

// a single range is basically a min/max filter
TYPED_TEST(RangeFilterTest, SingleRange) {
  auto filter = RangeFilter<TypeParam>::build_filter(this->_values, 1);

  for (const auto& value : this->_values) {
    EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, {value}).type,
              EstimateType::MatchesApproximately);
  }

  // testing for interval bounds
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::LessThan, {this->_min_value}).type,
            EstimateType::MatchesNone);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::GreaterThan, {this->_min_value}).type,
            EstimateType::MatchesApproximately);

  // cannot prune values in between, even though non-existent
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, {this->_in_between}).type,
            EstimateType::MatchesApproximately);

  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::LessThanEquals, {this->_max_value}).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::GreaterThan, {this->_max_value}).type,
            EstimateType::MatchesNone);
}

// create range filters with varying number of ranges/gaps
TYPED_TEST(RangeFilterTest, MultipleRanges) {
  for (auto gap_count = size_t{0}; gap_count < this->_values.size() * 2; ++gap_count) {
    this->test_varying_range_filter_size(gap_count, this->_values);
  }
}

// create more ranges than distinct values in the test data
TYPED_TEST(RangeFilterTest, MoreRangesThanValues) {
  auto filter = RangeFilter<TypeParam>::build_filter(this->_values, 10'000);

  for (const auto& value : this->_values) {
    EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, {value}).type,
              EstimateType::MatchesApproximately);
  }

  // testing for interval bounds
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::LessThan, {this->_min_value}).type,
            EstimateType::MatchesNone);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::GreaterThan, {this->_min_value}).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, {this->_in_between}).type,
            EstimateType::MatchesNone);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::LessThanEquals, {this->_max_value}).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::GreaterThan, {this->_max_value}).type,
            EstimateType::MatchesNone);
}

// this test checks the correct pruning on the bounds (min/max) of the test data for various predicate conditions
// for better understanding, see min_max_filter_test.cpp
TYPED_TEST(RangeFilterTest, CanPruneOnBounds) {
  auto filter = RangeFilter<TypeParam>::build_filter(this->_values);

  for (const auto& value : this->_values) {
    EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, {value}).type,
              EstimateType::MatchesApproximately);
  }

  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::LessThan, {this->_before_range}).type,
            EstimateType::MatchesNone);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::LessThan, {this->_min_value}).type,
            EstimateType::MatchesNone);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::LessThan, {this->_in_between}).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::LessThan, {this->_max_value}).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::LessThan, {this->_after_range}).type,
            EstimateType::MatchesApproximately);

  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::LessThanEquals, {this->_before_range}).type,
            EstimateType::MatchesNone);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::LessThanEquals, {this->_min_value}).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::LessThanEquals, {this->_in_between}).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::LessThanEquals, {this->_max_value}).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::LessThanEquals, {this->_after_range}).type,
            EstimateType::MatchesApproximately);

  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, {this->_before_range}).type,
            EstimateType::MatchesNone);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, {this->_min_value}).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, {this->_in_between}).type,
            EstimateType::MatchesNone);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, {this->_max_value}).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, {this->_after_range}).type,
            EstimateType::MatchesNone);

  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::GreaterThanEquals, {this->_before_range}).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::GreaterThanEquals, {this->_min_value}).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::GreaterThanEquals, {this->_in_between}).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::GreaterThanEquals, {this->_max_value}).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::GreaterThanEquals, {this->_after_range}).type,
            EstimateType::MatchesNone);

  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::GreaterThan, {this->_before_range}).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::GreaterThan, {this->_min_value}).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::GreaterThan, {this->_in_between}).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::GreaterThan, {this->_max_value}).type,
            EstimateType::MatchesNone);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::GreaterThan, {this->_after_range}).type,
            EstimateType::MatchesNone);
}

// test larger value ranges
TYPED_TEST(RangeFilterTest, LargeValueDomain) {
  std::random_device rd;
  auto rng = std::mt19937(rd());

  // values on which is the range filter is later built on
  pmr_vector<TypeParam> values;

  // We randomly create values between min_value(TypeParam) to -1000 and 1000 to max(TypeParam).
  // Any value in between should be prunable for values between -999 and 999
  for (auto i = size_t{0}; i < 10'000; ++i) {
    values.push_back(get_random_number<TypeParam>(rng, std::numeric_limits<TypeParam>::lowest(), -1000));
    values.push_back(get_random_number<TypeParam>(rng, 1000, std::numeric_limits<TypeParam>::max()));
  }

  std::vector<size_t> gap_counts = {1, 2, 4, 8, 16, 32, 64, values.size() + 1};
  for (auto gap_count : gap_counts) {
    // execute general tests and receive created range filter
    auto filter = this->test_varying_range_filter_size(gap_count, values);

    // additionally, test for further values
    for (auto i = size_t{0}; i < 100; ++i) {
      EXPECT_EQ(
          filter->estimate_cardinality(PredicateCondition::Equals, {get_random_number<TypeParam>(rng, 1000, 1000)})
              .type,
          EstimateType::MatchesNone);
    }
  }
}

TYPED_TEST(RangeFilterTest, Between) {
  const auto ranges = std::vector<std::pair<TypeParam, TypeParam>>{{5, 10}, {20, 25}, {35, 100}};
  const auto filter = std::make_shared<RangeFilter<TypeParam>>(ranges);

  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Between, 6, 8).type, EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Between, 6, 12).type, EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Between, 18, 21).type, EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Between, 18, 30).type, EstimateType::MatchesApproximately);

  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Between, 100, 0).type, EstimateType::MatchesNone);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Between, 1, 3).type, EstimateType::MatchesNone);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Between, 12, 18).type, EstimateType::MatchesNone);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Between, 110, 200).type, EstimateType::MatchesNone);

  // Bounds
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Between, 1, 4).type, EstimateType::MatchesNone);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Between, 1, 5).type, EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Between, 10, 12).type, EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Between, 11, 12).type, EstimateType::MatchesNone);
}

TYPED_TEST(RangeFilterTest, SliceWithPredicate) {
  auto new_filter = std::shared_ptr<RangeFilter<TypeParam>>{};
  const auto ranges = std::vector<std::pair<TypeParam, TypeParam>>{{5, 10}, {20, 25}, {35, 100}};

  const auto filter = std::make_shared<RangeFilter<TypeParam>>(ranges);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::LessThan, ranges.front().first).type,
            EstimateType::MatchesNone);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::LessThanEquals, ranges.front().first).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, 15).type, EstimateType::MatchesNone);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, 30).type, EstimateType::MatchesNone);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::GreaterThanEquals, ranges.back().second).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::GreaterThan, ranges.back().second).type,
            EstimateType::MatchesNone);

  new_filter =
      std::static_pointer_cast<RangeFilter<TypeParam>>(filter->slice_with_predicate(PredicateCondition::NotEquals, 7));
  // Should be the same filter.
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::LessThan, ranges.front().first).type,
            EstimateType::MatchesNone);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::LessThanEquals, ranges.front().first).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, 15).type, EstimateType::MatchesNone);
  EXPECT_EQ(filter->estimate_cardinality(PredicateCondition::Equals, 30).type, EstimateType::MatchesNone);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::GreaterThanEquals, ranges.back().second).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::GreaterThan, ranges.back().second).type,
            EstimateType::MatchesNone);

  new_filter = std::static_pointer_cast<RangeFilter<TypeParam>>(
      filter->slice_with_predicate(PredicateCondition::LessThanEquals, 7));
  // New filter should start at same value as before and end at 7.
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::LessThan, ranges.front().first).type,
            EstimateType::MatchesNone);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::LessThanEquals, ranges.front().first).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::GreaterThanEquals, 7).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::GreaterThan, 7).type, EstimateType::MatchesNone);

  new_filter = std::static_pointer_cast<RangeFilter<TypeParam>>(
      filter->slice_with_predicate(PredicateCondition::LessThanEquals, 17));
  // New filter should start at same value as before and end before first gap (because 17 is in that first gap).
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::LessThan, ranges.front().first).type,
            EstimateType::MatchesNone);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::LessThanEquals, ranges.front().first).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::GreaterThanEquals, ranges.front().second).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::GreaterThan, ranges.front().second).type,
            EstimateType::MatchesNone);

  new_filter = std::static_pointer_cast<RangeFilter<TypeParam>>(
      filter->slice_with_predicate(PredicateCondition::GreaterThanEquals, 7));
  // New filter should start at 7 and end at same value as before.
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::LessThan, 7).type, EstimateType::MatchesNone);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::LessThanEquals, 7).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::GreaterThanEquals, ranges.back().second).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::GreaterThan, ranges.back().second).type,
            EstimateType::MatchesNone);

  new_filter = std::static_pointer_cast<RangeFilter<TypeParam>>(
      filter->slice_with_predicate(PredicateCondition::GreaterThanEquals, 17));
  // New filter should start after first gap (because 17 is in that first gap) and end at same value as before.
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::LessThan, ranges[1].first).type,
            EstimateType::MatchesNone);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::LessThanEquals, ranges[1].first).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::GreaterThanEquals, ranges.back().second).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::GreaterThan, ranges.back().second).type,
            EstimateType::MatchesNone);

  new_filter = std::static_pointer_cast<RangeFilter<TypeParam>>(
      filter->slice_with_predicate(PredicateCondition::Between, 7, 17));
  // New filter should start at 7 and end right before first gap (because 17 is in that gap).
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::LessThan, 7).type, EstimateType::MatchesNone);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::LessThanEquals, 7).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::GreaterThanEquals, ranges.front().second).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::GreaterThan, ranges.front().second).type,
            EstimateType::MatchesNone);

  new_filter = std::static_pointer_cast<RangeFilter<TypeParam>>(
      filter->slice_with_predicate(PredicateCondition::Between, 17, 27));
  // New filter should start right after first gap (because 17 is in that gap)
  // and end right before second gap (because 27 is in that gap).
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::LessThan, ranges[1].first).type,
            EstimateType::MatchesNone);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::LessThanEquals, ranges[1].first).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::GreaterThanEquals, ranges[1].second).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(new_filter->estimate_cardinality(PredicateCondition::GreaterThan, ranges[1].second).type,
            EstimateType::MatchesNone);

  // Slice with equality predicate will return MinMaxFilter.
  const auto min_max_filter =
      std::static_pointer_cast<MinMaxFilter<TypeParam>>(filter->slice_with_predicate(PredicateCondition::Equals, 7));
  // New filter should have 7 as min and max.
  EXPECT_EQ(min_max_filter->estimate_cardinality(PredicateCondition::LessThan, 7).type, EstimateType::MatchesNone);
  EXPECT_EQ(min_max_filter->estimate_cardinality(PredicateCondition::LessThanEquals, 7).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(min_max_filter->estimate_cardinality(PredicateCondition::GreaterThanEquals, 7).type,
            EstimateType::MatchesApproximately);
  EXPECT_EQ(min_max_filter->estimate_cardinality(PredicateCondition::GreaterThan, 7).type, EstimateType::MatchesNone);
}

TYPED_TEST(RangeFilterTest, SliceWithPredicateEmptyStatistics) {
  const auto filter = RangeFilter<TypeParam>::build_filter(this->_values, 5);

  EXPECT_TRUE(std::dynamic_pointer_cast<EmptyStatisticsObject>(
      filter->slice_with_predicate(PredicateCondition::LessThan, this->_min_value)));
  EXPECT_FALSE(std::dynamic_pointer_cast<EmptyStatisticsObject>(
      filter->slice_with_predicate(PredicateCondition::LessThanEquals, this->_min_value)));
  EXPECT_FALSE(std::dynamic_pointer_cast<EmptyStatisticsObject>(
      filter->slice_with_predicate(PredicateCondition::GreaterThanEquals, this->_max_value)));
  EXPECT_TRUE(std::dynamic_pointer_cast<EmptyStatisticsObject>(
      filter->slice_with_predicate(PredicateCondition::GreaterThan, this->_max_value)));
}

}  // namespace opossum