#include "sql.hpp"

namespace opossum {

SQL::SQL(const std::string& sql) : _sql(sql) {}

SQL& SQL::set_use_mvcc(const UseMvcc use_mvcc) {
  _use_mvcc = use_mvcc;
  return *this;
}

SQL& SQL::set_optimizer(const std::shared_ptr<Optimizer>& optimizer) {
  _optimizer = optimizer;
  return *this;
}

SQL& SQL::set_prepared_statement_cache(const PreparedStatementCache& prepared_statements) {
  _prepared_statements = prepared_statements;
  return *this;
}

SQL& SQL::set_transaction_context(const std::shared_ptr<TransactionContext>& transaction_context) {
  _transaction_context = transaction_context;
  _use_mvcc = UseMvcc::Yes;

  return *this;
}

SQL& SQL::set_lqp_translator(const std::shared_ptr<LQPTranslator>& lqp_translator) {
  _lqp_translator = lqp_translator;
  return *this;
}

SQL& SQL::disable_mvcc() { return set_use_mvcc(UseMvcc::No); }

SQLPipeline SQL::pipeline() const {
  auto optimizer = _optimizer ? _optimizer : Optimizer::create_default_optimizer();

  return {_sql, _transaction_context, _use_mvcc, optimizer, _prepared_statements, _lqp_translator};
}

SQLPipelineStatement SQL::pipeline_statement(std::shared_ptr<hsql::SQLParserResult> parsed_sql) const {
  auto optimizer = _optimizer ? _optimizer : Optimizer::create_default_optimizer();

  return {_sql, parsed_sql, _use_mvcc, _transaction_context, optimizer, _prepared_statements, _lqp_translator};
}

}  // namespace opossum