#include "cardinality_estimation_cache.hpp"

#include <iostream>

#include "optimizer/join_ordering/join_plan_predicate.hpp"

namespace opossum {

std::optional<Cardinality> CardinalityEstimationCache::get(const BaseJoinGraph& join_graph) {
  auto normalized_join_graph = _normalize(join_graph);
  
  const auto cache_iter = _cache.find(normalized_join_graph);
  if (cache_iter != _cache.end()) {
//    std::cout << "CardinalityEstimationCache [HIT ]: " << normalized_join_graph.description() << ": " << cache_iter->second << std::endl;
    ++_hit_count;
    return cache_iter->second;
  } else {
//    std::cout << "CardinalityEstimationCache [MISS]: " << normalized_join_graph.description() << std::endl;
    ++_miss_count;
    return std::nullopt;
  }
}

void CardinalityEstimationCache::put(const BaseJoinGraph& join_graph, const Cardinality cardinality) {
  auto normalized_join_graph = _normalize(join_graph);
  
//  if (_cache.count(normalized_join_graph) == 0) {
//    std::cout << "CardinalityEstimationCache [PUT ]: " << normalized_join_graph.description() << ": " << cardinality << std::endl;
//  }
  _cache[normalized_join_graph] = cardinality;
}

size_t CardinalityEstimationCache::cache_hit_count() const {
  return _hit_count;
}

size_t CardinalityEstimationCache::cache_miss_count() const {
  return _miss_count;
}

size_t CardinalityEstimationCache::size() const {
  return _cache.size();
}

void CardinalityEstimationCache::clear() {
  _cache.clear();
  _hit_count = 0;
  _miss_count = 0;
}

BaseJoinGraph CardinalityEstimationCache::_normalize(const BaseJoinGraph& join_graph) {
  auto normalized_join_graph = join_graph;

  for (auto& predicate : normalized_join_graph.predicates) {
    predicate = _normalize(predicate);
  }

  return normalized_join_graph;
}

std::shared_ptr<const AbstractJoinPlanPredicate> CardinalityEstimationCache::_normalize(const std::shared_ptr<const AbstractJoinPlanPredicate>& predicate) {
  if (predicate->type() == JoinPlanPredicateType::Atomic) {
    const auto atomic_predicate = std::static_pointer_cast<const JoinPlanAtomicPredicate>(predicate);
    if (is_lqp_column_reference(atomic_predicate->right_operand)) {
      const auto right_operand_column_reference = boost::get<LQPColumnReference>(atomic_predicate->right_operand);

      if (std::hash<LQPColumnReference>{}(right_operand_column_reference) < std::hash<LQPColumnReference>{}(atomic_predicate->left_operand)) {
        if (atomic_predicate->predicate_condition != PredicateCondition::Like) {
          const auto flipped_predicate_condition = flip_predicate_condition(atomic_predicate->predicate_condition);
          return std::make_shared<JoinPlanAtomicPredicate>(right_operand_column_reference, flipped_predicate_condition, atomic_predicate->left_operand);
        }
      }
    }
  } else {
    const auto logical_predicate = std::static_pointer_cast<const JoinPlanLogicalPredicate>(predicate);

    auto normalized_left = _normalize(logical_predicate->left_operand);
    auto normalized_right = _normalize(logical_predicate->right_operand);

    if (normalized_right->hash() < normalized_left->hash()) {
      std::swap(normalized_left, normalized_right);
    }

    return std::make_shared<JoinPlanLogicalPredicate>(normalized_left,
                                                      logical_predicate->logical_operator,
                                                      normalized_right);

  }

  return predicate;
}

}  // namespace opossum