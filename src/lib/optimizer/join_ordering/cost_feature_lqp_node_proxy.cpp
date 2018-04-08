#include "cost_feature_lqp_node_proxy.hpp"

#include <cmath>

#include "logical_query_plan/abstract_lqp_node.hpp"
#include "logical_query_plan/join_node.hpp"
#include "logical_query_plan/predicate_node.hpp"
#include "logical_query_plan/sort_node.hpp"
#include "optimizer/table_statistics.hpp"
#include "resolve_type.hpp"

namespace opossum {

CostFeatureLQPNodeProxy::CostFeatureLQPNodeProxy(const std::shared_ptr<AbstractLQPNode>& node):
  _node(node) {

}

CostFeatureVariant CostFeatureLQPNodeProxy::_extract_feature_impl(const CostFeature cost_feature) const  {
  switch (cost_feature) {
    case CostFeature::LeftInputRowCount: return _node->left_input()->get_statistics()->row_count();
    case CostFeature::RightInputRowCount: return _node->right_input()->get_statistics()->row_count();
    case CostFeature::LeftInputIsReferences: return _node->left_input()->get_statistics()->table_type() == TableType::References;
    case CostFeature::RightInputIsReferences: return _node->right_input()->get_statistics()->table_type() == TableType::References;
    case CostFeature::OutputRowCount: return _node->get_statistics()->row_count();

    case CostFeature::LeftDataType:
    case CostFeature::RightDataType: {
      auto column_reference = LQPColumnReference{};

      if (_node->type() == LQPNodeType::Join) {
        const auto join_node = std::static_pointer_cast<JoinNode>(_node);
        column_reference = cost_feature == CostFeature::LeftDataType ? join_node->join_column_references().first :
                           join_node->join_column_references().second;
      } else if (_node->type() == LQPNodeType::Predicate) {
        const auto predicate_node = std::static_pointer_cast<PredicateNode>(_node);
        if (cost_feature == CostFeature::LeftDataType) {
          column_reference = predicate_node->column_reference();
        } else {
          if (predicate_node->value().type() == typeid(AllTypeVariant)) {
            return data_type_from_all_type_variant(boost::get<AllTypeVariant>(predicate_node->value()));
          } else {
            Assert(predicate_node->value().type() == typeid(LQPColumnReference), "Expected LQPColumnReference");
            column_reference = boost::get<LQPColumnReference>(predicate_node->value());
          }
        }

        auto column_id = _node->get_output_column_id(column_reference);
        return _node->get_statistics()->column_statistics().at(column_id)->data_type();
      } else {
        Fail("CostFeature not defined for LQPNodeType");
      }
    }

    case CostFeature::PredicateCondition:
      if (_node->type() == LQPNodeType::Join) {
        return std::static_pointer_cast<JoinNode>(_node)->predicate_condition();
      } else if (_node->type() == LQPNodeType::Predicate) {
        return std::static_pointer_cast<PredicateNode>(_node)->predicate_condition();
      } else {
        Fail("CostFeature not defined for LQPNodeType");
      }

    case CostFeature::RightOperandIsColumn:
      if (_node->type() == LQPNodeType::Predicate) {
        return is_lqp_column_reference(std::static_pointer_cast<PredicateNode>(_node)->value());
      } else {
        Fail("CostFeature not defined for LQPNodeType");
      }

    default:
      Fail("Extraction of this feature is not implemented. Maybe it should be handled in AbstractCostFeatureProxy?");
  }
}

}  // namespace opossum
<