/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX SQL_RESV
#include "share/ob_define.h"
#include "lib/string/ob_sql_string.h"
#include "lib/utility/ob_tracepoint.h"
#include "share/ob_autoincrement_param.h"
#include "share/schema/ob_schema_mgr.h"
#include "sql/resolver/ddl/ob_ddl_resolver.h"
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/expr/ob_raw_expr.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/resolver/expr/ob_raw_expr_canonicalizer_impl.h"
#include "sql/resolver/expr/ob_raw_expr_resolver_impl.h"
#include "sql/resolver/expr/ob_raw_expr_info_extractor.h"
#include "sql/resolver/expr/ob_raw_expr_info_extractor.h"
#include "sql/resolver/dml/ob_view_table_resolver.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_update_stmt.h"
#include "sql/ob_sql_context.h"
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_node.h"
#include "sql/parser/parse_malloc.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/schema/ob_table_schema.h"
#include "sql/resolver/expr/ob_raw_expr_canonicalizer_impl.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/optimizer/ob_optimizer_util.h"
#include "sql/resolver/dml/ob_default_value_utils.h"
#include "common/sql_mode/ob_sql_mode_utils.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "share/schema/ob_part_mgr_util.h"
#include "share/schema/ob_routine_info.h"
#include "share/ob_get_compat_mode.h"
#include "sql/ob_sql_utils.h"
#include "lib/oblog/ob_trace_log.h"
#include "pl/ob_pl_package.h"
#include "pl/ob_pl_resolver.h"
#include "pl/ob_pl_stmt.h"
#include "sql/optimizer/ob_opt_est_utils.h"
#include "objit/expr/ob_iraw_expr.h"
#include "sql/rewrite/ob_transform_utils.h"
#include "sql/resolver/expr/ob_raw_expr_printer.h"
#include "sql/parser/ob_item_type_str.h"
#include "sql/ob_select_stmt_printer.h"
#include "lib/utility/ob_fast_convert.h"
#include "sql/engine/expr/ob_expr_autoinc_nextval.h"
#include "sql/engine/expr/ob_expr_column_conv.h"
#include "sql/engine/expr/ob_expr_version.h"
#include "common/ob_smart_call.h"
#include "share/resource_manager/ob_resource_manager.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
using namespace pl;

namespace sql
{
ObDMLResolver::ObDMLResolver(ObResolverParams &params)
    : ObStmtResolver(params),
      current_scope_(T_NONE_SCOPE),
      current_level_(0),
      field_list_first_(false),
      parent_namespace_resolver_(NULL),
      column_namespace_checker_(params),
      sequence_namespace_checker_(params),
      gen_col_exprs_(),
      from_items_order_(),
      query_ref_(NULL),
      has_ansi_join_(false),
      has_oracle_join_(false),
      with_clause_without_record_(false),
      is_prepare_stage_(params.is_prepare_stage_),
      in_pl_(params.secondary_namespace_ || params.is_dynamic_sql_ || params.is_dbms_sql_),
      resolve_alias_for_subquery_(true),
      current_view_level_(0),
      view_ref_id_(OB_INVALID_ID),
      is_resolving_view_(false),
      join_infos_(),
      parent_cte_tables_(),
      current_cte_tables_()
{
  column_namespace_checker_.set_joininfos(&join_infos_);
}

ObDMLResolver::~ObDMLResolver()
{
}

int ResolverJoinInfo::assign(const ResolverJoinInfo &other)
{
  int ret = OB_SUCCESS;
  if (this == &other) {
    //skip
  } else if (OB_FAIL(using_columns_.assign(other.using_columns_))) {
    LOG_WARN("fail to assign SEArray using_columns_", K(ret));
  } else if (OB_FAIL(coalesce_expr_.assign(other.coalesce_expr_))) {
    LOG_WARN("fail to assign SEArray coalesce_expr_", K(ret));
  } else {
    table_id_ = other.table_id_;
  }
  return ret;
}

ObDMLStmt *ObDMLResolver::get_stmt()
{
  return static_cast<ObDMLStmt*>(stmt_);
}

// use_sys_tenant 标记是否需要以系统租户的身份获取schema

int ObDMLResolver::check_need_use_sys_tenant(bool &use_sys_tenant) const
{
  use_sys_tenant = false;
  return OB_SUCCESS;
}

int ObDMLResolver::check_in_sysview(bool &in_sysview) const
{
  in_sysview = false;
  return OB_SUCCESS;
}

int ObDMLResolver::alloc_joined_table_item(JoinedTable *&joined_table)
{
  int ret = OB_SUCCESS;
  void *ptr = NULL;
  if (OB_ISNULL(allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid argument", K(ret));
  } else if (OB_ISNULL(ptr = allocator_->alloc(sizeof(JoinedTable)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    SQL_RESV_LOG(ERROR, "alloc memory for JoinedTable failed", "size", sizeof(JoinedTable));
  } else {
    joined_table = new (ptr) JoinedTable();
  }
  return ret;
}

int ObDMLResolver::create_joined_table_item(
    const ObJoinType joined_type,
    const TableItem *left_table,
    const TableItem *right_table,
    JoinedTable* &joined_table)
{
  int ret = OB_SUCCESS;
  OZ(alloc_joined_table_item(joined_table));
  CK(OB_NOT_NULL(joined_table));
  if (OB_SUCC(ret)) {
    // 如果 dependency 是空的, 那么使用 inner join
    joined_table->table_id_ = generate_table_id();
    joined_table->type_ = TableItem::JOINED_TABLE;
    joined_table->joined_type_ = joined_type;
    joined_table->left_table_ = const_cast<TableItem*>(left_table);
    joined_table->right_table_ = const_cast<TableItem*>(right_table);

    // push up single table ids (left deep tree)
    // left table ids
    CK(OB_NOT_NULL(joined_table->left_table_));
    if (OB_FAIL(ret)) {
      // do nothing
    } else if (joined_table->left_table_->is_joined_table()) {
      JoinedTable *cur_joined = static_cast<JoinedTable*>(joined_table->left_table_);
      for (int64_t i = 0; OB_SUCC(ret) && i < cur_joined->single_table_ids_.count(); i++) {
        OZ((joined_table->single_table_ids_.push_back)(
                cur_joined->single_table_ids_.at(i)));
      }
    } else {
      OZ((joined_table->single_table_ids_.push_back)(
              joined_table->left_table_->table_id_));
    }

    // right table id
    CK(OB_NOT_NULL(joined_table->right_table_));
    if (OB_FAIL(ret)) {
      // do nothing
    } else if (joined_table->right_table_->is_joined_table()) {
      JoinedTable *cur_joined = static_cast<JoinedTable*>(joined_table->right_table_);
      for (int64_t i = 0; OB_SUCC(ret) && i < cur_joined->single_table_ids_.count(); i++) {
        OZ((joined_table->single_table_ids_.push_back)(
                cur_joined->single_table_ids_.at(i)));
      }
    } else {
      OZ((joined_table->single_table_ids_.push_back)(
              joined_table->right_table_->table_id_));
    }
  }
  return ret;
}

int ObDMLResolver::resolve_sql_expr(const ParseNode &node, ObRawExpr *&expr,
    ObArray<ObQualifiedName> *output_columns/* = NULL*/)
{
  int ret = OB_SUCCESS;
  bool tmp_field_list_first = field_list_first_;
  field_list_first_ = false;
  ObArray<ObQualifiedName> columns;
  bool need_analyze_aggr = false;
  if (output_columns == NULL) {
    output_columns = &columns;
    need_analyze_aggr = true;
  }
  ObArray<ObSubQueryInfo> sub_query_info;
  ObArray<ObVarInfo> sys_vars;
  ObArray<ObAggFunRawExpr*> aggr_exprs;
  ObArray<ObWinFunRawExpr*> win_exprs;
  ObArray<ObUDFInfo> udf_info;
  ObArray<ObOpRawExpr*> op_exprs;
  ObCollationType collation_connection = CS_TYPE_INVALID;
  ObCharsetType character_set_connection = CHARSET_INVALID;
  CK( OB_NOT_NULL(params_.expr_factory_),
      OB_NOT_NULL(stmt_),
      OB_NOT_NULL(get_stmt()),
      OB_NOT_NULL(session_info_));
  OC( (params_.session_info_->get_collation_connection)(collation_connection) );
  OC( (params_.session_info_->get_character_set_connection)(character_set_connection) );

  if (OB_SUCC(ret)) {
    ObExprResolveContext ctx(*params_.expr_factory_,
                             session_info_->get_timezone_info(),
                             OB_NAME_CASE_INVALID);
    ctx.dest_collation_ = collation_connection;
    ctx.connection_charset_ = character_set_connection;
    ctx.param_list_ = params_.param_list_;
    ctx.is_extract_param_type_ = !params_.is_prepare_protocol_; //when prepare do not extract
    ctx.external_param_info_ = &params_.external_param_info_;
    ctx.current_scope_ = current_scope_;
    ctx.stmt_ = static_cast<ObStmt*>(get_stmt());
    ctx.schema_checker_ = schema_checker_;
    ctx.session_info_ = session_info_;
    ctx.secondary_namespace_ = params_.secondary_namespace_;
    ctx.prepare_param_count_ = params_.prepare_param_count_;
    ctx.query_ctx_ = params_.query_ctx_;
    ctx.is_for_pivot_ = !need_analyze_aggr;
    ctx.is_for_dynamic_sql_ = params_.is_dynamic_sql_;
    ctx.is_for_dbms_sql_ = params_.is_dbms_sql_;
    ObRawExprResolverImpl expr_resolver(ctx);
    ObIArray<ObUserVarIdentRawExpr *> &user_var_exprs = get_stmt()->get_user_vars();
    OC( (session_info_->get_name_case_mode)(ctx.case_mode_));
    OC( (expr_resolver.resolve)(&node,
                                expr,
                                *output_columns,
                                sys_vars,
                                sub_query_info,
                                aggr_exprs,
                                win_exprs,
                                udf_info,
                                op_exprs,
                                user_var_exprs));
    if (OB_SUCC(ret)) {
      params_.prepare_param_count_ = ctx.prepare_param_count_; //prepare param count
    }
    OC( (resolve_subquery_info)(sub_query_info));
    if (OB_SUCC(ret)) {
      //are there any user variable assignments?
      get_stmt()->set_contains_assignment(expr_resolver.is_contains_assignment());
    }
    if (OB_SUCC(ret)) {
      if (OB_NOT_NULL(params_.query_ctx_)) {
        params_.query_ctx_->set_has_nested_sql(!udf_info.empty());
      }
    }

    if (OB_SUCC(ret)) {
      if (expr->is_calc_part_expr()) {
        if (OB_FAIL(reset_calc_part_id_param_exprs(expr, *output_columns))) {
          LOG_WARN("failed to reset calc part id param exprs", K(ret));
        } else {/*do nothing*/}
      } else if (expr->get_expr_type() == T_OP_EQ &&
                 expr->get_param_count() == 2 &&
                 expr->get_param_expr(0) != NULL &&
                 expr->get_param_expr(0)->is_calc_part_expr()) {
        if (OB_FAIL(reset_calc_part_id_param_exprs(expr->get_param_expr(0), *output_columns))) {
          LOG_WARN("failed to reset calc part id param exprs", K(ret));
        } else {/*do nothing*/}
      }
    }

    // resolve column(s)
    if (OB_SUCC(ret) && output_columns->count() > 0) {
      if (tmp_field_list_first && stmt_->is_select_stmt()) {
        ObSelectStmt *sel_stmt = static_cast<ObSelectStmt *>(stmt_);
        if (OB_FAIL(resolve_columns_field_list_first(expr, *output_columns, sel_stmt))) {
          LOG_WARN("resolve columns field list first failed", K(ret));
        }
      } else if (OB_FAIL(resolve_columns(expr, *output_columns))) {
        LOG_WARN("resolve columns failed", K(ret));
      }
    }

    if (OB_SUCC(ret) && udf_info.count() > 0) {
      stmt_->get_query_ctx()->has_pl_udf_ = true;
      for (int64_t i = 0; OB_SUCC(ret) && i < udf_info.count(); ++i) {
        ObUDFInfo &udf = udf_info.at(i);
        if(OB_FAIL(ObRawExprUtils::init_udf_info(params_, udf))) {
          LOG_WARN("resolve user defined functions failed", K(ret));
        } else {
          ObDMLStmt *stmt = get_stmt();
          ObUDFRawExpr *udf_expr = static_cast<ObUDFRawExpr*>(udf_info.at(i).ref_expr_);
          if (OB_ISNULL(stmt) || OB_ISNULL(udf_expr)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("stmt or expr is null", K(stmt), K(udf_expr), K(ret));
          } else if (udf_expr->need_add_dependency()) {
            ObSchemaObjVersion udf_version;
            OZ (udf_expr->get_schema_object_version(udf_version));
            OZ (stmt->add_global_dependency_table(udf_version));
          }
        }
      }
    }
    //try to convert ObUDFRawExpr to ObAggRawExpr for pl agg udf
    if (OB_SUCC(ret)) {
      if (OB_FAIL(convert_udf_to_agg_expr(expr, NULL, ctx))) {
        LOG_WARN("failed to convert udf to agg expr", K(ret));
      }
    }
    if (OB_SUCC(ret) && aggr_exprs.count() > 0) {
      if (OB_FAIL(resolve_aggr_exprs(expr, aggr_exprs, need_analyze_aggr))) {
        LOG_WARN("resolve aggr exprs failed", K(ret));
      }
    }
    // resolve sys var(s)
    if (OB_SUCC(ret) && sys_vars.count() > 0) {
      if (OB_FAIL(resolve_sys_vars(sys_vars))) {
        LOG_WARN("resolve system variables failed", K(ret));
      }
    }

    if (OB_SUCC(ret) && win_exprs.count() > 0) {
      if (OB_FAIL(resolve_win_func_exprs(expr, win_exprs))) {
        LOG_WARN("resolve aggr exprs failed", K(ret));
      }
    }

    //process oracle compatible implimental cast
    LOG_DEBUG("is oracle mode", K(lib::is_oracle_mode()), K(lib::is_oracle_mode()), K(op_exprs));
    if (OB_SUCC(ret) && op_exprs.count() > 0) {
      if (OB_FAIL(expr->extract_info())) {
        LOG_WARN("failed to extract info", K(ret), K(*expr));
      } else if (OB_FAIL(ObRawExprUtils::resolve_op_exprs_for_oracle_implicit_cast(
                                                                                  ctx.expr_factory_,
                                                                                  ctx.session_info_,
                                                                                  op_exprs))) {
        LOG_WARN("implicit cast faild", K(ret));
      }
    }
    // resolve special expression, like functions, e.g abs, concat
    // acutally not so special, hmm...
    if (OB_SUCC(ret)) {
      // update flag info
      if (OB_FAIL(expr->extract_info())) {
        LOG_WARN("failed to extract info", K(ret), K(*expr));
      } else if (OB_FAIL(resolve_outer_join_symbol(current_scope_, expr))) {
        LOG_WARN("Failed to check and remove outer join symbol", K(ret));
      } else if (OB_FAIL(resolve_special_expr(expr, current_scope_))) {
        LOG_WARN("resolve special expression failed", K(ret));
      }
    }
    //LOG_DEBUG("resolve_sql_expr:5", "usec", ObSQLUtils::get_usec());
    // refresh info again
    if (OB_SUCC(ret)) {
      if (OB_FAIL(expr->extract_info())) {
        LOG_WARN("failed to extract info", K(ret));
      } else if (OB_FAIL(check_expr_param(*expr))) {
        //一个表达式的根表达式不能是一个向量表达式或者向量结果的子查询表达式
        LOG_WARN("check expr param failed", K(ret));
      } else if (OB_FAIL(ObRawExprUtils::check_composite_cast(expr, *schema_checker_))) {
        LOG_WARN("check composite cast failed", K(ret));
      }
    }
  }

  return ret;
}

int ObDMLResolver::reset_calc_part_id_param_exprs(ObRawExpr *&expr,
                                                  ObIArray<ObQualifiedName> &columns)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(get_stmt()) || OB_ISNULL(expr) || OB_ISNULL(params_.schema_checker_) ||
      OB_ISNULL(params_.expr_factory_) || OB_ISNULL(params_.session_info_) ||
      OB_UNLIKELY(!expr->is_calc_part_expr() ||
                  (expr->get_param_count() != 2 && expr->get_param_count() != 3)||
                  columns.count() != expr->get_param_count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get unexpected error", K(ret), K(expr), K(params_.schema_checker_),
                                     K(params_.expr_factory_), K(params_.session_info_));
  } else {
    ObString table_name;
    ObString index_name;
    ObString opt_str;
    if (columns.count() == 2) {
      table_name = columns.at(0).col_name_;
      opt_str = columns.at(1).col_name_;
    } else {
      table_name = columns.at(0).col_name_;
      index_name = columns.at(1).col_name_;
      opt_str = columns.at(2).col_name_;
    }
    int64_t tbl_id = -1;
    int64_t ref_id = -1;
    bool find_it = false;
    ObString subpart_str(7, "SUBPART");
    ObString part_str(4, "PART");
    PartitionIdCalcType calc_type = CALC_INVALID;
    if (0 == subpart_str.case_compare(opt_str)) {
      calc_type = CALC_NORMAL;
    } else if (0 == part_str.case_compare(opt_str)) {
      calc_type = CALC_IGNORE_SUB_PART;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("get invalid opt string", K(ret), K(opt_str));
    }
    for(int64_t i = 0; OB_SUCC(ret) && !find_it && i < get_stmt()->get_table_items().count(); ++i) {
      TableItem *table_item = NULL;
      if (OB_ISNULL(table_item = get_stmt()->get_table_items().at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret), K(table_item));
      } else if (0 == table_name.case_compare(table_item->table_name_)) {
        if (!index_name.empty()) {
          if (OB_FAIL(find_table_index_infos(index_name, table_item, find_it, tbl_id, ref_id))) {
            LOG_WARN("failed to find table index infos", K(ret));
          } else {/*do nothing*/}
        } else {
          find_it = true;
          tbl_id = table_item->table_id_;
          ref_id = table_item->ref_id_;
        }
      } else {/*do nothing*/}
    }
    CK (OB_NOT_NULL(session_info_));
    if (OB_SUCC(ret)) {
      share::schema::ObSchemaGetterGuard *schema_guard = NULL;
      const share::schema::ObTableSchema *table_schema = NULL;
      if (!find_it) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("get invalid table name", K(ret), K(table_name));
      } else if (OB_ISNULL(schema_guard = params_.schema_checker_->get_schema_guard())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("table schema is null", K(ret), K(schema_guard));
      } else if (OB_FAIL(schema_guard->get_table_schema(
                 session_info_->get_effective_tenant_id(), ref_id, table_schema))) {
        LOG_WARN("get table schema failed", K(ref_id), K(ret));
      } else if (OB_ISNULL(table_schema)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("table schema is null", K(ret), K(table_schema));
      } else {
        schema::ObPartitionLevel part_level = table_schema->get_part_level();
        ObRawExpr *part_expr = get_stmt()->get_part_expr(tbl_id, ref_id);
        ObRawExpr *subpart_expr = get_stmt()->get_subpart_expr(tbl_id, ref_id);
        ObRawExpr *new_part_expr = NULL;
        ObRawExpr *new_subpart_expr = NULL;
        if (OB_FAIL(ObRawExprUtils::copy_expr(*params_.expr_factory_,
                                              part_expr,
                                              new_part_expr,
                                              COPY_REF_DEFAULT))) {
          LOG_WARN("fail to copy part expr", K(ret));
        } else if (OB_FAIL(ObRawExprUtils::copy_expr(*params_.expr_factory_,
                                                     subpart_expr,
                                                     new_subpart_expr,
                                                     COPY_REF_DEFAULT))) {
          LOG_WARN("fail to copy subpart expr", K(ret));
        } else if (OB_FAIL(ObRawExprUtils::build_calc_part_id_expr(*params_.expr_factory_,
                                                                   *params_.session_info_,
                                                                   ref_id,
                                                                   part_level,
                                                                   new_part_expr,
                                                                   new_subpart_expr,
                                                                   expr))) {
          LOG_WARN("fail to build table location expr", K(ret));
        } else {
          expr->set_partition_id_calc_type(calc_type);
          columns.reset();
          LOG_TRACE("Succeed to reset calc part id param exprs", K(*expr));
        }
      }
    }
  }
  return ret;
}

//resolve order by items时，先在select items中查找。
////create table t1(c1 int,c2 int);
////create table t2(c1 int, c2 int);
////select a.c1, b.c2 from t1 a, t2 b order by (c1+c2);是合法的，order by后的c1和c2分别对应select items中的a.c1和b.c2
int ObDMLResolver::resolve_columns_field_list_first(ObRawExpr *&expr, ObArray<ObQualifiedName> &columns, ObSelectStmt* sel_stmt)
{
  int ret = OB_SUCCESS;
  ObArray<ObRawExpr*> real_exprs;
  if (OB_ISNULL(sel_stmt) || OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("expr or select stmt is null", K(expr), K(sel_stmt));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); i++) {
    bool found = false;
    if (columns.at(i).tbl_name_.empty()) {
      for (int64_t j = 0; OB_SUCC(ret) && j < sel_stmt->get_select_item_size(); j++) {
        ObRawExpr *select_item_expr = NULL;
        if (OB_ISNULL(select_item_expr = sel_stmt->get_select_item(j).expr_)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("select item expr is null", K(ret));
        } else if (select_item_expr->is_column_ref_expr()) {
          ObColumnRefRawExpr *column_ref_expr = static_cast<ObColumnRefRawExpr *>(select_item_expr);
          if (ObCharset::case_insensitive_equal(sel_stmt->get_select_item(j).is_real_alias_ ? sel_stmt->get_select_item(j).alias_name_
                                              : column_ref_expr->get_column_name(), columns.at(i).col_name_)) {
            if (found) {
              ObString scope_name = ObString::make_string(get_scope_name(current_scope_));
              ret = OB_NON_UNIQ_ERROR;
              LOG_USER_ERROR(OB_NON_UNIQ_ERROR,
                             columns.at(i).col_name_.length(),
                             columns.at(i).col_name_.ptr(),
                             scope_name.length(),
                             scope_name.ptr());
            } else {
              found = true;
              if (OB_FAIL(real_exprs.push_back(column_ref_expr))) {
                LOG_WARN("push back failed", K(ret));
              } else if (OB_FAIL(ObRawExprUtils::replace_ref_column(expr, columns.at(i).ref_expr_, column_ref_expr))) {
                LOG_WARN("replace column ref expr failed", K(ret));
              } else { /* do nothing */ }
            }
          }
        } else if (is_oracle_mode()) {
          const SelectItem &select_item = sel_stmt->get_select_item(j);
          if (ObCharset::case_insensitive_equal(select_item.is_real_alias_ ? select_item.alias_name_
                                              : select_item.expr_name_, columns.at(i).col_name_)) {
            if (found) {
              ObString scope_name = ObString::make_string(get_scope_name(T_FIELD_LIST_SCOPE));
              ret = OB_NON_UNIQ_ERROR;
              LOG_USER_ERROR(OB_NON_UNIQ_ERROR,
                             columns.at(i).col_name_.length(),
                             columns.at(i).col_name_.ptr(),
                             scope_name.length(),
                             scope_name.ptr());
            } else {
              found = true;
              if (OB_FAIL(real_exprs.push_back(select_item_expr))) {
                LOG_WARN("push back failed", K(ret));
              } else if (OB_FAIL(ObRawExprUtils::replace_ref_column(expr, columns.at(i).ref_expr_, select_item_expr))) {
                LOG_WARN("replace column ref expr failed", K(ret));
              } else { /* do nothing */ }
            }
          }
        }
      }
    }
    if (OB_SUCC(ret) && false == found) {
      ObQualifiedName &q_name = columns.at(i);
      ObRawExpr *real_ref_expr = NULL;
      if (OB_FAIL(resolve_qualified_identifier(q_name, columns, real_exprs, real_ref_expr))) {
        LOG_WARN_IGNORE_COL_NOTFOUND(ret, "resolve column ref expr failed", K(ret), K(q_name));
        report_user_error_msg(ret, expr, q_name);
      } else if (OB_FAIL(real_exprs.push_back(real_ref_expr))) {
        LOG_WARN("push back failed", K(ret));
      } else if (OB_FAIL(ObRawExprUtils::replace_ref_column(expr, q_name.ref_expr_, real_ref_expr))) {
        LOG_WARN("replace column ref expr failed", K(ret));
      } else { /*do nothing*/ }
    }
  }

  return ret;
}

int ObDMLResolver::resolve_into_variables(const ParseNode *node,
                                          ObIArray<ObString> &user_vars,
                                          ObIArray<ObRawExpr*> &pl_vars,
                                          ObSelectStmt *select_stmt)
{
  int ret = OB_SUCCESS;

  CK(OB_NOT_NULL(node),
     OB_LIKELY(T_INTO_VARIABLES == node->type_),
     OB_NOT_NULL(node->children_[0]),
     OB_NOT_NULL(get_stmt()));

  if (OB_SUCC(ret)) {
    current_scope_ = T_INTO_SCOPE;
    const ParseNode *into_node = node->children_[0];
    ObRawExpr *expr = NULL;
    ParseNode *ch_node = NULL;
    ObBitSet<> user_var_idx;
    for (int64_t i = 0; OB_SUCC(ret) && i < into_node->num_child_; ++i) {
      ch_node = into_node->children_[i];
      expr = NULL;
      CK (OB_NOT_NULL(ch_node));
      CK (OB_LIKELY(T_USER_VARIABLE_IDENTIFIER == ch_node->type_ /*MySQL Mode for user_var*/
                    || T_IDENT == ch_node->type_ /*MySQL Mode for pl_var*/
                    || T_OBJ_ACCESS_REF == ch_node->type_ /*Oracle Mode for pl_var*/
                    || T_QUESTIONMARK == ch_node->type_));/*Oracle Mode for dynamic sql*/
      if (OB_SUCC(ret)) {
        if (T_USER_VARIABLE_IDENTIFIER == ch_node->type_) {
          ObString var_name(ch_node->str_len_, ch_node->str_value_);
          ObCharset::casedn(CS_TYPE_UTF8MB4_GENERAL_CI, var_name);
          OZ (user_vars.push_back(var_name));
          OZ (user_var_idx.add_member(i));
        } else {
          if (OB_NOT_NULL(params_.secondary_namespace_)) { //PL语句的Prepare阶段
            CK(OB_NOT_NULL(params_.allocator_), OB_NOT_NULL(params_.expr_factory_));

            OZ (pl::ObPLResolver::resolve_raw_expr(*ch_node,
                                                   *params_.allocator_,
                                                   *params_.expr_factory_,
                                                   *params_.secondary_namespace_,
                                                   params_.is_prepare_protocol_,
                                                   expr,
                                                   true));
            if (OB_ERR_VARIABLE_IS_READONLY == ret
                && lib::is_oracle_mode()
                && T_OBJ_ACCESS_REF == ch_node->type_
                && NULL != ch_node->children_[0]) {
              ret = OB_ERR_EXP_NOT_INTO_TARGET;
              LOG_WARN("string cannot be used as an INTO-target of a SELECT/FETCH stmt", K(ret), K(i));
              LOG_USER_ERROR(OB_ERR_EXP_NOT_INTO_TARGET,
                            (int)(ch_node->str_len_),
                            ch_node->str_value_);
            }
            OZ (pl_vars.push_back(expr));
          } else if (params_.is_prepare_protocol_) { //动态SQL中的RETURNING子句, 后面跟的是QuestionMark
            ObSEArray<ObQualifiedName, 1> columns;
            ObSEArray<ObVarInfo, 1> var_infos;
            OZ (ObResolverUtils::resolve_const_expr(params_,
                                                   *ch_node,
                                                    expr,
                                                    &var_infos));
            CK (0 == var_infos.count());
            if (OB_SUCC(ret) && expr->get_expr_type() != T_QUESTIONMARK) {
              ret = OB_NOT_SUPPORTED;
              LOG_WARN("dynamic sql into variable not a question mark", K(ret), KPC(expr));
              LOG_USER_ERROR(OB_NOT_SUPPORTED, "dynamic sql into variable not a question mark");
            }
            OZ (pl_vars.push_back(expr));
          } else {
            /*
             * 直接在sql端执行select 1 into a；mysql会报“ERROR 1327 (42000): Undeclared variable: a”
             * */
            ret = OB_ERR_SP_UNDECLARED_VAR;
            LOG_WARN("PL Variable used in SQL", K(params_.secondary_namespace_), K(ret));
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (NULL == params_.secondary_namespace_
          && params_.is_prepare_protocol_
          && 1 == node->value_) { //动态SQL的这里不允许跟Bulk Collect
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("dynamic sql returning bulk collect is not supported!", K(ret));
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "dynamic sql returning bulk collect");
      } else { //非BULK
        for (uint64_t i = 0; OB_SUCC(ret) && i < pl_vars.count(); ++i) {
          CK (OB_NOT_NULL(pl_vars.at(i)));
          if (OB_SUCC(ret) && pl_vars.at(i)->is_obj_access_expr()) {
            pl::ObPLDataType pl_type;
            const ObUserDefinedType *user_type = NULL;
            const ObRecordType *record_type = NULL;
            ObObjAccessRawExpr* access_expr = static_cast<ObObjAccessRawExpr*>(pl_vars.at(i));
            OZ (access_expr->get_final_type(pl_type));
            if (OB_FAIL(ret)) {
            } else if (pl_type.is_collection_type()) {
              if (pl_type.is_udt_type()) {
                ret = OB_ERR_INVALID_TYPE_FOR_OP;
                LOG_WARN("inconsistent datatypes", K(ret), K(pl_type));
              } else {
                ret = OB_ERR_LOCAL_COLL_IN_SQL;
                LOG_WARN("local collection types not allowed in SQL statements",
                          K(ret), K(pl_type));
              }
            } else if (pl_type.is_record_type()) {
              CK (OB_NOT_NULL(params_.secondary_namespace_));
              OZ (params_.secondary_namespace_->get_pl_data_type_by_id(pl_type.get_user_type_id(),
                                                                       user_type));
              CK (OB_NOT_NULL(user_type));
              CK (user_type->is_record_type());
              CK (OB_NOT_NULL(record_type = static_cast<const ObRecordType*>(user_type)));
              for (int64_t i = 0; OB_SUCC(ret) && i < record_type->get_record_member_count(); ++i) {
                const ObPLDataType *member = record_type->get_record_member_type(i);
                CK (OB_NOT_NULL(member));
                if (OB_SUCC(ret) && !member->is_obj_type()) {
                  ret = OB_ERR_INTO_EXPR_ILLEGAL;
                  LOG_WARN("PLS-00597: expression 'string' in the INTO list is of wrong type", K(ret), K(i));
                }
              }
            }
          }
        }
      }
    }
    if (OB_SUCC(ret) && !pl_vars.empty() && !user_vars.empty()) {
      CK( OB_NOT_NULL(params_.expr_factory_),
          OB_NOT_NULL(params_.session_info_) );
      if (OB_SUCC(ret)) {
        ObArray<ObRawExpr*> tmp_exprs;
        int64_t pl_var_idx = 0;
        for (int64_t i = 0; OB_SUCC(ret) && i < into_node->num_child_; ++i) {
          expr = NULL;
          ch_node = into_node->children_[i];
          if (user_var_idx.has_member(i)) {
            OZ( ObRawExprUtils::build_get_user_var(
                  *params_.expr_factory_,
                  ObString(ch_node->str_len_, ch_node->str_value_),
                  expr,
                  params_.session_info_,
                  params_.query_ctx_,
                  &get_stmt()->get_user_vars()) );
            OZ( tmp_exprs.push_back(expr) );
          } else {
            OZ( tmp_exprs.push_back(pl_vars.at(pl_var_idx++)) );
          }
        }
        OZ (pl_vars.assign(tmp_exprs));
        user_vars.reset();
      }
    }
  }
  if (OB_SUCC(ret) && NULL != select_stmt) {
    ObIArray<SelectItem> &select_items = select_stmt->get_select_items();
    CK(OB_NOT_NULL(params_.session_info_));
    for (int64_t i = 0; i < select_items.count() && OB_SUCC(ret); i++) {
      SelectItem &item = select_items.at(i);
      ObRawExpr *expr = NULL;
      if (OB_ISNULL(expr = item.expr_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("expr of select item is null", K(ret));
      } else if (OB_FAIL(expr->formalize(params_.session_info_))) {
        LOG_WARN("formailize column reference expr failed", K(ret));
      } else if (ob_is_temporal_type(expr->get_data_type())) {
        // add implicit cast to varchar type
        ObCastMode cast_mode = CM_NONE;
        ObExprResType cast_dst_type;
        ObRawExpr *new_expr = NULL;
        ObCollationType coll_type = CS_TYPE_INVALID;
        const int64_t temporal_max_len = 64;
        cast_dst_type.set_type(ObVarcharType);
        cast_dst_type.set_collation_level(CS_LEVEL_IMPLICIT);
        cast_dst_type.set_length(temporal_max_len);
        cast_dst_type.set_calc_meta(ObObjMeta());
        cast_dst_type.set_result_flag(expr->get_result_type().get_result_flag());
        if (OB_FAIL(params_.session_info_->get_collation_connection(coll_type))) {
          LOG_WARN("get collation connection failed", K(ret));
        } else if (FALSE_IT(cast_dst_type.set_collation_type(coll_type))) {
        } else if (OB_FAIL(ObSQLUtils::get_default_cast_mode(params_.session_info_, cast_mode))) {
          LOG_WARN("get default cast mode failed", K(ret));
        } else if (OB_FAIL(ObRawExprUtils::try_add_cast_expr_above(
                                              params_.expr_factory_, params_.session_info_,
                                              *expr,  cast_dst_type, cast_mode, new_expr))) {
          LOG_WARN("try add cast expr above failed", K(ret));
        } else if (OB_FAIL(new_expr->add_flag(IS_OP_OPERAND_IMPLICIT_CAST))) {
          LOG_WARN("failed to add flag", K(ret));
        } else {
          item.expr_ = new_expr;
        }
      }
    }
  }
  return ret;
}

//used to find column in all namespace
//search column ref in table columns
//update, delete, insert only has basic table column
//select has joined table column and  basic table column
//so select resolver will overwrite it
int ObDMLResolver::resolve_column_ref_for_subquery(const ObQualifiedName &q_name, ObRawExpr *&real_ref_expr)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(resolve_basic_column_ref(q_name, real_ref_expr))) {
    LOG_WARN_IGNORE_COL_NOTFOUND(ret, "resolve basic column failed", K(ret), K(q_name));
  }
  return ret;
}

//resolve column ref expr that column in single basic or alias table
// TODO bin.lb:  remove resolve_generated_table_column_item
int ObDMLResolver::resolve_basic_column_ref(const ObQualifiedName &q_name, ObRawExpr *&real_ref_expr)
{
  int ret = OB_SUCCESS;
  //check column namespace
  const TableItem *table_item = NULL;
  ColumnItem *column_item = NULL;
  ObDMLStmt *stmt = get_stmt();
  if (OB_ISNULL(stmt)) {
    ret = OB_NOT_INIT;
    LOG_WARN("stmt is null", K(ret));
  } else {
    if (OB_FAIL(column_namespace_checker_.check_table_column_namespace(q_name, table_item))) {
      LOG_WARN_IGNORE_COL_NOTFOUND(ret, "check basic column namespace failed", K(ret), K(q_name));
    } else if (OB_ISNULL(table_item)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table item is invalid", KPC(table_item));
    } else {
      if (table_item->is_basic_table()) {
        if (OB_FAIL(resolve_basic_column_item(*table_item, q_name.col_name_, false, column_item))) {
          LOG_WARN("resolve column item failed", K(ret));
        }
      } else if (table_item->is_generated_table() || table_item->is_temp_table()) {
        if (OB_FAIL(resolve_generated_table_column_item(*table_item, q_name.col_name_, column_item))) {
          LOG_WARN("resolve column item failed", K(ret));
        }
      }
      if (OB_SUCC(ret)) {
        real_ref_expr = column_item->expr_;
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_basic_column_item(const TableItem &table_item,
                                             const ObString &column_name,
                                             bool include_hidden,
                                             ColumnItem *&col_item,
                                             ObDMLStmt *stmt /* = NULL */)
{
  int ret = OB_SUCCESS;
  ObColumnRefRawExpr *col_expr = NULL;
  ObSchemaGetterGuard *schema_guard = NULL;
  ColumnItem column_item;
  if (NULL == stmt) {
    stmt = get_stmt();
  }
  if (OB_ISNULL(stmt)
      || OB_ISNULL(schema_checker_)
      || OB_ISNULL(schema_guard = schema_checker_->get_schema_guard())
      || OB_ISNULL(params_.expr_factory_)
      || OB_ISNULL(session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema checker is null", K(stmt), K_(schema_checker), K_(params_.expr_factory));
  } else if (OB_UNLIKELY(!table_item.is_basic_table() && !table_item.is_fake_cte_table())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("not base table or alias from base table", K_(table_item.type));
  } else if (NULL != (col_item = stmt->get_column_item(table_item.table_id_, column_name))) {
    //exist, ignore resolve...
  } else {
    bool is_uni = false;
    bool is_mul = false;
    //not resolve, so add column item to dml stmt
    const ObColumnSchemaV2 *col_schema = NULL;
    const ObTableSchema *table_schema = NULL;
    //for materialized view, should use materialized view id to resolve column,
    //and its schema id saved in table_item.table_id_
    uint64_t tid = table_item.ref_id_;
    if (!include_hidden) {
      if (!ObCharset::case_insensitive_equal(column_name, OB_HIDDEN_PK_INCREMENT_COLUMN_NAME)) {
        //do nothing
      } else if (ObResolverUtils::is_restore_user(*session_info_)
                 || ObResolverUtils::is_drc_user(*session_info_)
                 || session_info_->is_inner()) {
        include_hidden = true;
      } else {
        include_hidden = true;
        if (T_NONE_SCOPE == params_.hidden_column_scope_) {
          params_.hidden_column_scope_ = current_scope_;
        }
      }
    }
    if (OB_FAIL(ret)) {
      //do nothing
    } else if (OB_FAIL(schema_checker_->get_table_schema(session_info_->get_effective_tenant_id(), tid, table_schema, table_item.is_link_table()))) {
      LOG_WARN("invalid table id", K(tid));
    } else if (OB_FAIL(get_column_schema(tid, column_name, col_schema, include_hidden, table_item.is_link_table()))) {
      LOG_WARN("get column schema failed", K(ret), K(tid), K(column_name));
    } else if (OB_ISNULL(col_schema)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("column schema is null");
    } else if (OB_FAIL(ObRawExprUtils::build_column_expr(*params_.expr_factory_, *col_schema, col_expr))) {
      LOG_WARN("build column expr failed", K(ret));
    } else if (OB_FAIL(table_schema->is_unique_key_column(*schema_guard,
                                                          col_schema->get_column_id(),
                                                          is_uni))) {
      LOG_WARN("fail to check is unique key column",
      K(ret), KPC(table_schema), K(col_schema->get_column_id()));
    } else if (OB_FAIL(table_schema->is_multiple_key_column(*schema_guard,
                                                            col_schema->get_column_id(),
                                                            is_mul))) {
      LOG_WARN("fail to check is multiple key column",
      K(ret), KPC(table_schema), K(col_schema->get_column_id()));
    } else {
      if (is_oracle_mode() && ObLongTextType == col_expr->get_data_type()
          && ! is_virtual_table(col_schema->get_table_id())) {
        col_expr->set_data_type(ObLobType);
      }
      col_expr->set_synonym_db_name(table_item.synonym_db_name_);
      col_expr->set_synonym_name(table_item.synonym_name_);
      col_expr->set_column_attr(table_item.get_table_name(), col_schema->get_column_name_str());
      col_expr->set_from_alias_table(!table_item.alias_name_.empty());
      col_expr->set_database_name(table_item.database_name_);
      //column maybe from alias table, so must reset ref id by table id from table_item
      col_expr->set_ref_id(table_item.table_id_, col_schema->get_column_id());
      col_expr->set_unique_key_column(is_uni);
      col_expr->set_mul_key_column(is_mul);
      if (table_item.is_link_table()) {
        col_expr->set_link_table_column();
      }
      if (!table_item.alias_name_.empty()) {
        col_expr->set_table_alias_name();
      }
      bool is_lob_column = (ob_is_text_tc(col_schema->get_data_type())
                            || ob_is_json_tc(col_schema->get_data_type())
                            || ob_is_geometry_tc(col_schema->get_data_type()));
      col_expr->set_lob_column(is_lob_column);
      if (session_info_->get_ddl_info().is_ddl()) {
        column_item.set_default_value(col_schema->get_orig_default_value());
      } else {
        column_item.set_default_value(col_schema->get_cur_default_value());
      }
    }
    if (OB_SUCC(ret)) {
      ObString col_def;
      ObRawExpr *ref_expr = NULL;
      if (col_schema->is_generated_column()) {
        column_item.set_default_value(ObObj()); // set null to generated default value
        if (OB_FAIL(col_schema->get_cur_default_value().get_string(col_def))) {
          LOG_WARN("get generated column definition failed", K(ret), K(*col_schema));
        } else if (OB_FAIL(ObSQLUtils::convert_sql_text_from_schema_for_resolve(*allocator_,
                                              session_info_->get_dtc_params(), col_def))) {
          LOG_WARN("fail to convert for resolve", K(ret));
        } else if (OB_FAIL(resolve_generated_column_expr(col_def, table_item, col_schema,
                                                         *col_expr, ref_expr, true, stmt))) {
          LOG_WARN("resolve generated column expr failed", K(ret));
        } else {
          ref_expr->set_for_generated_column();
          col_expr->set_dependant_expr(ref_expr);
        }
      } else if (col_schema->is_default_expr_v2_column()) {
        const bool used_for_generated_column = false;
        if (OB_FAIL(col_schema->get_cur_default_value().get_string(col_def))) {
          LOG_WARN("get expr_default column definition failed", K(ret), KPC(col_schema));
        } else if (OB_FAIL(ObSQLUtils::convert_sql_text_from_schema_for_resolve(*allocator_,
                                              session_info_->get_dtc_params(), col_def))) {
          LOG_WARN("fail to convert for resolve", K(ret));
        } else if (OB_FAIL(resolve_generated_column_expr(col_def, table_item, col_schema, *col_expr,
                                                         ref_expr, used_for_generated_column, stmt))) {
          LOG_WARN("resolve expr_default column expr failed", K(ret), K(col_def), K(*col_schema));
        } else {
          column_item.set_default_value_expr(ref_expr);
        }
      }
    }
    //init column item
    if (OB_SUCC(ret)) {
      column_item.expr_ = col_expr;
      column_item.table_id_ = col_expr->get_table_id();
      column_item.column_id_ = col_expr->get_column_id();
      column_item.column_name_ = col_expr->get_column_name();
      column_item.base_tid_ = tid;
      column_item.base_cid_ = column_item.column_id_;
      column_item.is_geo_ = col_schema->is_geometry();
      LOG_DEBUG("succ to fill column_item", K(column_item), KPC(col_schema));
      if (OB_FAIL(stmt->add_column_item(column_item))) {
        LOG_WARN("add column item to stmt failed", K(ret));
      } else if (OB_FAIL(col_expr->pull_relation_id_and_levels(stmt->get_current_level()))) {
        LOG_WARN("failed to pullup relation ids", K(ret));
      } else {
        col_item = stmt->get_column_item(stmt->get_column_size() - 1);
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_columns(ObRawExpr *&expr, ObArray<ObQualifiedName> &columns)
{
  int ret = OB_SUCCESS;
  ObArray<ObRawExpr*> real_exprs;
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    ObQualifiedName& q_name = columns.at(i);
    ObRawExpr* real_ref_expr = NULL;
    params_.is_column_ref_ = expr->is_column_ref_expr();
    if (OB_FAIL(resolve_qualified_identifier(q_name, columns, real_exprs, real_ref_expr))) {
      LOG_WARN_IGNORE_COL_NOTFOUND(ret, "resolve column ref expr failed", K(ret), K(q_name));
      report_user_error_msg(ret, expr, q_name);
    } else if (OB_FAIL(real_exprs.push_back(real_ref_expr))) {
      LOG_WARN("push back failed", K(ret));
    } else if (OB_FAIL(ObRawExprUtils::replace_ref_column(expr, q_name.ref_expr_, real_ref_expr))) {
      LOG_WARN("replace column ref expr failed", K(ret));
    } else { /*do nothing*/ }
  }
  return ret;
}

int ObDMLResolver::resolve_qualified_identifier(ObQualifiedName &q_name,
                                                ObIArray<ObQualifiedName> &columns,
                                                ObIArray<ObRawExpr*> &real_exprs,
                                                ObRawExpr *&real_ref_expr)
{
  int ret = OB_SUCCESS;
  bool is_external = false;
  if (OB_ISNULL(stmt_) || OB_ISNULL(stmt_->get_query_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), KP(stmt_));
  } else if (q_name.is_sys_func()) {
    if (OB_FAIL(q_name.access_idents_.at(0).sys_func_expr_->check_param_num())) {
      LOG_WARN("sys func param number not match", K(ret));
    } else {
      real_ref_expr = static_cast<ObRawExpr *>(q_name.access_idents_.at(0).sys_func_expr_);
      is_external = (T_FUN_PL_GET_CURSOR_ATTR == real_ref_expr->get_expr_type());
    }
  } else if (q_name.is_pl_udf() || q_name.is_pl_var()) {
    is_external = true;
    if (OB_FAIL(resolve_external_name(q_name, columns, real_exprs, real_ref_expr))) {
      LOG_WARN("resolve column ref expr failed", K(ret), K(q_name));
    } else if (real_ref_expr->is_udf_expr()) {
      ObUDFRawExpr *udf = static_cast<ObUDFRawExpr *>(real_ref_expr);
      if (OB_ISNULL(udf)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("udf is null", K(ret), K(udf));
      } else if (udf->has_param_out()) {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("You tried to execute a SQL statement that referenced a package or function\
            that contained an OUT parameter. This is not allowed.", K(ret), K(q_name));
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "ORA-06572: function name has out arguments");
      } else if (udf->is_pkg_body_udf()) {
        ret = OB_ERR_PRIVATE_UDF_USE_IN_SQL;
        LOG_WARN("function 'string' may not be used in SQL", K(ret), KPC(udf));
      } else {
        stmt_->get_query_ctx()->has_pl_udf_ = true;
      }
    } else if (T_FUN_PL_COLLECTION_CONSTRUCT == real_ref_expr->get_expr_type()) {
      if (!params_.is_resolve_table_function_expr_) {
        //such as insert into tbl values(1,3, coll('a', 1));
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("dml with collection or record construction function is not supported", K(ret));
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "dml with collection or record construction function is");
      } else {
        is_external = false;
      }
    } else if (T_FUN_PL_OBJECT_CONSTRUCT == real_ref_expr->get_expr_type()) {
      is_external = false;
    }
  } else if (lib::is_oracle_mode() && q_name.col_name_.length() == 0) {
    //对于长度为0的identifier报错和oracle兼容
    ret = OB_ERR_ZERO_LENGTH_IDENTIFIER;
    LOG_WARN("illegal zero-length identifier", K(ret));
  } else {
    CK (OB_NOT_NULL(get_basic_stmt()));
    if (OB_SUCC(ret)) {
      if (lib::is_oracle_mode()
      && NULL != params_.secondary_namespace_
      && get_basic_stmt()->is_insert_stmt()
      && !static_cast<ObInsertStmt*>(get_basic_stmt())->value_from_select()) {
        //oracle模式insert语句的values子句，标识符优先解释为变量
        if (!q_name.access_idents_.empty()) { //q_name.access_idents_为NULL肯定是列
          if (OB_FAIL(resolve_external_name(q_name, columns, real_exprs, real_ref_expr))) {
            LOG_WARN_IGNORE_COL_NOTFOUND(ret, "resolve external symbol failed", K(ret), K(q_name));
          } else if (T_FUN_PL_COLLECTION_CONSTRUCT == real_ref_expr->get_expr_type()) {
            //such as insert into tbl values(1,3, coll('a', 1));
            ret = OB_NOT_SUPPORTED;
            LOG_WARN("dml with collection or record construction function is not supported", K(ret));
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "dml with collection or record construction function is");
          } else if ((ObExtendType == real_ref_expr->get_result_type().get_type()
                   || ObMaxType == real_ref_expr->get_result_type().get_type())
                   && (T_FUN_PL_SQLCODE_SQLERRM != real_ref_expr->get_expr_type())) {
            ret = OB_NOT_SUPPORTED;
            LOG_WARN("dml with collection or record construction function is not supported", K(ret));
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "dml with collection or record construction function is");
          } else {
            is_external = true;
          }
        }

        if (OB_ERR_BAD_FIELD_ERROR == ret || q_name.access_idents_.empty()) {
          if (OB_FAIL(resolve_column_ref_expr(q_name, real_ref_expr))) {
            if (OB_ERR_BAD_FIELD_ERROR == ret) {
              if (OB_FAIL(ObRawExprUtils::resolve_sequence_object(q_name, this, session_info_,
                                                                 params_.expr_factory_,
                                                                 sequence_namespace_checker_,
                                                                 real_ref_expr, false))) {
                LOG_WARN_IGNORE_COL_NOTFOUND(ret, "resolve sequence object failed", K(ret), K(q_name));
              }
            }

            if (OB_ERR_BAD_FIELD_ERROR == ret) {
              if (OB_FAIL(resolve_pseudo_column(q_name, real_ref_expr))) {
                LOG_WARN_IGNORE_COL_NOTFOUND(ret, "resolve pseudo column failed", K(ret), K(q_name));
                ret = OB_ERR_BAD_FIELD_ERROR;
              }
            }
          }
        }
      } else {
        if (OB_FAIL(resolve_column_ref_expr(q_name, real_ref_expr))) {
          if (OB_ERR_BAD_FIELD_ERROR == ret) {
            if (OB_FAIL(ObRawExprUtils::resolve_sequence_object(q_name, this, session_info_,
                                                                params_.expr_factory_,
                                                                sequence_namespace_checker_,
                                                                real_ref_expr, false))) {
              LOG_WARN_IGNORE_COL_NOTFOUND(ret, "resolve sequence object failed", K(ret), K(q_name));
            }
          }

          if (OB_ERR_BAD_FIELD_ERROR == ret) {
            if (OB_FAIL(resolve_pseudo_column(q_name, real_ref_expr))) {
              LOG_WARN_IGNORE_COL_NOTFOUND(ret, "resolve pseudo column failed", K(ret), K(q_name));
            }
          }
          if (OB_ERR_BAD_FIELD_ERROR == ret && !q_name.access_idents_.empty()) { //q_name.access_idents_为NULL肯定是列
            if (OB_FAIL(resolve_external_name(q_name, columns, real_exprs, real_ref_expr))) {
              LOG_WARN_IGNORE_COL_NOTFOUND(ret, "resolve external symbol failed", K(ret), K(q_name));
              ret = OB_ERR_BAD_FIELD_ERROR; // TODO: 单测test_resolver_select.test:465 select 1 as a from t1,t2 having c1=1; 失败
            } else {
              is_external = true;
            }
          } else if (!OB_SUCC(ret)) {
            LOG_WARN("resolve column ref expr failed", K(ret), K(q_name));
          }
        }
      }
      if (OB_SUCC(ret) && OB_NOT_NULL(real_ref_expr) && real_ref_expr->is_udf_expr()) {
        stmt_->get_query_ctx()->has_pl_udf_ = true;
      }
    }
  }

  //因为obj access的参数拉平处理，a(b,c)在columns会被存储为b,c,a，所以解释完一个ObQualifiedName，
  //都要把他前面的ObQualifiedName拿过来尝试替换一遍参数
  for (int64_t i = 0; OB_SUCC(ret) && i < real_exprs.count(); ++i) {
    if (OB_FAIL(ObRawExprUtils::replace_ref_column(real_ref_expr, columns.at(i).ref_expr_, real_exprs.at(i)))) {
      LOG_WARN("replace column ref expr failed", K(ret));
    }
  }

  //把需要传给PL的表达式整体替换成param
  if (OB_SUCC(ret)) {
    if (OB_ISNULL(real_ref_expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("expr is NULL", K(ret));
    } else if (T_FUN_PL_SQLCODE_SQLERRM == real_ref_expr->get_expr_type()) {
      ret = OB_ERR_SP_UNDECLARED_VAR;
      LOG_WARN("sqlcode or sqlerrm can not use in dml directly", K(ret), KPC(real_ref_expr));
    } else {
      if (q_name.is_access_root()
        && is_external
        && !params_.is_default_param_
        && T_INTO_SCOPE != current_scope_
        && NULL != params_.secondary_namespace_ //仅PL里的SQL出现了外部变量需要替换成QUESTIONMARK，纯SQL语境的不需要
        && (real_ref_expr->is_const_raw_expr() //local变量
            || real_ref_expr->is_obj_access_expr() //复杂变量
            || T_OP_GET_PACKAGE_VAR == real_ref_expr->get_expr_type() //package变量(system/user variable不会走到这里)
            || real_ref_expr->is_sys_func_expr()
            || T_FUN_PL_GET_CURSOR_ATTR == real_ref_expr->get_expr_type())) { //允许CURSOR%ROWID通过
        /*
         * 在已有的表达式里寻找是否有相同，如果有相同则使用同一个QuestionMark
         * */
        OZ (ObResolverUtils::resolve_external_param_info(params_.external_param_info_,
                                                         *params_.expr_factory_,
                                                         params_.prepare_param_count_,
                                                         real_ref_expr));
      }
    }
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    OZ (columns.at(i).replace_access_ident_params(q_name.ref_expr_, real_ref_expr));
  }

  if (OB_ERR_BAD_FIELD_ERROR == ret) {
    // 为了兼容 Oracle 的报错方式:
    //
    // SQL> select nextval from dual;
    // select nextval from dual
    // ERROR at line 1:
    // ORA-00904: "NEXTVAL": invalid identifier
    //
    // SQL> select s.nextval from dual;
    // select s.nextval from dual
    // ERROR at line 1:
    // ORA-02289: sequence does not exist
    ret = update_errno_if_sequence_object(q_name, ret);
  }
  return ret;
}

void ObDMLResolver::report_user_error_msg(int &ret, const ObRawExpr *root_expr, const ObQualifiedName &q_name) const
{
  if (OB_ERR_BAD_FIELD_ERROR == ret && !q_name.tbl_name_.empty()) {
    if (stmt_ != NULL && stmt_->is_select_stmt()) {
      const ObSelectStmt *select_stmt = static_cast<const ObSelectStmt*>(stmt_);
      if (select_stmt->is_set_stmt()) {
        //can't use table name in union query
        //eg. select c1 from t1 union select c1 from t1 order by t1.c1
        ret = OB_ERR_TABLENAME_NOT_ALLOWED_HERE;
        LOG_USER_ERROR(OB_ERR_TABLENAME_NOT_ALLOWED_HERE, q_name.tbl_name_.length(), q_name.tbl_name_.ptr());
      } else if (select_stmt->get_table_size() <= 0 && current_level_ <= 0) {
        //can't use table name in select from dual
        //eg. select t1.c1
        ret = OB_ERR_UNKNOWN_TABLE;
        ObString tbl_name = concat_table_name(q_name.database_name_, q_name.tbl_name_);
        ObString scope_name = ObString::make_string(get_scope_name(current_scope_));
        ObSQLUtils::copy_and_convert_string_charset(*allocator_, tbl_name, tbl_name,
                        CS_TYPE_UTF8MB4_BIN, session_info_->get_local_collation_connection());
        LOG_USER_ERROR(OB_ERR_UNKNOWN_TABLE, tbl_name.length(), tbl_name.ptr(), scope_name.length(), scope_name.ptr());
      }
    }
  }
  if (OB_ERR_BAD_FIELD_ERROR == ret) {
    ObString column_name = concat_qualified_name(q_name.database_name_, q_name.tbl_name_, q_name.col_name_);
    ObString scope_name = ObString::make_string(get_scope_name(current_scope_));
    ObSQLUtils::copy_and_convert_string_charset(*allocator_, column_name, column_name,
                    CS_TYPE_UTF8MB4_BIN, session_info_->get_local_collation_connection());
    LOG_USER_ERROR(OB_ERR_BAD_FIELD_ERROR, column_name.length(), column_name.ptr(), scope_name.length(), scope_name.ptr());
  } else if (OB_NON_UNIQ_ERROR == ret) {
    ObString column_name = concat_qualified_name(q_name.database_name_, q_name.tbl_name_, q_name.col_name_);
    ObString scope_name = ObString::make_string(get_scope_name(current_scope_));
    ObSQLUtils::copy_and_convert_string_charset(*allocator_, column_name, column_name,
                    CS_TYPE_UTF8MB4_BIN, session_info_->get_local_collation_connection());
    LOG_USER_ERROR(OB_NON_UNIQ_ERROR, column_name.length(), column_name.ptr(), scope_name.length(), scope_name.ptr());
  } else if (OB_ILLEGAL_REFERENCE == ret) {
    //compatiable with mysql
    //select max(c1) as c from t1 group by c -> err msg:ERROR 1056 (42000): Can't group on 'c'
    //others: select max(c1) as c from t1 group by c+1 ->
    //err msg:ERROR 1247 (42S22): Reference 'c' not supported (reference to group function)
    ObString column_name = q_name.col_name_;
    ObSQLUtils::copy_and_convert_string_charset(*allocator_, column_name, column_name,
                    CS_TYPE_UTF8MB4_BIN, session_info_->get_local_collation_connection());
    if (root_expr == q_name.ref_expr_ && q_name.ref_expr_->get_expr_level() == current_level_) {
      ret = OB_WRONG_GROUP_FIELD;
      LOG_USER_ERROR(OB_WRONG_GROUP_FIELD, column_name.length(), column_name.ptr());
    } else {
      LOG_USER_ERROR(OB_ILLEGAL_REFERENCE, column_name.length(), column_name.ptr());
    }
  }
}

//select resolver has namespace more than one layer, select resolve will overwrite it
int ObDMLResolver::resolve_column_ref_expr(const ObQualifiedName &q_name, ObRawExpr *&real_ref_expr)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(resolve_table_column_expr(q_name, real_ref_expr))) {
    LOG_WARN_IGNORE_COL_NOTFOUND(ret, "resolve table column failed", K(q_name), K(ret));
  }
  return ret;
}

int ObDMLResolver::resolve_aggr_exprs(ObRawExpr *&expr,
    common::ObIArray<ObAggFunRawExpr*> &aggr_exprs, const bool need_analyze/* = true*/)
{
  UNUSED(expr);
  UNUSED(need_analyze);
  int ret = OB_SUCCESS;
  if (aggr_exprs.count() > 0 && !is_select_resolver()) {
    if (OB_UNLIKELY(T_FIELD_LIST_SCOPE != current_scope_)) {
      ret = OB_ERR_INVALID_GROUP_FUNC_USE;
      LOG_WARN("invalid scope for agg function", K(ret), K(current_scope_));
    }
    // for (int64_t i = 0; OB_SUCC(ret) && i < aggr_exprs.count(); i++) {
      // ObAggFunRawExpr *final_aggr = NULL;
      // if (final_aggr != aggr_exprs.at(i)) {
        // if (OB_FAIL(ObRawExprUtils::replace_ref_column(expr, aggr_exprs.at(i), final_aggr))) {
          // LOG_WARN("repalce reference column failed", K(ret));
        // }
      // }
    // }
    for (int64_t i = 0; OB_SUCC(ret) && i < aggr_exprs.count(); i++) {
      ObDelUpdStmt *del_up_stmt = static_cast<ObDelUpdStmt*>(stmt_);
      if (OB_ISNULL(del_up_stmt) || OB_ISNULL(expr)) {
        ret = OB_NOT_INIT;
        LOG_WARN("del_up_stmt is null", K(ret));
      } else if (OB_FAIL(del_up_stmt->add_returning_agg_item(*(aggr_exprs.at(i))))) {
        LOG_WARN("add agg item failed", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_win_func_exprs(ObRawExpr *&expr, common::ObIArray<ObWinFunRawExpr*> &win_exprs)
{
  UNUSED(expr);
  UNUSED(win_exprs);
  return OB_ERR_INVALID_WINDOW_FUNC_USE;
}

int ObDMLResolver::check_resolve_oracle_sys_view(const ParseNode *node, bool &is_oracle_view)
{
  int ret = OB_SUCCESS;
  is_oracle_view = false;
  ObString table_name;
  ParseNode *db_node = node->children_[0];
  ParseNode *relation_node = node->children_[1];
  int32_t table_len = static_cast<int32_t>(relation_node->str_len_);
  table_name.assign_ptr(const_cast<char*>(relation_node->str_value_), table_len);
  if (nullptr == db_node && is_oracle_mode()) {
    if (session_info_->get_database_name().empty()) {
      ret = OB_ERR_NO_DB_SELECTED;
      LOG_WARN("No database selected");
    } else if (ObSQLUtils::is_oracle_sys_view(table_name)) {
      is_oracle_view = true;
    } else {
      LOG_DEBUG("table_name", K(table_name));
    }
  }
  return ret;
}

int ObDMLResolver::inner_resolve_sys_view(const ParseNode *table_node,
                                          uint64_t &database_id,
                                          ObString &tbl_name,
                                          ObString &db_name,
                                          bool &use_sys_tenant)
{
  int ret = OB_SUCCESS;
  bool is_db_explicit = false;
  if (OB_FAIL(inner_resolve_sys_view(table_node,
                                     database_id,
                                     tbl_name,
                                     db_name,
                                     is_db_explicit,
                                     use_sys_tenant))) {
    LOG_WARN("failed to inner_resolve_sys_view", K(ret));
  }
  return ret;
}

// oracle sys view will resolve again
int ObDMLResolver::inner_resolve_sys_view(const ParseNode *table_node,
                                          uint64_t &database_id,
                                          ObString &tbl_name,
                                          ObString &db_name,
                                          bool &is_db_explicit,
                                          bool &use_sys_tenant)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  ObString tmp_db_name;
  ObString tmp_tbl_name;
  bool is_oracle_sys_view = false;
  use_sys_tenant = false; // won't find table in sys tenant
  if (OB_SUCCESS != (tmp_ret = resolve_table_relation_node_v2(table_node,
                                                              tmp_tbl_name,
                                                              tmp_db_name,
                                                              is_db_explicit))) {
    LOG_WARN("fail to resolve table relation node", K(tmp_ret));
    tmp_ret = OB_SUCCESS;
  }
  // try resovle sys view in oracle mode
  if (!use_sys_tenant && (OB_SUCCESS != (tmp_ret = check_resolve_oracle_sys_view(table_node, is_oracle_sys_view)))) {
    LOG_WARN("fail to check resolve oracle sys view", K(tmp_ret));
  } else if (is_oracle_sys_view) {
    // resolve sys view in oracle mode
    if (OB_SUCCESS != (tmp_ret = resolve_table_relation_node_v2(table_node,
                                                                tmp_tbl_name,
                                                                tmp_db_name,
                                                                is_db_explicit,
                                                                false,
                                                                is_oracle_sys_view))) {
      LOG_WARN("fail to resolve table relation node", K(tmp_ret));
    } else {
      const bool is_index_table = false;
      const ObTableSchema *table_schema = NULL;
      if (OB_SUCCESS != (tmp_ret = schema_checker_->get_table_schema(session_info_->get_effective_tenant_id(),
                                                                      tmp_db_name,
                                                                      tmp_tbl_name,
                                                                      is_index_table,
                                                                      table_schema))) {
        LOG_WARN("fail to get table schema", K(tmp_ret), K(tmp_db_name), K(tmp_tbl_name));
      } else if (NULL == table_schema) {
        tmp_ret = OB_INVALID_ARGUMENT;
        LOG_WARN("table_schema should not be NULL", K(tmp_ret));
      } else if (!table_schema->is_sys_view()) {
        is_oracle_sys_view = false;
      } else if (OB_SUCCESS != (tmp_ret = schema_checker_->get_database_id(session_info_->get_effective_tenant_id(), tmp_db_name, database_id))) {
        LOG_WARN("fail to get database id", K(tmp_ret));
      }
    }
  }

  if (tmp_ret != OB_SUCCESS) {
    is_oracle_sys_view = false;
    ret = tmp_ret;
  } else if (is_oracle_sys_view) {
    ret = OB_SUCCESS;
    db_name = tmp_db_name;
    tbl_name = tmp_tbl_name;
    SQL_RESV_LOG(INFO, "table found in sys tenant", K(tmp_db_name), K(tmp_tbl_name));
  } else {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("fail to resolve table", K(ret));
  }
  return ret;
}

// 这个函数获取库名与表名, 并对表作存在性检查
// 普通租户下有一部分系统视图需要访问系统租户的表,
// 对于系统租户独有的表, 普通租户无法获取其schema
// 因此在当前租户找不到表并且满足一定的条件时，会以系统租户的身份再找一遍
// 这些条件包括 :
// 1. 当前是普通租户
// 2. 当前stmt是系统视图展开的
// 若在系统租户下找到的是用户表, 则忽略
int ObDMLResolver::resolve_table_relation_factor_wrapper(const ParseNode *table_node,
                                                         uint64_t &dblink_id,
                                                         uint64_t &database_id,
                                                         ObString &tbl_name,
                                                         ObString &synonym_name,
                                                         ObString &synonym_db_name,
                                                         ObString &db_name,
                                                         ObString &dblink_name,
                                                         bool &is_db_explicit,
                                                         bool &use_sys_tenant)
{
  int ret = OB_SUCCESS;

  if (NULL == table_node) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("table_node should not be NULL", K(ret));
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(resolve_table_relation_factor(table_node,
                                              dblink_id,
                                              database_id,
                                              tbl_name,
                                              synonym_name,
                                              synonym_db_name,
                                              db_name,
                                              dblink_name,
                                              is_db_explicit))) {
      if (ret != OB_TABLE_NOT_EXIST) {
        // 只关心找不到表的情况，因此这里直接跳过
      } else {
        int tmp_ret = OB_SUCCESS;
        if (OB_SUCCESS != (tmp_ret = inner_resolve_sys_view(table_node,
                                                            database_id,
                                                            tbl_name,
                                                            db_name,
                                                            is_db_explicit,
                                                            use_sys_tenant))) {
          LOG_WARN("fail to resolve sys view", K(ret));
        } else {
          ret = tmp_ret;
        }
      }
    }
  }

  return ret;
}

int ObDMLResolver::resolve_sys_vars(ObArray<ObVarInfo> &sys_vars)
{
  int ret = OB_SUCCESS;
  ObQueryCtx *query_ctx = NULL;
  if (OB_ISNULL(stmt_) || OB_ISNULL(query_ctx = stmt_->get_query_ctx())) {
    ret = OB_NOT_INIT;
    LOG_WARN("stmt_ or query_ctx is null", K_(stmt), K(query_ctx));
  } else if (OB_FAIL(ObRawExprUtils::merge_variables(sys_vars, query_ctx->variables_))) {
    LOG_WARN("failed to record variables", K(ret));
  }
  return ret;
}

int ObDMLResolver::resolve_basic_table(const ParseNode &parse_tree, TableItem *&table_item)
{
  int ret = OB_SUCCESS;
  const ParseNode *table_node = &parse_tree;
  const ParseNode *alias_node = NULL;
  const ParseNode *index_hint_node = NULL;
  const ParseNode *part_node = NULL;
  const ParseNode *sample_node = NULL;
  const ParseNode *time_node = NULL;
  const ParseNode *transpose_node = NULL;
  bool is_db_explicit = false;
  ObDMLStmt *stmt = get_stmt();
  uint64_t tenant_id = OB_INVALID_ID;
  uint64_t dblink_id = OB_INVALID_ID;
  ObString database_name;
  ObString table_name;
  ObString alias_name;
  ObString synonym_name;
  ObString dblink_name;
  ObString synonym_db_name;
  bool use_sys_tenant = false;
  uint64_t database_id = OB_INVALID_ID;
  const ObTableSchema *table_schema = NULL;

  if (T_ORG == parse_tree.type_) {
    table_node = parse_tree.children_[0];
    index_hint_node = parse_tree.children_[1];
    part_node = parse_tree.children_[2];
    if (parse_tree.num_child_ >= 4) {
      sample_node = parse_tree.children_[3];
    }
    if (parse_tree.num_child_ >= 5) {
      time_node = parse_tree.children_[4];
    }
  } else if (T_ALIAS == parse_tree.type_) {
    table_node = parse_tree.children_[0];
    alias_node = parse_tree.children_[1];
    if (T_RELATION_FACTOR == table_node->type_) {
      index_hint_node = parse_tree.children_[2];
      part_node = parse_tree.children_[3];
      if (parse_tree.num_child_ >= 5) {
        sample_node = parse_tree.children_[4];
      }
      if (parse_tree.num_child_ >= 6) {
        time_node = parse_tree.children_[5];
      }
      if (parse_tree.num_child_ >= 7) {
        transpose_node = parse_tree.children_[6];
      }
    }
  }

  if (OB_FAIL(resolve_table_relation_factor_wrapper(table_node,
                                                    dblink_id,
                                                    database_id,
                                                    table_name,
                                                    synonym_name,
                                                    synonym_db_name,
                                                    database_name,
                                                    dblink_name,
                                                    is_db_explicit,
                                                    use_sys_tenant))) {
    if (OB_TABLE_NOT_EXIST == ret || OB_ERR_BAD_DATABASE == ret) {
      if (is_information_schema_database_id(database_id)) {
        ret = OB_ERR_UNKNOWN_TABLE;
        LOG_USER_ERROR(OB_ERR_UNKNOWN_TABLE, table_name.length(), table_name.ptr(), database_name.length(), database_name.ptr());
      } else {
        ret = OB_TABLE_NOT_EXIST;
        LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(database_name), to_cstring(table_name));
      }
    } else {
      LOG_WARN("fail to resolve table name", K(ret));
    }
  }

  if (OB_SUCC(ret) && OB_INVALID_ID == dblink_id) {
    if (alias_node != NULL) {
      alias_name.assign_ptr(alias_node->str_value_, static_cast<int32_t>(alias_node->str_len_));
    }

    // flag IS to set larger query_timeout.
    if (is_information_schema_database_id(database_id)) {
      params_.query_ctx_->has_is_table_ = true;
    }
    tenant_id = use_sys_tenant ? OB_SYS_TENANT_ID : session_info_->get_effective_tenant_id();
    bool cte_table_fisrt = (table_node->children_[0] == NULL);
    if (OB_FAIL(resolve_base_or_alias_table_item_normal(tenant_id,
                                                        database_name,
                                                        is_db_explicit,
                                                        table_name,
                                                        alias_name,
                                                        synonym_name,
                                                        synonym_db_name,
                                                        table_item,
                                                        cte_table_fisrt))) {
      LOG_WARN("resolve base or alias table item failed", K(ret));
    } else {
      //如果当前解析的表属于oracle租户,在线程局部设置上mode.
      lib::Worker::CompatMode compat_mode;
      ObCompatModeGetter::get_tenant_mode(tenant_id, compat_mode);
      lib::CompatModeGuard g(compat_mode);
      bool is_sync_ddl_user = false;
      if (OB_ISNULL(table_item) || OB_ISNULL(stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("NULL ptr", K(stmt), K(ret));
      } else if (OB_ISNULL(session_info_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("session_info_ is null", K(ret));
      } else if (OB_FAIL(schema_checker_->get_table_schema(session_info_->get_effective_tenant_id(), table_item->ref_id_, table_schema))) {
        ret = OB_TABLE_NOT_EXIST;
        LOG_WARN("get table schema failed", K_(table_item->table_name), K(tenant_id), K(database_id), K(ret));
      } else if (table_schema->need_encrypt() && lib::is_oracle_mode() && OB_FAIL(check_keystore_status())) {
        LOG_WARN("keystore status does not meet expectations when accessing encrypted tables", K(ret));
      } else if (OB_FAIL(resolve_generated_column_expr_temp(table_item, *table_schema))) {
        LOG_WARN("resolve generated column expr templte failed", K(ret));
      } else if(OB_FAIL(ObResolverUtils::check_sync_ddl_user(session_info_, is_sync_ddl_user))) {
        // liboblog会对数据乱序排列，可能导致更新的数据放到删除表之后, 回放时就可能操作回收站里的表
        LOG_WARN("Failed to check sync_ddl_user", K(ret));
      } else if (!stmt->is_select_stmt() && table_schema->is_in_recyclebin() && !is_sync_ddl_user) {
        ret = OB_ERR_OPERATION_ON_RECYCLE_OBJECT;
        LOG_WARN("write operation on recylebin object is not allowed", K(ret),
                 "stmt_type", stmt->get_stmt_type());
      } else if (table_schema->is_vir_table() && !stmt->is_select_stmt()) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "DML operation on Virtual Table/Temporary Table");
      } else if (params_.is_from_create_view_ && table_schema->is_mysql_tmp_table()) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "View/Table's column refers to a temporary table");
      } else if (OB_FAIL(resolve_table_partition_expr(*table_item, *table_schema))) {
        LOG_WARN("resolve table partition expr failed", K(ret), K(table_name));
      } else if (OB_FAIL(resolve_table_check_constraint_items(table_item, table_schema))) {
        LOG_WARN("resolve table partition expr failed", K(ret), K(table_name));
      } else if (stmt->is_select_stmt() && OB_FAIL(resolve_geo_mbr_column())) {
        LOG_WARN("resolve geo mbr column failed", K(ret), K(table_name));
      } else if (table_schema->is_oracle_tmp_table() && stmt::T_MERGE == stmt->get_stmt_type()) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "MERGE refers to a temporary table");
      } else if (NULL != index_hint_node &&
                 OB_FAIL(resolve_index_hint(*table_item, *index_hint_node))) {
        LOG_WARN("resolve index hint failed", K(ret));
      }

      if (OB_SUCCESS == ret && table_item->is_view_table_) {
        if (OB_FAIL(expand_view(*table_item))) {
          LOG_WARN("expand view failed", K(ret), K(*table_item));
        }
      }
      if (OB_SUCCESS == ret && part_node) {
        if (is_virtual_table(table_item->ref_id_) &&
            table_schema->get_part_option().get_part_num() > 1) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "Partitioned virtual table with partition hint");
        } else if (table_item->cte_type_ != TableItem::CTEType::NOT_CTE) {
          // ret = -14109
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "Partitioned cte table with partition hint");
        } else if (table_item->is_view_table_) {
          if (lib::is_oracle_mode()) {
            ret = OB_ERR_PARTITION_EXTENDED_ON_VIEW;
            LOG_WARN("partition-extended object names on views not allowed", K(ret));
          }
        } else if (OB_FAIL(resolve_partitions(part_node, *table_schema, *table_item))) {
          LOG_WARN("Resolve partitions error", K(ret));
        } else { }
      }
      if (OB_SUCCESS == ret && sample_node != NULL && T_SAMPLE_SCAN == sample_node->type_) {
        if (is_virtual_table(table_item->ref_id_) &&
            !is_oracle_mapping_real_virtual_table(table_item->ref_id_)) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "sampling virtual table");
        } else if (OB_FAIL(resolve_sample_clause(sample_node, table_item->table_id_))) {
          LOG_WARN("resolve sample clause failed", K(ret));
        } else { }
      }
      //resolve flashback query node
      if (OB_SUCCESS == ret && time_node != NULL) {
        if (OB_FAIL(resolve_flashback_query_node(time_node, table_item))) {
          LOG_WARN("failed to resolve flashback query node", K(ret));
        //针对view需要递归的设置view对应查询的table的flashback query属性
        } else if (table_item->is_view_table_) {
          if (OB_FAIL(set_flashback_info_for_view(table_item->ref_query_, table_item))) {
            LOG_WARN("failed to set flashback info for view", K(ret));
          } else {
            //针对view的flashback属性经过set_flashback_info_for_view后,已经没用,为了不影响后续判断
            //这里将其还原为默认值
            table_item->flashback_query_expr_ = NULL;
            table_item->flashback_query_type_ = TableItem::NOT_USING;
          }
        } else {
          /*do nothing*/
        }
      }
      if (OB_SUCCESS == ret && is_virtual_table(table_item->ref_id_)) {
        stmt->get_query_ctx()->is_contain_virtual_table_ = true;
      }

      if (OB_SUCCESS == ret) {
        if (OB_FAIL(resolve_transpose_table(transpose_node, table_item))) {
          LOG_WARN("resolve_transpose_table failed", K(ret));
        }
      }
    }
  } // if (OB_SUCC(ret) && OB_INVALID_ID == dblink_id)
  if (OB_SUCC(ret) && OB_INVALID_ID != dblink_id) {
    if (OB_NOT_NULL(part_node)) {
      ret = OB_ERR_REMOTE_PART_ILLEGAL;
      LOG_WARN("partition extended table name cannot refer to a remote object", K(ret));
    } else if (!OB_ISNULL(alias_node)) {
      alias_name.assign_ptr(alias_node->str_value_, static_cast<int32_t>(alias_node->str_len_));
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(resolve_base_or_alias_table_item_dblink(dblink_id,
                                                          dblink_name,
                                                          database_name,
                                                          table_name,
                                                          alias_name,
                                                          synonym_name,
                                                          synonym_db_name,
                                                          table_item))) {
        LOG_WARN("resolve base or alias table item for dblink failed", K(ret));
      }
    }
  }

  LOG_DEBUG("finish resolve_basic_table", K(ret), KPC(table_item));
  return ret;
}

int ObDMLResolver::resolve_table_check_constraint_items(const TableItem *table_item,
                                                        const ObTableSchema *table_schema)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *dml_stmt = NULL;
  ObSEArray<ObRawExpr*, 4> stmt_constr_exprs;
  ObSEArray<int64_t, 4> check_flags;
  if (OB_ISNULL(dml_stmt = get_stmt()) || OB_ISNULL(table_item)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get stmt null", K(ret), K(dml_stmt), K(table_item));
  } else if (OB_FAIL(generate_check_constraint_exprs(table_item, table_schema, stmt_constr_exprs, &check_flags))) {
    LOG_WARN("failed to add check constraint to stmt");
  } else if (!stmt_constr_exprs.empty()) {
    ObDMLStmt::CheckConstraintItem check_constraint_item;
    check_constraint_item.table_id_ = table_item->table_id_;
    check_constraint_item.ref_table_id_ = table_schema->get_table_id();
    if (OB_FAIL(append(check_constraint_item.check_constraint_exprs_, stmt_constr_exprs))) {
      LOG_WARN("failed to append", K(ret));
    } else if (OB_FAIL(append(check_constraint_item.check_flags_, check_flags))) {
      LOG_WARN("failed to append", K(ret));
    } else if (OB_FAIL(dml_stmt->set_check_constraint_item(check_constraint_item))) {
      LOG_WARN("failed to set check constraint item", K(ret));
    } else {
      LOG_TRACE("succeed to resolve table check constraint items", K(table_item->table_id_),
                        K(table_schema->get_table_id()), K(check_constraint_item));
    }
  }
  return ret;
}

int ObDMLResolver::check_flashback_expr_validity(ObRawExpr *expr, bool &has_column)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("expr is null", K(ret));
  } else if (expr->is_column_ref_expr()) {
    has_column = true;
  } else if (expr->is_exec_param_expr()) {
    if (OB_FAIL(check_flashback_expr_validity(
                  static_cast<ObExecParamRawExpr *>(expr)->get_ref_expr(),
                  has_column))) {
      LOG_WARN("failed to check exec param expr", K(ret));
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && !has_column && i < expr->get_param_count(); ++i) {
      if (OB_FAIL(check_flashback_expr_validity(expr->get_param_expr(i),
                                                has_column))) {
        LOG_WARN("failed to check param expr", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_flashback_query_node(const ParseNode *time_node, TableItem *table_item)
{
  int ret = OB_SUCCESS;
  ParseNode *tmp_time_node = NULL;
  bool has_column = false;
  if (OB_ISNULL(time_node) || OB_ISNULL(table_item) || OB_ISNULL(session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected error", K(ret), K(time_node), K(table_item));
  } else if (T_TABLE_FLASHBACK_QUERY_TIMESTAMP == time_node->type_
             || T_TABLE_FLASHBACK_QUERY_SCN == time_node->type_) {
    tmp_time_node = time_node->children_[0];
    if (OB_NOT_NULL(tmp_time_node)) {
      ObRawExpr *expr = nullptr;
      if (OB_FAIL(resolve_sql_expr(*tmp_time_node, expr))) {
        LOG_WARN("resolve sql expr failed", K(ret));
      } else if (OB_ISNULL(expr)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("expr is null", K(ret));
      } else if (T_REF_QUERY == expr->get_expr_type()) {
        ret = OB_ERR_INVALID_SUBQUERY_USE;
        LOG_WARN("flashback query expr should not be subquery", K(ret));
      } else if (OB_FAIL(check_flashback_expr_validity(expr, has_column))) {
        LOG_WARN("failed to check expr validity", K(ret));
      } else if (has_column) {
        ret = OB_ERR_COLUMN_NOT_ALLOWED;
        LOG_WARN("column not allowed here", K(ret), K(*expr));
      } else {
        table_item->flashback_query_expr_ = expr;
        if (T_TABLE_FLASHBACK_QUERY_TIMESTAMP == time_node->type_) {
          table_item->flashback_query_type_ = TableItem::USING_TIMESTAMP;
        } else if (T_TABLE_FLASHBACK_QUERY_SCN == time_node->type_) {
          table_item->flashback_query_type_ = TableItem::USING_SCN;
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("invalid type", K(time_node->type_), K(ret));
        }
      }
    }
    if (OB_SUCC(ret)) {
      // try add cast expr in static typing engine.
      ObRawExpr *&expr = table_item->flashback_query_expr_;
      ObSysFunRawExpr *dst_expr = NULL;
      CK(NULL != expr);
      CK(NULL != params_.expr_factory_);
      OZ(expr->formalize(session_info_));
      const bool use_default_cm = true;
      ObCastMode cm = 0;
      if (TableItem::USING_TIMESTAMP == table_item->flashback_query_type_
          && ObTimestampTZType != expr->get_result_type().get_type()) {
        ObExprResType res_type;
        res_type.set_type(ObTimestampTZType);
        res_type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY2[ORACLE_MODE][ObTimestampTZType]);
        OZ(ObRawExprUtils::create_cast_expr(*params_.expr_factory_, expr, res_type, dst_expr,
                                            session_info_, use_default_cm, cm));
        if (OB_SUCC(ret)) {
          expr = dst_expr;
        }
      } else if (TableItem::USING_SCN == table_item->flashback_query_type_
                 && ObUInt64Type != expr->get_result_type().get_type()) {
        ObExprResType res_type;
        res_type.set_type(ObUInt64Type);
        res_type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY2[ORACLE_MODE][ObUInt64Type]);
        OZ(ObRawExprUtils::create_cast_expr(*params_.expr_factory_, expr, res_type, dst_expr,
                                            session_info_, use_default_cm, cm));
        if (OB_SUCC(ret)) {
          expr = dst_expr;
        }
      }
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid type", K(time_node->type_), K(ret));
  }
  return ret;
}

//针对subquery或者view按照oracle的设置原则，在表已经有相关flashback属性时保持原有的，在没有相关flashback属性时，
//设置为外层给view或者subquery的flashback属性，比如:
// select * from ((select * from t1 as of timestamp time1, t2) as of timestamp time1;
// 这个时候表t1仍保持原有的flashback的时间戳time1，而表t2则设置为外层的flashback时间戳time2
int ObDMLResolver::set_flashback_info_for_view(ObSelectStmt *select_stmt, TableItem *table_item)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObSelectStmt*, 4> child_stmts;
  bool is_stack_overflow = false;
  if (OB_ISNULL(select_stmt) ||OB_ISNULL(table_item) || OB_ISNULL(table_item->flashback_query_expr_)
      || OB_UNLIKELY(table_item->flashback_query_type_ == TableItem::NOT_USING)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected error", K(ret), K(select_stmt), K(table_item));
  } else if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("check stack overflow failed", K(ret));
  } else if (OB_UNLIKELY(is_stack_overflow)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack is overflow", K(ret));
  } else if (OB_FAIL(select_stmt->get_child_stmts(child_stmts))) {
    LOG_WARN("failed to get child stmts", K(ret));
  } else {
    //1.首先设置本层stmt table的flashback属性
    for (int64_t i = 0; OB_SUCC(ret) && i < select_stmt->get_table_size(); ++i) {
      TableItem *cur_table = select_stmt->get_table_item(i);
      if (OB_ISNULL(cur_table)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret), K(cur_table));
      } else if (cur_table->flashback_query_expr_ != NULL &&
                 cur_table->flashback_query_type_ != TableItem::NOT_USING) {
        /*do nothing */
      } else if (cur_table->is_basic_table()) {
        cur_table->flashback_query_expr_ = table_item->flashback_query_expr_;
        cur_table->flashback_query_type_ = table_item->flashback_query_type_;
      } else {/*do nothing*/}
    }
    //2.递归设置子查询的table flashback属性
    for (int64_t i = 0; OB_SUCC(ret) && i < child_stmts.count(); i++) {
      if (OB_FAIL(SMART_CALL(set_flashback_info_for_view(child_stmts.at(i), table_item)))) {
        LOG_WARN("failed to set flashback info for view", K(ret));
      } else {/*do nothing*/}
    }
  }
  return ret;
}

//oracle兼容临时表的数据清理在此进行, 当本session第一次遇到临时表时, 假定session id复用,
//原sessin id的数据未被清理, 此时执行alter system drop tables in session 12345, 最终在rs转换为
//delete from TMP1 where __session_id = 12345 and __sess_create_time <> sess_create_time
//不区分直连和proxy
int ObDMLResolver::resolve_table_drop_oracle_temp_table(TableItem *&table_item)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table_item) || OB_ISNULL(session_info_) || OB_ISNULL(schema_checker_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected NULL pointer", K(table_item), K(session_info_), K(schema_checker_), K(ret));
  } else if (table_item->is_system_table_ || table_item->is_index_table_ || table_item->is_view_table_
             || table_item->is_recursive_union_fake_table_) {
    //do nothing
  } else if (is_oracle_mode() && false == session_info_->get_has_temp_table_flag()) {
    const ObTableSchema *table_schema = NULL;
    if (table_item->is_link_table()) {
      // skip
    } else if (OB_FAIL(schema_checker_->get_table_schema(session_info_->get_effective_tenant_id(), table_item->ref_id_, table_schema))) {
      ret = OB_TABLE_NOT_EXIST;
      LOG_WARN("get table schema failed", K_(table_item->table_name), K(ret));
    } else if (OB_NOT_NULL(table_schema) && table_schema->is_oracle_tmp_table()) {
      if (OB_FAIL(session_info_->drop_reused_oracle_temp_tables())) {
        LOG_WARN("fail to drop reused oracle temporary tables", K(ret));
      } else {
        session_info_->set_has_temp_table_flag();
        LOG_DEBUG("succeed to drop oracle temporary table in case of session id reused",
                  K(session_info_->get_sessid_for_table()));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_table(const ParseNode &parse_tree,
                                 TableItem *&table_item)
{
  int ret = OB_SUCCESS;
  const ParseNode *table_node = &parse_tree;
  const ParseNode *alias_node = NULL;
  const ParseNode *time_node = NULL;
  const ParseNode *transpose_node = NULL;
  ObDMLStmt *stmt = get_stmt();

  if (T_ORG == parse_tree.type_) {
    table_node = parse_tree.children_[0];
    if (parse_tree.num_child_ >= 5) {
      time_node = parse_tree.children_[4];
    }
  } else if (T_ALIAS == parse_tree.type_) {
    table_node = parse_tree.children_[0];
    alias_node = parse_tree.children_[1];
    if (parse_tree.num_child_ >= 6) {
      time_node = parse_tree.children_[5];
    }
    if (parse_tree.num_child_ >= 7) {
      transpose_node = parse_tree.children_[6];
    }
    if (parse_tree.num_child_ >= 8 && OB_NOT_NULL(parse_tree.children_[7])) {
      ret = OB_ERR_PARSER_SYNTAX;
      LOG_WARN("fetch clause can't occur in table attributes", K(ret));
    }
  }
  //兼容oracle行为, flashback query不支持delete/update/insert stmt
  if (OB_SUCC(ret)) {
    if (!stmt->is_select_stmt() && OB_NOT_NULL(time_node)) {
      ret = OB_ERR_FLASHBACK_QUERY_WITH_UPDATE;
      LOG_WARN("snapshot expression not allowed here", K(ret));
    } else {
      switch (table_node->type_) {
      case T_RELATION_FACTOR: {
        if (OB_FAIL(resolve_basic_table(parse_tree, table_item))) {
          LOG_WARN("resolve basic table failed", K(ret));
        }
        break;
      }
      case T_SELECT: {
        bool has_flashback_query = false;
        if (OB_ISNULL(alias_node)) {
          ret = OB_ERR_PARSER_SYNTAX;
          LOG_WARN("generated table must have alias name");
        } else {
          bool tmp_have_same_table = params_.have_same_table_name_;
          params_.have_same_table_name_ = false;
          ObString alias_name(alias_node->str_len_, alias_node->str_value_);
          if (OB_FAIL(resolve_generate_table(*table_node, alias_name, table_item))) {
            LOG_WARN("resolve generate table failed", K(ret));
          } else if (OB_FAIL(resolve_transpose_table(transpose_node, table_item))) {
            LOG_WARN("resolve_transpose_table failed", K(ret));
          } else {
            params_.have_same_table_name_ = tmp_have_same_table;
          }
        }

        if (OB_FAIL(ret)) {
        } else if (!stmt->is_select_stmt() &&
                  OB_FAIL(check_stmt_has_flashback_query(table_item->ref_query_, false, has_flashback_query))) {
          LOG_WARN("failed to find stmt refer to flashback query", K(ret));
        } else if (has_flashback_query) {
          ret = OB_ERR_FLASHBACK_QUERY_WITH_UPDATE;
          LOG_WARN("snapshot expression not allowed here", K(ret));
        } else if (OB_NOT_NULL(time_node)) {
          if (OB_FAIL(resolve_flashback_query_node(time_node, table_item))) {
            LOG_WARN("failed to resolve flashback query node", K(ret));
          //针对子查询的flashback属性需要递归的设置
          } else if (OB_FAIL(set_flashback_info_for_view(table_item->ref_query_, table_item))) {
            LOG_WARN("failed to set flashback info for view", K(ret));
          } else {
            //针对generated table的flashback属性经过set_flashback_info_for_view后,已经没用,为了不影响后续判断
            //这里将其还原为默认值
            table_item->flashback_query_expr_ = NULL;
            table_item->flashback_query_type_ = TableItem::NOT_USING;
          }
        } else {/*do nothing*/}
        break;
      }
      case T_JOINED_TABLE: {
        JoinedTable *root = NULL;
        set_has_ansi_join(true);
        if (OB_FAIL(resolve_joined_table(parse_tree, root))) {
          LOG_WARN("resolve joined table failed", K(ret));
        } else if (OB_FAIL(stmt->add_joined_table(root))) {
          LOG_WARN("add joined table failed", K(ret));
        } else {
          table_item = root;
        }
        break;
      }
      case T_TABLE_COLLECTION_EXPRESSION: {
        if (OB_ISNULL(session_info_)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("invalid argument", K(ret));
        }
        OZ (resolve_function_table_item(*table_node, table_item));
        break;
      }
      default:
        /* won't be here */
        ret = OB_ERR_PARSER_SYNTAX;
        LOG_WARN("Unknown table type", "node_type", table_node->type_);
        break;
      }
      if (OB_SUCC(ret) && OB_FAIL(resolve_table_drop_oracle_temp_table(table_item))) {
        LOG_WARN("drop oracle temporary table failed in resolve table", K(ret), K(*table_item));
      }
      if (OB_SUCC(ret)
       && OB_NOT_NULL(params_.query_ctx_)
       && OB_FAIL(check_table_item_with_gen_col_using_udf(table_item,
                                             params_.query_ctx_->is_table_gen_col_with_udf_))) {
        LOG_WARN("failed to check table item generate column with udf", K(ret), KPC(table_item));
      }
    }
  }

  return ret;
}

int ObDMLResolver::check_table_item_with_gen_col_using_udf(const TableItem *table_item, bool &ans)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table_item)) {
    // do nothing
  } else if (ans) {
    // do nothing, some nested stmt may have check it formerly, do not have to check again
  } else if(OB_ISNULL(schema_checker_) ||
      OB_ISNULL(get_stmt())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("NULL param", K(ret), K(table_item), K(schema_checker_));
  } else if (OB_ISNULL(params_.session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("params_.session_info_ is null", K(ret));
  } else if (table_item->is_generated_table()) {
    // generated table should check it when resolve itself.
    // OZ (SMART_CALL(check_table_item_with_gen_col_using_udf(table_item->view_base_item_, ans)), KPC(table_item));
  } else if (table_item->is_basic_table() || table_item->is_fake_cte_table()) {
    /**
     * CTE_TABLE is same as BASIC_TABLE or ALIAS_TABLE
     */
    const ObTableSchema *table_schema = NULL;
    if (OB_FAIL(schema_checker_->get_table_schema(params_.session_info_->get_effective_tenant_id(), table_item->ref_id_, table_schema, table_item->is_link_table()))) {
      /**
       * Should not return OB_TABLE_NOT_EXIST.
       * Because tables have been checked in resolve_table already.
       */
      LOG_WARN("get table schema failed", K(ret));
    } else if (OB_ISNULL(table_schema)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get NULL table schema", K(ret));
    } else if (OB_FAIL(table_schema->has_generated_column_using_udf_expr(ans))){
      LOG_WARN("failed to get using udf expr flag", K(ret));
    }
  } else {
    // do nothing
  }
  return ret;
}

int ObDMLResolver::check_stmt_has_flashback_query(ObDMLStmt *stmt, bool check_all, bool &has_fq)
{
  int ret = OB_SUCCESS;
  bool is_stack_overflow = false;
  ObSEArray<ObSelectStmt*, 4> child_stmts;
  has_fq = false;
  if (OB_ISNULL(stmt)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(stmt));
  } else if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("check stack overflow failed", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack is overflow", K(ret));
  } else if (check_all && OB_FAIL(stmt->get_child_stmts(child_stmts))) {
    LOG_WARN("failed to get child stmts", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && !has_fq && i < stmt->get_table_size(); ++i) {
      TableItem *cur_table = stmt->get_table_item(i);
      if (OB_ISNULL(cur_table)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret), K(cur_table));
      } else if (OB_NOT_NULL(cur_table->flashback_query_expr_)) {
        has_fq = true;
      } else if ((cur_table->is_generated_table() || cur_table->is_temp_table()) && !check_all &&
                 OB_FAIL(SMART_CALL(check_stmt_has_flashback_query(cur_table->ref_query_,
                                                                   check_all, has_fq)))) {
        LOG_WARN("failed to find stmt refer to flashback query", K(ret));
      } else {/*do nothing*/}
    }
    //需要整个查询是否含有flashback属性
    if (check_all) {
      for (int64_t i = 0; OB_SUCC(ret) && !has_fq && i < child_stmts.count(); i++) {
        if (OB_FAIL(SMART_CALL(check_stmt_has_flashback_query(child_stmts.at(i),
                                                              check_all,
                                                              has_fq)))) {
          LOG_WARN("failed to check stmt has flashback query", K(ret));
        } else {/*do nothing*/}
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_joined_table(const ParseNode &parse_node, JoinedTable *&joined_table)
{
  int ret = OB_SUCCESS;
  ParseNode *condition_node = parse_node.children_[3];
  ParseNode *attr_node = parse_node.children_[4];
  if (OB_FAIL(resolve_joined_table_item(parse_node, joined_table))) {
    LOG_WARN("resolve joined table item failed", K(ret));
  } else if (OB_FAIL(join_infos_.push_back(ResolverJoinInfo(joined_table->table_id_)))) {
    LOG_WARN("fail to push back join information", K(ret));
  } else {
    column_namespace_checker_.add_current_joined_table(joined_table);
  }
  if (OB_FAIL(ret)) {
    // do noting;
  } else if (NULL != attr_node && T_NATURAL_JOIN == attr_node->type_) {
    if (OB_FAIL(fill_same_column_to_using(joined_table))) {
      LOG_WARN("failed to fill same columns", K(ret));
    } else if (OB_FAIL(transfer_using_to_on_expr(joined_table))) {
      LOG_WARN("failed to transfer using to on expr", K(ret));
    }
  } else if (condition_node != NULL) {
    if (T_COLUMN_LIST == condition_node->type_) {
      ResolverJoinInfo *join_info = NULL;
      if (!get_joininfo_by_id(joined_table->table_id_, join_info)) {
        LOG_WARN("fail to get join infos", K(ret));
      } else if (OB_FAIL(resolve_using_columns(*condition_node, join_info->using_columns_))) {
        LOG_WARN("resolve using column failed", K(ret));
      } else if (OB_FAIL(transfer_using_to_on_expr(joined_table))) {
        LOG_WARN("transfer using to on expr failed", K(ret));
      }
    } else {
      //transform join on condition
      ObStmtScope old_scope = current_scope_;
      current_scope_ = T_ON_SCOPE;
      if (OB_FAIL(resolve_and_split_sql_expr_with_bool_expr(*condition_node,
                                                      joined_table->join_conditions_))) {
        LOG_WARN("resolve and split sql expr failed", K(ret));
      } else { /*do nothing*/ }
      current_scope_ = old_scope;
    }
  }
  return ret;
}

int ObDMLResolver::resolve_using_columns(const ParseNode &using_node, ObIArray<ObString> &column_names)
{
  int ret = OB_SUCCESS;
  ObString column_name;
  for (int32_t i = 0; OB_SUCCESS == ret && i < using_node.num_child_; ++i) {
    const ParseNode *child_node = NULL;
    if (OB_ISNULL(child_node = using_node.children_[i])) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("child node is null");
    } else {
      // will check using column in tansfer using expr
      column_name.assign_ptr(const_cast<char *>(child_node->str_value_), static_cast<int32_t>(child_node->str_len_));
      if (lib::is_oracle_mode()
          && ObCharset::case_insensitive_equal(OB_HIDDEN_LOGICAL_ROWID_COLUMN_NAME, column_name)) {
        ret = OB_ERR_ONLY_SIMPLE_COLUMN_NAME_ALLOWED;
        LOG_WARN("only simple column names allowed here", K(ret));
      } else {
        // filter out duplicated column
        bool found = false;
        for (int64_t j = 0; j < column_names.count(); j++) {
          if (ObCharset::case_insensitive_equal(column_names.at(j), column_name)) {
            found = true;
            break;
          }
        }
        if (!found) {
          if (OB_FAIL(column_names.push_back(column_name))) {
            LOG_WARN("Add column name failed", K(ret));
          }
        }
      }
    }
  }
  return ret;
}

// major logic is refered to
// - resolve_and_split_sql_expr();
// - resolve_sql_expr();
//  As the left_table might be nested, but the right_table not,
//  therefore, the left_table should only have unique column_names
//  to avoid ambiguous column resolving.
//
int ObDMLResolver::transfer_using_to_on_expr(JoinedTable *&joined_table)
{
  int ret = OB_SUCCESS;
  ObArray<ObQualifiedName> columns;
  JoinedTable *cur_table = joined_table;
  if (OB_ISNULL(params_.expr_factory_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("params is invalid", K_(params_.expr_factory));
  }
  // construct AND expr and columns in exprs
  ResolverJoinInfo *join_info = NULL;
  bool found = get_joininfo_by_id(joined_table->table_id_, join_info);
  for (int64_t i = 0; OB_SUCC(ret) && found && i < join_info->using_columns_.count(); ++i) {
    // construct t_left_N.ck = t_right.ck
    ObOpRawExpr *b_expr = NULL;
    ObRawExpr *left_expr = NULL;
    ObRawExpr *right_expr = NULL;

    // make two sub exprs: t_left_N.ck and t_right.ck
    const TableItem *left_table = NULL;
    const TableItem *right_table = NULL;
    const ObString &column_name = join_info->using_columns_.at(i);
    if (OB_FAIL(column_namespace_checker_.check_using_column_namespace(column_name, left_table, right_table))) {
      LOG_WARN("check using column namespace failed", K(column_name));
      if (OB_ERR_BAD_FIELD_ERROR == ret) {
        ObString scope_name = ObString::make_string(get_scope_name(current_scope_));
        LOG_USER_ERROR(OB_ERR_BAD_FIELD_ERROR, column_name.length(), column_name.ptr(), scope_name.length(), scope_name.ptr());
      } else if (OB_NON_UNIQ_ERROR == ret) {
        ObString scope_name = ObString::make_string(get_scope_name(current_scope_));
        LOG_USER_ERROR(OB_NON_UNIQ_ERROR, column_name.length(), column_name.ptr(), scope_name.length(), scope_name.ptr());
      }
    } else if (left_table->is_joined_table()) {
      if (OB_FAIL(resolve_join_table_column_item(static_cast<const JoinedTable&>(*left_table),
                                                 column_name, left_expr))) {
        LOG_WARN("resolve join table column item failed", K(ret), K(column_name));
      }
    } else {
      ColumnItem *column = NULL;
      if (OB_FAIL(resolve_single_table_column_item(*left_table, column_name, false, column))) {
        LOG_WARN("resolve single table column item failed", K(ret), K(column_name));
      } else {
        left_expr = column->expr_;
      }
    }
    if (OB_SUCC(ret)) {
      if (right_table->is_joined_table()) {
        if (OB_FAIL(resolve_join_table_column_item(static_cast<const JoinedTable&>(*right_table),
                                                   column_name, right_expr))) {
          LOG_WARN("resolve join table column item failed", K(ret), K(column_name));
        }
      } else {
        ColumnItem *column = NULL;
        if (OB_FAIL(resolve_single_table_column_item(*right_table, column_name, false, column))) {
          LOG_WARN("resolve single table column item failed", K(ret), K(column_name));
        } else {
          right_expr = column->expr_;
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_OP_EQ, b_expr))) { // make equal expr: t_left_N.ck = t_right.ck
        LOG_WARN("b_expr is null", K(ret));
      } else if (OB_ISNULL(b_expr)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("b_expr is null");
      } else if (OB_FAIL(b_expr->set_param_exprs(left_expr, right_expr))) {
        LOG_WARN("set b_expr param exprs failed", K(ret));
      } else if (OB_FAIL(b_expr->formalize(session_info_))) {
        LOG_WARN("resolve formalize expression", K(ret));
      } else if (OB_FAIL(cur_table->join_conditions_.push_back(b_expr))) {
        LOG_WARN("Add expression error", K(ret));
      }
    }
    if (OB_SUCC(ret) && FULL_OUTER_JOIN == cur_table->joined_type_) {
      ObSysFunRawExpr *coalesce_expr = NULL;
      if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_FUN_SYS_COALESCE, coalesce_expr))) {
        LOG_WARN("create raw expr failed", K(ret));
      } else if (OB_FAIL(coalesce_expr->set_param_exprs(left_expr, right_expr))) {
        LOG_WARN("set coalesce expr child failed", K(ret));
      } else if (OB_FAIL(coalesce_expr->formalize(session_info_))) {
        LOG_WARN("formalize coalesce expr failed", K(ret));
      } else if (OB_NOT_NULL(join_info) && OB_FAIL(join_info->coalesce_expr_.push_back(coalesce_expr))) {
        LOG_WARN("push expr to coalesce failed", K(ret));
      }
    }
  } // end of for

  return ret;
}

//transfer (t1,t2) join (t3,t4) to (t1 join t2) join (t3 join t4)
int ObDMLResolver::transfer_to_inner_joined(const ParseNode &parse_node, JoinedTable *&joined_table)
{
  int ret = OB_SUCCESS;
  ParseNode *table_node = NULL;
  JoinedTable *cur_table = NULL;
  JoinedTable *child_table = NULL;
  JoinedTable *temp_table = NULL;
  TableItem *table_item = NULL;
  for (int64_t j = 0; OB_SUCC(ret) && j < parse_node.num_child_; j++) {
    if (0 == j) {
      if (OB_FAIL(alloc_joined_table_item(cur_table))) {
        LOG_WARN("create joined table item failed", K(ret));
      } else {
        cur_table->table_id_ = generate_table_id();
        cur_table->type_ = TableItem::JOINED_TABLE;
        if (OB_FAIL(join_infos_.push_back(ResolverJoinInfo(cur_table->table_id_)))) {
          LOG_WARN("fail to push back join information", K(ret));
        }
      }
    }
    table_node = parse_node.children_[j];
    if (OB_SUCC(ret)) {
      if (T_JOINED_TABLE == table_node->type_) {
        if (OB_FAIL(resolve_joined_table(*table_node, child_table))) {
          LOG_WARN("resolve child joined table failed", K(ret));
        } else if (0 == j) {
          cur_table->left_table_ = child_table;
        } else {
          cur_table->right_table_ = child_table;
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < child_table->single_table_ids_.count(); ++i) {
          uint64_t child_table_id = child_table->single_table_ids_.at(i);
          if (OB_FAIL(cur_table->single_table_ids_.push_back(child_table_id))) {
            LOG_WARN("push back child_table_id failed", K(ret));
          }
        }
      } else {
        if (OB_FAIL(resolve_table(*table_node, table_item))) {
          LOG_WARN("resolve table failed", K(ret));
        } else if (0 == j) {
          cur_table->left_table_ = table_item;
        } else {
          cur_table->right_table_ = table_item;
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(cur_table->single_table_ids_.push_back(table_item->table_id_))) {
            LOG_WARN("push back child table id failed", K(ret));
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      cur_table->joined_type_ = INNER_JOIN;
      if (j != 0 && j != parse_node.num_child_ - 1) {
        if (OB_FAIL(alloc_joined_table_item(temp_table))) {
          LOG_WARN("create joined table item failed", K(ret));
        } else {
          temp_table->table_id_ = generate_table_id();
          temp_table->type_ = TableItem::JOINED_TABLE;
          temp_table->left_table_ = cur_table;
          cur_table = temp_table;
          if (OB_FAIL(join_infos_.push_back(ResolverJoinInfo(cur_table->table_id_)))) {
            LOG_WARN("fail to push back join information", K(ret));
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    joined_table = cur_table;
  }
  return ret;
}

//resolve table column reference
//select&update&delete stmt can access joined table column, generated table column or base table column
int ObDMLResolver::resolve_table_column_expr(const ObQualifiedName &q_name, ObRawExpr *&real_ref_expr)
{
  //search order
  //1. joined table column
  //2. basic table column or generated table column
  int ret = OB_SUCCESS;
  if (OB_ISNULL(session_info_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("session info is null", K_(session_info));
  } else {
    const TableItem *table_item = NULL;
    if (OB_FAIL(column_namespace_checker_.check_table_column_namespace(q_name, table_item))) {
      LOG_WARN_IGNORE_COL_NOTFOUND(ret, "column don't found in table", K(ret), K(q_name));
    } else if (table_item->is_joined_table()) {
      const JoinedTable &joined_table = static_cast<const JoinedTable&>(*table_item);
      if (OB_FAIL(resolve_join_table_column_item(joined_table, q_name.col_name_, real_ref_expr))) {
        LOG_WARN("resolve join table column item failed", K(ret));
      }
    } else {
      ColumnItem *col_item = NULL;
      if (OB_FAIL(resolve_single_table_column_item(*table_item, q_name.col_name_, false, col_item))) {
        LOG_WARN("resolve single table column item failed", K(ret));
      } else if (OB_ISNULL(col_item)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("col item is null", K(ret));
      } else {
        real_ref_expr = col_item->expr_;
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_single_table_column_item(const TableItem &table_item,
                                                    const ObString &column_name,
                                                    bool include_hidden,
                                                    ColumnItem *&col_item)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  if (OB_ISNULL(stmt) || OB_ISNULL(schema_checker_) || OB_ISNULL(params_.expr_factory_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema checker is null", K(stmt), K_(schema_checker), K_(params_.expr_factory));
  } else if (table_item.is_basic_table() || table_item.is_fake_cte_table()) {
    if (OB_FAIL(resolve_basic_column_item(table_item, column_name, include_hidden, col_item))) {
      LOG_WARN("resolve basic column item failed", K(ret));
    } else { /*do nothing*/ }
  } else if (table_item.is_generated_table() || table_item.is_temp_table()) {
    if (OB_FAIL(resolve_generated_table_column_item(table_item, column_name, col_item))) {
      LOG_WARN("resolve generated table column failed", K(ret));
    }
  } else if (table_item.is_function_table()) {
    if (OB_FAIL(resolve_function_table_column_item(table_item, column_name, col_item))) {
      LOG_WARN("resolve function table column failed", K(ret), K(column_name));
    }
  }
  return ret;
}

int ObDMLResolver::resolve_join_table_column_item(const JoinedTable &joined_table,
                                                  const ObString &column_name,
                                                  ObRawExpr *&real_ref_expr)
{
  int ret = OB_SUCCESS;
  ObRawExpr *coalesce_expr = NULL;
  if (OB_UNLIKELY(joined_table.joined_type_ != FULL_OUTER_JOIN)) {
    //only when column name hit full join table using name, we would search column expr in joined table
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("joined table type is unexpected", K(ret));
  } else {
    ResolverJoinInfo *join_info = NULL;
    if (get_joininfo_by_id(joined_table.table_id_, join_info) && join_info->coalesce_expr_.count() > 0) {
      for (int i = 0; i < join_info->coalesce_expr_.count(); i++) {
        if (ObCharset::case_insensitive_equal(join_info->using_columns_.at(i), column_name)) {
          coalesce_expr = join_info->coalesce_expr_.at(i);
          break;
        }
      }
    }
    if (NULL == coalesce_expr) {
      ret = OB_ERR_BAD_FIELD_ERROR;
      LOG_DEBUG("full join table using name can't be found", K(column_name));
    } else {
      real_ref_expr = coalesce_expr;
    }
  }
  return ret;
}

int ObDMLResolver::resolve_joined_table_item(const ParseNode &parse_node, JoinedTable *&joined_table)
{
  int ret = OB_SUCCESS;
  ParseNode *table_node = NULL;
  JoinedTable *cur_table = NULL;
  JoinedTable *child_table = NULL;
  TableItem *table_item = NULL;
  ObSelectStmt *select_stmt = static_cast<ObSelectStmt*>(stmt_);

  if (OB_ISNULL(select_stmt) || OB_UNLIKELY(parse_node.type_ != T_JOINED_TABLE)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(select_stmt), K_(parse_node.type));
  } else if (OB_FAIL(alloc_joined_table_item(cur_table))) {
    LOG_WARN("create joined table item failed", K(ret));
  } else {
    cur_table->table_id_ = generate_table_id();
    cur_table->type_ = TableItem::JOINED_TABLE;
  }
  /* resolve table */
  for (uint64_t i = 1; OB_SUCC(ret) && i <= 2; i++) {
    table_node = parse_node.children_[i];
    // nested join case or normal join case
    if (T_JOINED_TABLE == table_node->type_) {
      if (OB_FAIL(resolve_joined_table(*table_node, child_table))) {
        LOG_WARN("resolve child joined table failed", K(ret));
      } else if (1 == i) {
        cur_table->left_table_ = child_table;
      } else {
        cur_table->right_table_ = child_table;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < child_table->single_table_ids_.count(); ++i) {
        uint64_t child_table_id = child_table->single_table_ids_.at(i);
        if (OB_FAIL(cur_table->single_table_ids_.push_back(child_table_id))) {
          LOG_WARN("push back child_table_id failed", K(ret));
        }
      }
    } else if (T_TABLE_REFERENCES == table_node->type_) {
      if (OB_FAIL(transfer_to_inner_joined(*table_node, child_table))) {
        LOG_WARN("transfer to inner join failed", K(ret));
      } else if (1 == i) {
        cur_table->left_table_ = child_table;
      } else {
        cur_table->right_table_ = child_table;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < child_table->single_table_ids_.count(); ++i) {
        uint64_t child_table_id = child_table->single_table_ids_.at(i);
        if (OB_FAIL(cur_table->single_table_ids_.push_back(child_table_id))) {
          LOG_WARN("push back child_table_id failed", K(ret));
        }
      }
    } else {
      /*
       * 对于recursive cte来说，如果union all右支是个join，
       * cte不能出现在right join的左面，不能出现在left join的右边，不能使用full join，
       * 可以出现在inner join的两边
       * */
      if (1 == i) {
        column_namespace_checker_.add_current_joined_table(NULL);
      }
      if (OB_FAIL(resolve_table(*table_node, table_item))) {
        LOG_WARN("resolve table failed", K(ret));
      } else if (OB_FAIL(check_special_join_table(*table_item, 1 == i, parse_node.children_[0]->type_))) {
        LOG_WARN("check special join table failed", K(ret), K(i));
      } else if (1 == i) {
        cur_table->left_table_ = table_item;
        column_namespace_checker_.add_current_joined_table(table_item);
      } else {
        cur_table->right_table_ = table_item;
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(cur_table->single_table_ids_.push_back(table_item->table_id_))) {
          LOG_WARN("push back child table id failed", K(ret));
        }
      }
    }
  }

  /* resolve join type */
  if (OB_SUCC(ret)) {
    switch (parse_node.children_[0]->type_) {
    case T_JOIN_FULL:
      cur_table->joined_type_ = FULL_OUTER_JOIN;
      break;
    case T_JOIN_LEFT:
      cur_table->joined_type_ = LEFT_OUTER_JOIN;
      break;
    case T_JOIN_RIGHT:
      cur_table->joined_type_ = RIGHT_OUTER_JOIN;
      break;
    case T_JOIN_INNER:
      cur_table->joined_type_ = INNER_JOIN;
      break;
    default:
      /* won't be here */
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("Unknown table type in outer join", K_(table_node->type));
      break;
    }
  }
  if (OB_SUCC(ret)) {
    joined_table = cur_table;
  }

  return ret;
}

int ObDMLResolver::check_special_join_table(const TableItem &join_table, bool is_left_child, ObItemType join_type)
{
  UNUSED(is_left_child);
  UNUSED(join_type);
  int ret = OB_SUCCESS;
  if (join_table.is_fake_cte_table()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "CTE in dml");
  }
  return ret;
}

int ObDMLResolver::resolve_generate_table(const ParseNode &table_node,
                                          const ObString &alias_name,
                                          TableItem *&table_item)
{
  int ret = OB_SUCCESS;
  ObSelectResolver select_resolver(params_);
  //from子查询和当前查询属于平级，因此current level和当前保持一致
  select_resolver.set_current_level(current_level_);
  select_resolver.set_current_view_level(current_view_level_);
  select_resolver.set_parent_namespace_resolver(parent_namespace_resolver_);
  if (OB_FAIL(select_resolver.add_parent_gen_col_exprs(gen_col_exprs_))) {
    LOG_WARN("failed to add gen col exprs", K(ret));
  } else if (OB_FAIL(do_resolve_generate_table(table_node, alias_name, select_resolver, table_item))) {
    LOG_WARN("do resolve generated table failed", K(ret));
  }
  return ret;
}

int ObDMLResolver::do_resolve_generate_table(const ParseNode &table_node,
                                             const ObString &alias_name,
                                             ObChildStmtResolver &child_resolver,
                                             TableItem *&table_item)
{
  int ret = OB_SUCCESS;
  ObSelectStmt *ref_stmt = NULL;
   /*oracle模式允许sel/upd/del stmt中的generated table含有重复列，只要外层没有引用到重复列就行，同时对于外层引用
  * 到的列是否为重复列会在检查column时进行检测，eg: select 1 from (select c1,c1 from t1);
  * 因此对于oracle模式下sel/upd/del stmt进行检测时，检测到重复列时只需skip，但是仍然需要添加相关plan cache约束
  * https://work.aone.alibaba-inc.com/issue/29799516
   */
  bool can_skip = (lib::is_oracle_mode() && get_stmt()->is_sel_del_upd());
  if (OB_FAIL(child_resolver.resolve_child_stmt(table_node))) {
    LOG_WARN("resolve select stmt failed", K(ret));
  } else if (OB_ISNULL(ref_stmt = child_resolver.get_child_stmt()) || OB_ISNULL(get_stmt())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("resolve select stmt failed", K(ret));
  } else if (lib::is_oracle_mode() && ref_stmt->has_for_update()) {
    ret = OB_ERR_PARSER_SYNTAX;
    LOG_WARN("for update not allowed in from clause", K(ret));
  } else if (OB_FAIL(ObResolverUtils::check_duplicated_column(*ref_stmt, can_skip))) {
    // check duplicate column name for genereated table
    LOG_WARN("check duplicated column failed", K(ret));
  } else if (OB_FAIL(resolve_generate_table_item(ref_stmt, alias_name, table_item))) {
    LOG_WARN("resolve generate table item failed", K(ret));
  } else {
    LOG_DEBUG("finish do_resolve_generate_table", K(alias_name), KPC(table_item),
                                                  KPC(table_item->ref_query_));
  }
  return ret;
}

int ObDMLResolver::resolve_generate_table_item(ObSelectStmt *ref_query,
                                               const ObString &alias_name,
                                               TableItem *&tbl_item)
{
  int ret = OB_SUCCESS;
  TableItem *item = NULL;
  ObDMLStmt *dml_stmt = get_stmt();
  if (OB_ISNULL(dml_stmt) || OB_ISNULL(allocator_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("resolver isn't init");
  } else if (OB_UNLIKELY(NULL == (item = dml_stmt->create_table_item(*allocator_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("create table item failed");
  } else {
    item->ref_query_ = ref_query;
    item->table_id_ = generate_table_id();
    item->table_name_ = alias_name;
    item->alias_name_ = alias_name;
    item->type_ = TableItem::GENERATED_TABLE;
    item->is_view_table_ = false;
    if (OB_FAIL(dml_stmt->add_table_item(session_info_, item, params_.have_same_table_name_))) {
      LOG_WARN("add table item failed", K(ret));
    } else {
      tbl_item = item;
    }
  }
  return ret;
}

int ObDMLResolver::resolve_function_table_item(const ParseNode &parse_tree,
                                               TableItem *&tbl_item)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  ParseNode *alias_node = NULL;
  ObRawExpr *function_table_expr = NULL;
  TableItem *item = NULL;
  ObString alias_name;
  CK (OB_LIKELY(T_TABLE_COLLECTION_EXPRESSION == parse_tree.type_));
  CK (OB_LIKELY(2 == parse_tree.num_child_));
  CK (OB_NOT_NULL(parse_tree.children_[0]));
  if (OB_SUCC(ret) && (OB_ISNULL(stmt) || OB_ISNULL(allocator_))) {
    ret = OB_NOT_INIT;
    LOG_WARN("resolver isn't init", K(ret), K(stmt), K(allocator_));
  }
  if (OB_SUCC(ret)) {
    OX (params_.is_resolve_table_function_expr_ = true);
    OZ (resolve_sql_expr(*(parse_tree.children_[0]), function_table_expr));
    OX (params_.is_resolve_table_function_expr_ = false);
    CK (OB_NOT_NULL(function_table_expr));
    OX (alias_node = parse_tree.children_[1]);
  }
  CK (OB_NOT_NULL(function_table_expr));
  OZ (function_table_expr->deduce_type(session_info_));
  if (OB_SUCC(ret)) {
    if (!function_table_expr->get_result_type().is_ext()) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("ORA-22905: cannot access rows from a non-nested table item",
               K(ret), K(function_table_expr->get_result_type()));
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "access rows from a non-nested table item");
    } else {
      CK(OB_NOT_NULL(schema_checker_));
      if (OB_SUCC(ret)) {
        ObPLPackageGuard package_guard(params_.session_info_->get_effective_tenant_id());
        const ObUserDefinedType *user_type = NULL;
        CK (OB_NOT_NULL(params_.schema_checker_));
        OZ (ObResolverUtils::get_user_type(
          params_.allocator_, params_.session_info_, params_.sql_proxy_,
          params_.schema_checker_->get_schema_guard(),
          package_guard,
          function_table_expr->get_udt_id(),
          user_type));
        if (OB_FAIL(ret)) {
        } else if (OB_UNLIKELY(NULL == user_type)) {
          ret = OB_ERR_WRONG_FUNC_ARGUMENTS_TYPE;
          LOG_WARN("Can not found User Defined Type",
                   K(ret), K(function_table_expr->get_udt_id()), KPC(user_type));
          LOG_USER_ERROR(OB_ERR_WRONG_FUNC_ARGUMENTS_TYPE, 14, "TABLE FUNCTION");
        } else if (!user_type->is_collection_type()) {
          ret = OB_NOT_SUPPORTED;
          LOG_WARN("ORA-22905: cannot access rows from a non-nested table item",
                   K(ret), K(function_table_expr->get_result_type()));
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "access rows from a non-nested table item");
        } else { /*do nothing*/ }
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_ISNULL(item = stmt->create_table_item(*allocator_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to create table item", K(ret));
    } else if (NULL != alias_node) {
      alias_name.assign_ptr((char *)(alias_node->str_value_), static_cast<int32_t>(alias_node->str_len_));
    } else if (NULL == alias_node) {
      if (OB_FAIL(stmt->generate_func_table_name(*allocator_, alias_name))) {
        LOG_WARN("failed to generate func table name", K(ret));
      }
    }
    OX (item->table_name_ = alias_name);
    OX (item->alias_name_ = alias_name);
    OX (item->table_id_ = generate_table_id());
    OX (item->type_ = TableItem::FUNCTION_TABLE);
    OX (item->function_table_expr_ = function_table_expr);

    if (OB_SUCC(ret) && function_table_expr->is_udf_expr()) {
      ObUDFRawExpr *udf = static_cast<ObUDFRawExpr*>(function_table_expr);
      ObSchemaObjVersion table_version;
      CK (OB_NOT_NULL(udf));
      if (OB_SUCC(ret) && udf->need_add_dependency()) {
        OZ (udf->get_schema_object_version(table_version));
        OZ (stmt->add_global_dependency_table(table_version));
      }
    }
  }
  OZ (stmt->add_table_item(session_info_, item));
  if (OB_SUCC(ret)) {
    // https://work.aone.alibaba-inc.com/issue/31120239
    // ObFunctionTable填充行数据时依赖row前面的列是udf的输出列, 这里强制将udf的输出列加到ObFunctionTable
    ObSEArray<ColumnItem, 16> col_items;
    CK (OB_NOT_NULL(item));
    OZ (resolve_function_table_column_item(*item, col_items));
  }
  OX (tbl_item = item);
  return ret;
}

int ObDMLResolver::resolve_base_or_alias_table_item_normal(uint64_t tenant_id,
                                                           const ObString &db_name,
                                                           const bool &is_db_explicit,
                                                           const ObString &tbl_name,
                                                           const ObString &alias_name,
                                                           const ObString &synonym_name,
                                                           const ObString &synonym_db_name,
                                                           TableItem *&tbl_item,
                                                           bool cte_table_fisrt)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  TableItem *item = NULL;
  const ObTableSchema *tschema = NULL;
  if (OB_ISNULL(stmt) || OB_ISNULL(schema_checker_) || OB_ISNULL(session_info_) || OB_ISNULL(allocator_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("resolver isn't init", K(stmt), K_(schema_checker), K_(session_info), K_(allocator));
  } else if (OB_UNLIKELY(NULL == (item = stmt->create_table_item(*allocator_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("create table item failed");
  } else if (alias_name.length() > 0) {
    item->type_ = TableItem::ALIAS_TABLE;
  } else {
    item->type_ = TableItem::BASE_TABLE;
  }
  if (OB_SUCC(ret)) {
    item->synonym_name_ = synonym_name;
    if (is_db_explicit) {
      item->synonym_db_name_ = synonym_db_name;
    }
    item->database_name_ = db_name;
    bool select_index_enabled = false;
    uint64_t database_id = OB_INVALID_ID;
    const bool is_hidden = session_info_->is_table_name_hidden();
    if (OB_FAIL(session_info_->is_select_index_enabled(select_index_enabled))) {
      LOG_WARN("get select index status failed", K(ret));
    } else if (OB_FAIL(schema_checker_->get_database_id(tenant_id, db_name, database_id))) {
      LOG_WARN("get database id failed", K(ret));
    } else if (OB_FAIL(schema_checker_->get_table_schema(tenant_id,
                                                         database_id,
                                                         tbl_name,
                                                         false /*data table first*/,
                                                         cte_table_fisrt,
                                                         is_hidden,
                                                         tschema))) {
      if (OB_TABLE_NOT_EXIST == ret && ((stmt->is_select_stmt() && select_index_enabled) || session_info_->get_ddl_info().is_ddl())) {
        if (OB_FAIL(schema_checker_->get_table_schema(tenant_id,
                                                      database_id,
                                                      tbl_name,
                                                      true /* for index table */,
                                                      cte_table_fisrt,
                                                      is_hidden,
                                                      tschema))) {
          LOG_WARN("table or index doesn't exist", K(tenant_id), K(database_id), K(tbl_name), K(ret));
        }
      } else {
        LOG_WARN("table or index get schema failed", K(ret));
      }
    }
    
    // restrict accessible virtual table can not be use in sys tenant or sys view.
    if (OB_SUCC(ret)
        && tschema->is_vir_table()
        && is_restrict_access_virtual_table(tschema->get_table_id())
        && OB_SYS_TENANT_ID != session_info_->get_effective_tenant_id()) {
      bool in_sysview = false;
      if (OB_FAIL(check_in_sysview(in_sysview))) {
        LOG_WARN("check in sys view failed", K(ret));
      } else {
        if (!in_sysview) {
          ret = OB_TABLE_NOT_EXIST;
          LOG_WARN("restrict accessible virtual table can not access directly",
              K(ret), K(db_name), K(tbl_name));
          LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(db_name), to_cstring(tbl_name));
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (session_info_->get_ddl_info().is_ddl()) {
        if (!tschema->is_storage_local_index_table()) {
          item->ref_id_ = tschema->get_table_id();
          item->table_id_ = tschema->get_table_id();
          item->is_system_table_ = tschema->is_sys_table();
          item->is_view_table_ = tschema->is_view_table();
          item->type_ = TableItem::BASE_TABLE;
          item->table_name_ = tschema->get_table_name_str();
          item->alias_name_ = tschema->get_table_name_str();
          item->ddl_schema_version_ = tschema->get_schema_version();
          item->ddl_table_id_ = tschema->get_table_id();
        } else {
          const ObTableSchema *tab_schema = nullptr;
          if (OB_FAIL(schema_checker_->get_table_schema(session_info_->get_effective_tenant_id(), tschema->get_data_table_id(), tab_schema))) {
            LOG_WARN("get data table schema failed", K(ret), K_(item->ref_id));
          } else {
            item->ref_id_ = tab_schema->get_table_id();
            item->table_id_ = tab_schema->get_table_id();
            item->is_system_table_ = tab_schema->is_sys_table();
            item->is_view_table_ = tab_schema->is_view_table();
            item->type_ = TableItem::BASE_TABLE;
            item->table_name_ = tab_schema->get_table_name_str();
            item->alias_name_ = tab_schema->get_table_name_str();
            item->ddl_schema_version_ = tschema->get_schema_version();
            item->ddl_table_id_ = tschema->get_table_id();
          }
        }
        if (OB_SUCC(ret)) {
          // sql used by foreign key checking ddl may have more than one table items refering to the same table
          if (item->ref_id_ == OB_INVALID_ID) {
            ret = OB_TABLE_NOT_EXIST;
            LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(db_name), to_cstring(tbl_name));
          } else if (TableItem::BASE_TABLE == item->type_) {
            bool is_exist = false;
            if (OB_FAIL(check_table_id_exists(item->ref_id_, is_exist))) {
              LOG_WARN("check table id exists failed", K_(item->ref_id));
            } else if (is_exist) {
              //in the whole query stmt, table exists in the other query layer, so subquery must alias it
              //implicit alias table, alias name is table name
              item->table_id_ = generate_table_id();
              item->type_ = TableItem::ALIAS_TABLE;
              if (!synonym_name.empty()) {
                // bug: 31827906
                item->alias_name_ = synonym_name;
                item->database_name_ = synonym_db_name;
              } else {
                item->alias_name_ = tbl_name;
              }
            } else {
              //base table, no alias name
              item->table_id_ = generate_table_id();
            }
          } else {
            item->table_id_ = generate_table_id();
            item->alias_name_ = alias_name;
          }
        }
      } else if (tschema->is_index_table() || tschema->is_materialized_view()) {
        //feature: select * from index_name where... rewrite to select index_col1, index_col2... from data_table_name where...
        //the feature only use in mysqtest case, not open for user
        const ObTableSchema *tab_schema = NULL;
        item->is_index_table_ = true;

        item->ref_id_ = tschema->get_table_id();
        item->table_id_ = generate_table_id();
        item->type_ = TableItem::ALIAS_TABLE;
        //主表schema
        if (OB_FAIL(schema_checker_->get_table_schema(session_info_->get_effective_tenant_id(), tschema->get_data_table_id(), tab_schema, item->is_link_table()))) {
          LOG_WARN("get data table schema failed", K(ret), K_(item->ref_id));
        } else {
          item->table_name_ = tab_schema->get_table_name_str(); //主表的名字
          item->alias_name_ = tschema->get_table_name_str(); //将索引名作为主表的alias name
          //如果是查索引表，需要将主表的依赖也要加入到plan中
          ObSchemaObjVersion table_version;
          table_version.object_id_ = tab_schema->get_table_id();
          table_version.object_type_ = DEPENDENCY_TABLE;
          table_version.version_ = tab_schema->get_schema_version();
          table_version.is_db_explicit_ = is_db_explicit;
          if (common::is_cte_table(table_version.object_id_)) {
            // do nothing
          } else if (OB_FAIL(stmt->add_global_dependency_table(table_version))) {
            LOG_WARN("add global dependency table failed", K(ret));
          }
        }
      } else {
        item->ref_id_ = tschema->get_table_id();
        item->table_name_ = tbl_name;
        item->is_system_table_ = tschema->is_sys_table();
        item->is_view_table_ = tschema->is_view_table();
        item->ddl_schema_version_ = tschema->get_schema_version();
        if (item->ref_id_ == OB_INVALID_ID) {
          ret = OB_TABLE_NOT_EXIST;
          LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(db_name), to_cstring(tbl_name));
        } else if (TableItem::BASE_TABLE == item->type_) {
          bool is_exist = false;
          if (OB_FAIL(check_table_id_exists(item->ref_id_, is_exist))) {
            LOG_WARN("check table id exists failed", K_(item->ref_id));
          } else if (is_exist) {
            //in the whole query stmt, table exists in the other query layer, so subquery must alias it
            //implicit alias table, alias name is table name
            item->table_id_ = generate_table_id();
            item->type_ = TableItem::ALIAS_TABLE;
            if (!synonym_name.empty()) {
              // bug: 31827906
              item->alias_name_ = synonym_name;
              item->database_name_ = synonym_db_name;
            } else {
              item->alias_name_ = tbl_name;
            }
          } else {
            //base table, no alias name
            item->table_id_ = generate_table_id();
          }
        } else {
          item->table_id_ = generate_table_id();
          item->alias_name_ = alias_name;
        }
      }
    }
    if (OB_SUCC(ret)) {
      ObSchemaObjVersion table_version;
      table_version.object_id_ = tschema->get_table_id();
      table_version.object_type_ = tschema->is_view_table() ? DEPENDENCY_VIEW : DEPENDENCY_TABLE;
      table_version.version_ = tschema->get_schema_version();
      table_version.is_db_explicit_ = is_db_explicit;
      if (common::is_cte_table(table_version.object_id_)) {
         // do nothing
      } else if (OB_FAIL(stmt->add_global_dependency_table(table_version))) {
        LOG_WARN("add global dependency table failed", K(ret));
      } else { /*do nothing*/ }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(stmt->add_table_item(session_info_, item, params_.have_same_table_name_))) {
        LOG_WARN("push back table item failed", K(ret), KPC(item));
      } else {
        tbl_item = item;
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_base_or_alias_table_item_dblink(uint64_t dblink_id,
                                                           const ObString &dblink_name,
                                                           const ObString &database_name,
                                                           const ObString &table_name,
                                                           const ObString &alias_name,
                                                           const ObString &synonym_name,
                                                           const ObString &synonym_db_name,
                                                           TableItem *&table_item)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  TableItem *item = NULL;
  const ObTableSchema *table_schema = NULL;
  if (OB_ISNULL(stmt) || OB_ISNULL(schema_checker_) || OB_ISNULL(session_info_) || OB_ISNULL(allocator_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("resolver isn't init", K(stmt), K_(schema_checker), K_(session_info), K_(allocator));
  } else if (OB_ISNULL(item = stmt->create_table_item(*allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("create table item failed");
  } else if (OB_FAIL(schema_checker_->get_link_table_schema(dblink_id, database_name,
                                                            table_name, table_schema, session_info_->get_sessid()))) {
    LOG_WARN("get link table info failed", K(ret));
  } else {
    // common info.
    if (0 == alias_name.length()) {
      // item->table_id_ = table_schema->get_table_id();
      // table_id_ must be unique, ref_id_
      // ex: SELECT c_id FROM remote_dblink_test.stu2@my_link3 WHERE p_id = (SELECT MIN(p_id) FROM remote_dblink_test.stu2@my_link3);
      // parent table id and sub table id may same if table_id_ using table_schema->get_table_id();
      item->table_id_ = generate_table_id();
      item->type_ = TableItem::BASE_TABLE;
    } else {
      item->table_id_ = generate_table_id();
      item->type_ = TableItem::ALIAS_TABLE;
      item->alias_name_ = alias_name;
    }
    item->ref_id_ = table_schema->get_table_id();
    item->table_name_ = table_name;
    item->is_index_table_ = false;
    item->is_system_table_ = false;
    item->is_view_table_ = false;
    item->synonym_name_ = synonym_name;
    item->synonym_db_name_ = synonym_db_name;
    // dblink info.
    item->dblink_id_ = dblink_id;
    item->dblink_name_ = dblink_name;
    item->link_database_name_ = database_name;
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(stmt->add_table_item(session_info_, item))) {
      LOG_WARN("push back table item failed", K(ret));
    } else {
      table_item = item;
    }
  }

  return ret;
}

int ObDMLResolver::expand_view(TableItem &view_item)
{
  int ret = OB_SUCCESS;
  bool is_oracle_mode = lib::is_oracle_mode();
  int64_t org_session_id = 0;
  if (!is_oracle_mode) {
    if (OB_ISNULL(params_.schema_checker_)
        || OB_ISNULL(params_.schema_checker_->get_schema_guard())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("null ptr", K(ret), KP(params_.schema_checker_));
    } else {
      //bug19839990, MySQL视图解析时需要忽略临时表, 目前不支持视图包含临时表,
      //这里更新sess id防止将视图定义中表按照临时表解析
      org_session_id = params_.schema_checker_->get_schema_guard()->get_session_id();
      params_.schema_checker_->get_schema_guard()->set_session_id(0);
    }
  }
  if (OB_SUCC(ret)) {
    ObViewTableResolver view_resolver(params_, get_view_db_name(), get_view_name());
    view_resolver.set_current_level(current_level_);
    view_resolver.set_current_view_level(current_view_level_ + 1);
    view_resolver.set_view_ref_id(view_item.ref_id_);
    view_resolver.set_current_view_item(view_item);
    view_resolver.set_parent_namespace_resolver(parent_namespace_resolver_);
    if (OB_FAIL(do_expand_view(view_item, view_resolver))) {
      LOG_WARN("do expand view resolve failed", K(ret));
    }
    if (!is_oracle_mode) {
      params_.schema_checker_->get_schema_guard()->set_session_id(org_session_id);
    }
  }
  return ret;
}

int ObDMLResolver::do_expand_view(TableItem &view_item, ObChildStmtResolver &view_resolver)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();

  if (OB_ISNULL(stmt)) {
    ret = OB_NOT_INIT;
    LOG_WARN("stmt is null");
  } else if (OB_ISNULL(params_.session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("params_.session_info_ is null", K(ret));
  } else {
    // expand view as subquery which use view name as alias
    ObSelectStmt *view_stmt = NULL;
    const ObTableSchema *view_schema = NULL;
    ObReferenceObjTable *ref_obj_tbl = NULL;

    if (OB_FAIL(schema_checker_->get_table_schema(params_.session_info_->get_effective_tenant_id(), view_item.ref_id_, view_schema))) {
      LOG_WARN("get table schema failed", K(view_item));
    } else {
      // parse and resolve view defination
      ParseResult view_result;
      ObString view_def;

      ObParser parser(*params_.allocator_, session_info_->get_sql_mode(),
                      session_info_->get_local_collation_connection());
      if (OB_FAIL(ObSQLUtils::generate_view_definition_for_resolve(
                              *params_.allocator_,
                              session_info_->get_local_collation_connection(),
                              view_schema->get_view_schema(),
                              view_def))) {
        LOG_WARN("fail to generate view definition for resolve", K(ret));
      } else if (OB_FAIL(parser.parse(view_def, view_result))) {
        LOG_WARN("parse view defination failed", K(view_def), K(ret));
      } else if (OB_ISNULL(ref_obj_tbl = stmt->get_ref_obj_table())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("reference obj table is null", K(ret));
      } else if (0 == current_view_level_
                && OB_FAIL(ref_obj_tbl->set_obj_schema_version(view_item.ref_id_,
                view_schema->get_database_id(), ObObjectType::VIEW,
                view_schema->get_max_dependency_version(),
                view_schema->get_schema_version(), *allocator_))) {
        LOG_WARN("failed to set max dependency version", K(ret));
      } else {
        // use alias to make all columns number continued
        // view总是在from中，而from子查询不能看到parents的所有属性，所以不能将parent传给from substmt
        // select_resolver.set_upper_scope_stmt(stmt_);
        ParseNode *view_stmt_node = view_result.result_tree_->children_[0];
        if (OB_FAIL(view_resolver.resolve_child_stmt(*view_stmt_node))) {
          if (OB_TABLE_NOT_EXIST == ret || OB_ERR_BAD_FIELD_ERROR == ret || OB_NON_UNIQ_ERROR == ret) {
            ret = OB_ERR_VIEW_INVALID;
            const ObString &db_name = view_item.database_name_;
            const ObString &table_name = view_item.table_name_;
            LOG_USER_ERROR(OB_ERR_VIEW_INVALID, db_name.length(), db_name.ptr(), table_name.length(), table_name.ptr());
          } else {
            LOG_WARN("expand view table failed", K(ret));
          }
        } else {
          view_stmt = view_resolver.get_child_stmt();
          view_stmt->set_is_view_stmt(true, view_item.ref_id_);
          view_stmt->set_check_option(view_schema->get_view_schema().get_view_check_option());
        }
      }
      if (OB_SUCC(ret)) {
        for (int64_t i = 0; i < view_stmt->get_select_item_size(); ++i) {
          SelectItem &item = view_stmt->get_select_item(i);
          item.is_real_alias_ = true;
        }
      }
      // push-down view_stmt as sub-query for view_item
      if (OB_SUCC(ret)) {
        view_item.type_ = TableItem::GENERATED_TABLE;
        view_item.ref_query_ = view_stmt;
        view_item.is_view_table_ = true;
      }
    }
  }

  return ret;
}

int ObDMLResolver::resolve_table_partition_expr(const TableItem &table_item, const ObTableSchema &table_schema)
{
  int ret = OB_SUCCESS;
  const ObString &part_str = table_schema.get_part_option().get_part_func_expr_str();
  ObPartitionFuncType part_type = table_schema.get_part_option().get_part_func_type();
  ObRawExpr *part_expr = NULL;
  ObRawExpr *subpart_expr = NULL;
  ObDMLStmt *dml_stmt = get_stmt();
  if (OB_ISNULL(dml_stmt)) {
    ret = OB_NOT_INIT;
    LOG_WARN("dml_stmt is null");
  } else if (table_schema.get_part_level() != PARTITION_LEVEL_ZERO) {
    if (OB_FAIL(resolve_partition_expr(table_item, table_schema, part_type, part_str, part_expr))) {
      LOG_WARN("Failed to resolve partition expr", K(ret), K(part_str), K(part_type));
    } else if (PARTITION_LEVEL_TWO == table_schema.get_part_level()) {
      const ObString &subpart_str = table_schema.get_sub_part_option().get_part_func_expr_str();
      ObPartitionFuncType subpart_type = table_schema.get_sub_part_option().get_part_func_type();
      if (OB_FAIL(resolve_partition_expr(table_item, table_schema, subpart_type, subpart_str, subpart_expr))) {
        LOG_WARN("Failed to resolve partition expr", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      ObRawExpr *tmp_part_expr = part_expr;
      ObRawExpr *tmp_subpart_expr = subpart_expr;
      if (session_info_->get_ddl_info().is_ddl() ) {
        const ObTableSchema *index_schema = NULL;
        const ObPartitionKeyInfo &partition_keys = table_schema.get_partition_key_info();
        const ObPartitionKeyInfo &subpartition_keys = table_schema.get_subpartition_key_info();
        bool index_table_has_part_key = true;
        bool index_table_has_subpart_key = true;
        if (OB_FAIL(schema_checker_->get_table_schema(session_info_->get_effective_tenant_id(), table_item.ddl_table_id_, index_schema))) {
          LOG_WARN("get index schema from schema checker failed", K(ret), K(table_item.ddl_table_id_));
        } else if (OB_ISNULL(index_schema)) {
          ret = OB_TABLE_NOT_EXIST;
          LOG_WARN("index table not exists", K(table_item.ddl_table_id_));
        } else if (nullptr != part_expr) {
          if (OB_FAIL(check_index_table_has_partition_keys(index_schema, partition_keys, index_table_has_part_key))) {
            LOG_WARN("fail to check if index table has partition keys", K(ret));
          } else if (index_table_has_part_key) {
            // part key is in index table, no need to do replace
          } else if (OB_FAIL(ObRawExprUtils::get_real_expr_without_generated_column(part_expr, tmp_part_expr))) {
            LOG_WARN("get real expr without generated column", K(ret));
          }
        }
        if (OB_SUCC(ret) && nullptr != subpart_expr) {
          if (OB_FAIL(check_index_table_has_partition_keys(index_schema,
                                                           subpartition_keys,
                                                           index_table_has_subpart_key))) {
            LOG_WARN("fail to check if index table has partition keys", K(ret));
          } else if (index_table_has_subpart_key) {
            // subpart key is in index table, no need to do replace
          } else if (OB_FAIL(ObRawExprUtils::get_real_expr_without_generated_column(subpart_expr, tmp_subpart_expr))) {
            LOG_WARN("get real expr without generated column", K(ret));
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(dml_stmt->set_part_expr(table_item.table_id_, table_item.ref_id_, tmp_part_expr, tmp_subpart_expr))) {
        LOG_WARN("set part expr to dml stmt failed", K(ret));
      } else {
        LOG_TRACE("resolve partition expr", K(table_item), K(*part_expr), K(part_str));
      }
    }
  }
  //resolve global index table partition expr
  if (OB_SUCC(ret)) {
    ObSEArray<ObAuxTableMetaInfo, 16> simple_index_infos;
    if (OB_FAIL(table_schema.get_simple_index_infos(simple_index_infos, false))) {
      LOG_WARN("get simple_index_infos failed", K(ret));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < simple_index_infos.count(); ++i) {
      const ObTableSchema *index_schema = NULL;
      if (OB_FAIL(schema_checker_->get_table_schema(session_info_->get_effective_tenant_id(), simple_index_infos.at(i).table_id_, index_schema, table_item.is_link_table()))) {
        LOG_WARN("get index schema from schema checker failed", K(ret), K(simple_index_infos.at(i).table_id_));
      } else if (OB_ISNULL(index_schema)) {
        ret = OB_TABLE_NOT_EXIST;
        LOG_WARN("index table not exists", K(simple_index_infos.at(i).table_id_));
      } else if (index_schema->is_final_invalid_index() || !index_schema->is_global_index_table()) {
        //do nothing
      } else if (index_schema->get_part_level() != PARTITION_LEVEL_ZERO) {
        ObPartitionFuncType index_part_type = index_schema->get_part_option().get_part_func_type();
        const ObString &index_part_str = index_schema->get_part_option().get_part_func_expr_str();
        ObRawExpr *index_part_expr = NULL;
        ObRawExpr *index_subpart_expr = NULL;
        if (OB_FAIL(resolve_partition_expr(table_item, table_schema, index_part_type, index_part_str, index_part_expr))) {
          LOG_WARN("resolve global index table partition expr failed", K(ret), K(index_part_str), K(index_part_type));
        } else if (OB_FAIL(PARTITION_LEVEL_TWO == index_schema->get_part_level())) {
          ObPartitionFuncType index_subpart_type = index_schema->get_sub_part_option().get_part_func_type();
          const ObString &index_subpart_str = index_schema->get_sub_part_option().get_part_func_expr_str();
          if (OB_FAIL(resolve_partition_expr(table_item,
                                             table_schema,
                                             index_subpart_type,
                                             index_subpart_str,
                                             index_subpart_expr))) {
            LOG_WARN("resolve subpart expr failed", K(ret), K(index_subpart_str), K(index_subpart_type));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(dml_stmt->set_part_expr(table_item.table_id_,
                                              index_schema->get_table_id(),
                                              index_part_expr,
                                              index_subpart_expr))) {
            LOG_WARN("set part expr to dml stmt failed", K(ret), K(index_schema->get_table_id()));
          }
        }
      }
    }
  }
  return ret;
}

//for recursively process columns item in resolve_partition_expr
//just wrap columns process logic in resolve_partition_expr
int ObDMLResolver::resolve_columns_for_partition_expr(ObRawExpr *&expr,
                                                      ObIArray<ObQualifiedName> &columns,
                                                      const TableItem &table_item,
                                                      bool include_hidden)
{
  int ret = OB_SUCCESS;
  ObArray<ObRawExpr*> real_exprs;
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); i++) {
    ObQualifiedName &q_name = columns.at(i);
    ObRawExpr *real_ref_expr = NULL;
    if (q_name.is_sys_func()) {
      if (OB_FAIL(resolve_qualified_identifier(q_name, columns, real_exprs, real_ref_expr))) {
        LOG_WARN("resolve sysfunc expr failed", K(q_name), K(ret));
      } else if (OB_ISNULL(real_ref_expr)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("expr is NULL", K(ret));
      } else if (!real_ref_expr->is_sys_func_expr()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid exor", K(*real_ref_expr), K(ret));
      } else {
        ObSysFunRawExpr *sys_func_expr = static_cast<ObSysFunRawExpr*>(real_ref_expr);
         if (OB_FAIL(sys_func_expr->check_param_num())) {
          LOG_WARN("sys func check param failed", K(ret));
        }
      }
    } else {
      ColumnItem *column_item = NULL;
      if (OB_FAIL(resolve_basic_column_item(table_item, q_name.col_name_, include_hidden, column_item))) {
        LOG_WARN("resolve basic column item failed", K(i), K(q_name), K(ret));
      } else {
        real_ref_expr = column_item->expr_;
      }
    }

    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < real_exprs.count(); ++i) {
        OZ (ObRawExprUtils::replace_ref_column(real_ref_expr, columns.at(i).ref_expr_, real_exprs.at(i)));
      }
      OZ (real_exprs.push_back(real_ref_expr));
      OZ (ObRawExprUtils::replace_ref_column(expr, q_name.ref_expr_, real_ref_expr));
    }
  }
  return ret;
}

int ObDMLResolver::resolve_partition_expr(
    const TableItem &table_item,
    const ObTableSchema &table_schema,
    const ObPartitionFuncType part_type,
    const ObString &part_str,
    ObRawExpr *&expr)
{
  int ret = OB_SUCCESS;
  ObArray<ObQualifiedName> columns;

  //for special case key()
  if (OB_ISNULL(params_.session_info_) || OB_ISNULL(allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("null pointer", K(ret));
  } else if (OB_UNLIKELY(part_type >= PARTITION_FUNC_TYPE_MAX)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Error part type", K(ret));
  } else if (PARTITION_FUNC_TYPE_KEY_IMPLICIT == part_type) {
    if (OB_FAIL(ObResolverUtils::build_partition_key_expr(params_,
                                                          table_schema,
                                                          expr,
                                                          &columns))) {
      LOG_WARN("failed to build partition key expr!", K(ret));
    }
  } else if (OB_UNLIKELY(part_str.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Part string should not be empty", K(ret));
  } else {
    ObSqlString sql_str;
    ParseResult parse_result;
    ParseNode *stmt_node = NULL;
    ParseNode *select_node = NULL;
    ParseNode *select_expr_list = NULL;
    ParseNode *select_expr_node = NULL;
    ParseNode *part_expr_node = NULL;
    ObSQLMode sql_mode = params_.session_info_->get_sql_mode();
    //这里普通租户工作线程和rs主动发rpc启的obs工作线程都会访问
    //如果当前线程局部是oracle,期望走oracle parser
    if (lib::is_oracle_mode()) {
      sql_mode = DEFAULT_ORACLE_MODE | SMO_ORACLE;
    }
    ObParser parser(*allocator_, sql_mode);
    LOG_DEBUG("resolve partition expr", K(sql_mode), K(table_schema.get_tenant_id()));
    if (PARTITION_FUNC_TYPE_KEY == part_type) {
      if (OB_FAIL(sql_str.append_fmt("SELECT %s(%.*s) FROM DUAL", N_PART_KEY,
                                     part_str.length(), part_str.ptr()))) {
        LOG_WARN("fail to concat string", K(part_str), K(ret));
      }
    } else if (lib::is_oracle_mode() && PARTITION_FUNC_TYPE_HASH == part_type) {
      if (OB_FAIL(sql_str.append_fmt("SELECT %s(%.*s) FROM DUAL", N_PART_HASH,
                                     part_str.length(), part_str.ptr()))) {
        LOG_WARN("fail to concat string", K(part_str), K(ret));
      }
    } else {
      //对于oracle模式下的部分特殊关键字需要添加双引号去除关键字属性，以防止将列名识别为了函数。比如current_date
      if (lib::is_oracle_mode()) {
        ObArenaAllocator calc_buf(ObModIds::OB_SQL_PARSER);
        ObString new_part_str;
        if (OB_FAIL(process_part_str(calc_buf, part_str, new_part_str))) {
          LOG_WARN("failed to process part str");
        } else if (OB_FAIL(sql_str.append_fmt("SELECT (%.*s) FROM DUAL",
                                              new_part_str.length(),
                                              new_part_str.ptr()))) {
          LOG_WARN("fail to concat string", K(part_str), K(ret));
        } else {/*do nothing*/}
      } else if (OB_FAIL(sql_str.append_fmt("SELECT (%.*s) FROM DUAL",
                                            part_str.length(),
                                            part_str.ptr()))) {
        LOG_WARN("fail to concat string", K(part_str), K(ret));
      }
    }

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(parser.parse(sql_str.string(), parse_result))) {
      ret = OB_ERR_PARSE_SQL;
      _OB_LOG(WARN, "parse: %p, %p, %p, msg=[%s], start_col_=[%d], end_col_[%d], line_[%d], yycolumn[%d], yylineno_[%d], sql[%.*s]",
              parse_result.yyscan_info_,
              parse_result.result_tree_,
              parse_result.malloc_pool_,
              parse_result.error_msg_,
              parse_result.start_col_,
              parse_result.end_col_,
              parse_result.line_,
              parse_result.yycolumn_,
              parse_result.yylineno_,
              static_cast<int>(sql_str.length()),
              sql_str.ptr());
    } else {
      if (OB_ISNULL(stmt_node = parse_result.result_tree_) || OB_UNLIKELY(stmt_node->type_ != T_STMT_LIST)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("stmt node is invalid", K(stmt_node));
      } else if (OB_ISNULL(select_node = stmt_node->children_[0]) || OB_UNLIKELY(select_node->type_ != T_SELECT)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("select node is invalid", K(select_node));
      } else if (OB_ISNULL(select_expr_list =
                           select_node->children_[PARSE_SELECT_SELECT]) || OB_UNLIKELY(select_expr_list->type_ != T_PROJECT_LIST)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("select expr list is invalid", K(ret));
      } else if (OB_ISNULL(select_expr_node = select_expr_list->children_[0])
                 || OB_UNLIKELY(select_expr_node->type_ != T_PROJECT_STRING)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("select expr node is invalid", K(ret));
      } else if (OB_ISNULL(part_expr_node = select_expr_node->children_[0])) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("part expr node is invalid", K(part_expr_node));
      } else if (OB_FAIL(resolve_partition_expr(*part_expr_node, expr, columns))) {
        LOG_WARN("resolve partition expr failed", K(ret));
      } else if (OB_ISNULL(expr)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("expr is null", K(ret));
      }
      //destroy syntax tree
      parser.free_result(parse_result);
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(resolve_columns_for_partition_expr(expr, columns, table_item, table_schema.is_oracle_tmp_table()))) {
      LOG_WARN("resolve columns for partition expr failed", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(expr->formalize(session_info_))) {
      LOG_WARN("formalize expr failed", K(ret));
    }
  }
  return ret;
}

int ObDMLResolver::resolve_partition_expr(const ParseNode &part_expr_node, ObRawExpr *&expr, ObIArray<ObQualifiedName> &columns)
{
  int ret = OB_SUCCESS;
  ObArray<ObSubQueryInfo> sub_query_info;
  ObArray<ObVarInfo> sys_vars;
  ObArray<ObAggFunRawExpr*> aggr_exprs;
  ObArray<ObWinFunRawExpr*> win_exprs;
  ObArray<ObUDFInfo> udf_info;
  ObArray<ObOpRawExpr*> op_exprs;
  ObSEArray<ObUserVarIdentRawExpr*, 1> user_var_exprs;
  ObCollationType collation_connection = CS_TYPE_INVALID;
  ObCharsetType character_set_connection = CHARSET_INVALID;
  if (OB_ISNULL(params_.expr_factory_) || OB_ISNULL(params_.session_info_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("resolve status is invalid", K_(params_.expr_factory), K_(params_.session_info));
  } else if (OB_FAIL(params_.session_info_->get_collation_connection(collation_connection))) {
    LOG_WARN("fail to get collation_connection", K(ret));
  } else if (OB_FAIL(params_.session_info_->get_character_set_connection(character_set_connection))) {
    LOG_WARN("fail to get character_set_connection", K(ret));
  } else {
    ObExprResolveContext ctx(*params_.expr_factory_, params_.session_info_->get_timezone_info(),
                             OB_NAME_CASE_INVALID);
    ctx.dest_collation_ = collation_connection;
    ctx.connection_charset_ = character_set_connection;
    ctx.param_list_ = params_.param_list_;
    ctx.stmt_ = static_cast<ObStmt*>(get_stmt());
    ctx.session_info_ = params_.session_info_;
    ctx.query_ctx_ = params_.query_ctx_;
    ObRawExprResolverImpl expr_resolver(ctx);
    if (OB_FAIL(params_.session_info_->get_name_case_mode(ctx.case_mode_))) {
      LOG_WARN("fail to get name case mode", K(ret));
    } else if (OB_FAIL(expr_resolver.resolve(&part_expr_node, expr, columns, sys_vars,
                                             sub_query_info, aggr_exprs, win_exprs, udf_info,
                                             op_exprs, user_var_exprs))) {
      LOG_WARN("resolve expr failed", K(ret));
    } else if (sub_query_info.count() > 0 || sys_vars.count() > 0 || aggr_exprs.count() > 0 ||
               columns.count() <= 0 || udf_info.count() > 0 || op_exprs.count() > 0) {
      ret = OB_ERR_UNEXPECTED; //TODO Molly not allow type cast in part expr?
      LOG_WARN("part expr is invalid", K(sub_query_info.count()), K(sys_vars.count()),
                K(aggr_exprs.count()), K(columns.count()), K(udf_info.count()));
    }
  }
  return ret;
}

int ObDMLResolver::resolve_is_expr(ObRawExpr *&expr)
{
  int ret = OB_SUCCESS;
  ObOpRawExpr *is_expr = dynamic_cast<ObOpRawExpr *>(expr);
  ObDMLStmt *stmt = get_stmt();
  int64_t num_expr = expr->get_param_count();
  ColumnItem *column_item = NULL;
  if (3 != num_expr) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("is_expr must have three sub_expr", K(num_expr));
  } else if (NULL == is_expr->get_param_expr(0) || NULL == is_expr->get_param_expr(1)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("left argument and right argument can not be null", K(ret));
  } else if (T_REF_COLUMN == is_expr->get_param_expr(0)->get_expr_type()) {
    ObColumnRefRawExpr *ref_expr = static_cast<ObColumnRefRawExpr *>(is_expr->get_param_expr(0));
    ObConstRawExpr *flag_expr = static_cast<ObConstRawExpr *>(is_expr->get_param_expr(2));
    if (NULL == (column_item = stmt->get_column_item_by_id(ref_expr->get_table_id(), ref_expr->get_column_id()))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get column item failed");
    } else if (OB_SUCCESS == ret && NULL != column_item) {
      //t_op_is本应该是两个参数的，这里的第三个参数，是用标记一种特殊情况的
      //特殊情况：如果是date或者datetime列且被声明为not null, 则可以通过类似这样的语句
      //select * from t1 where c1 is null 找到 '0000-00-00'如此的值。
      //所以这里使用第三个参数来标记是不是这种特殊情况，如果是，在计算后缀的时候对其进行特殊处理。
      if ((column_item->get_column_type()->is_date() || column_item->get_column_type()->is_datetime())
          && !lib::is_oracle_mode()
          && column_item->is_not_null_for_read()) {
        flag_expr->get_value().set_bool(true);
        flag_expr->set_expr_obj_meta(flag_expr->get_value().get_meta());
        if (OB_FAIL(flag_expr->formalize(session_info_))) {
          LOG_WARN("formalize expr failed", K(ret));
        }
      } else if (column_item->is_auto_increment()
                 && T_NULL == is_expr->get_param_expr(1)->get_expr_type()) {
        if (OB_FAIL(resolve_autoincrement_column_is_null(expr))) {
          LOG_WARN("fail to process autoincrement column is null", K(ret));
        } else {
          stmt->set_affected_last_insert_id(true);
        }
      }
    }
  }
  return ret;
}

/* resolve default function */
int ObDMLResolver::resolve_special_expr_static(
    const ObTableSchema *table_schema,
    const ObSQLSessionInfo &session_info,
    ObRawExprFactory &expr_factory,
    ObRawExpr *&expr,
    bool& has_default,
    const ObResolverUtils::PureFunctionCheckStatus check_status)
{
  int ret = OB_SUCCESS;
  bool is_found = false;
  if (OB_ISNULL(expr)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(expr));
  }
  if (OB_SUCC(ret) && (expr->has_flag(CNT_DEFAULT))) {
    has_default = true;
    if (expr->has_flag(IS_DEFAULT)) {
      ObDefaultValueUtils utils(NULL, NULL, NULL);
      if (OB_FAIL(utils.resolve_default_function_static(table_schema,
                                                        session_info,
                                                        expr_factory,
                                                        expr,
                                                        check_status))) {
        LOG_WARN("fail to resolve default expr", K(ret), K(*expr));
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); i++) {
      if (OB_FAIL(SMART_CALL(resolve_special_expr_static(table_schema,
                                                         session_info,
                                                         expr_factory,
                                                         expr->get_param_expr(i),
                                                         has_default,
                                                         check_status)))) {
        LOG_WARN("resolve special expr failed", K(ret), K(i));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_special_expr(ObRawExpr *&expr, ObStmtScope scope)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  bool is_found = false;
  if (OB_ISNULL(expr) || OB_ISNULL(stmt) || OB_ISNULL(stmt->get_query_ctx())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(expr), K(stmt));
  } else if (expr->has_flag(CNT_LAST_INSERT_ID) || expr->has_flag(IS_LAST_INSERT_ID)) {
    stmt->set_affected_last_insert_id(true);
  }
  if (OB_SUCC(ret) && (expr->has_flag(CNT_DEFAULT)
      || (expr->has_flag(CNT_IS_EXPR) && T_WHERE_SCOPE == scope)
      || gen_col_exprs_.count() > 0)) {
    //先找找有没有匹配的生成列模板,找到了就不再往下递归了
    if (OB_FAIL(find_generated_column_expr(expr, is_found))) {
      LOG_WARN("find generated column expr failed", K(ret));
    } else if (!is_found) {
      if (expr->has_flag(IS_DEFAULT)) {
        ObDefaultValueUtils utils(stmt, &params_, this);
        if (OB_FAIL(utils.resolve_default_function(expr, scope))) {
          LOG_WARN("fail to resolve default expr", K(ret), K(*expr));
        }
      } else if (T_OP_IS == expr->get_expr_type()) {
        if (OB_FAIL(resolve_is_expr(expr))) {
          LOG_WARN("resolve special is_expr failed", K(ret));
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); i++) {
        if (OB_FAIL(SMART_CALL(resolve_special_expr(expr->get_param_expr(i), scope)))) {
          LOG_WARN("resolve special expr failed", K(ret), K(i));
        }
      }
    }
  }
  if (OB_SUCC(ret) && expr->has_flag(CNT_CUR_TIME)) {
    stmt->get_query_ctx()->fetch_cur_time_ = true;
  }
  return ret;
}

int ObDMLResolver::build_heap_table_hidden_pk_expr(ObRawExpr *&expr, const ObColumnRefRawExpr *ref_expr)
{
  int ret = OB_SUCCESS;
  ObPseudoColumnRawExpr *func_expr = nullptr;
  if (OB_ISNULL(session_info_) || OB_ISNULL(params_.expr_factory_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session info is NULL", K_(session_info), K_(params_.expr_factory));
  } else if (OB_ISNULL(ref_expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ref_expr is NULL", K(ret));
  } else if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_TABLET_AUTOINC_NEXTVAL, func_expr))) {
    LOG_WARN("create nextval failed", K(ret));
  } else {
    func_expr->set_expr_name(ObString::make_string("pk_tablet_seq"));
    func_expr->set_accuracy(ref_expr->get_accuracy());
    func_expr->set_result_flag(ref_expr->get_result_flag());
    func_expr->set_data_type(ref_expr->get_data_type());
    if (OB_FAIL(func_expr->formalize(session_info_))) {
      LOG_WARN("failed to extract info", K(ret));
    } else {
      expr = func_expr;
    }
  }
  return ret;
}

// build next_val expr; set expr as its param
int ObDMLResolver::build_autoinc_nextval_expr(ObRawExpr *&expr,
                                              const uint64_t autoinc_table_id,
                                              const uint64_t autoinc_col_id,
                                              const ObString autoinc_table_name,
                                              const ObString autoinc_column_name)
{
  int ret = OB_SUCCESS;
  ObSysFunRawExpr *func_expr = NULL;
  if (OB_ISNULL(session_info_) || OB_ISNULL(params_.expr_factory_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session info is NULL", K_(session_info), K_(params_.expr_factory));
  } else if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_FUN_SYS_AUTOINC_NEXTVAL, func_expr))) {
    LOG_WARN("create nextval failed", K(ret));
  } else {
    func_expr->set_func_name(ObString::make_string(N_AUTOINC_NEXTVAL));
    if (NULL != expr && OB_FAIL(func_expr->add_param_expr(expr))) {
      LOG_WARN("add function param expr failed", K(ret));
    } else if (OB_FAIL(func_expr->formalize(session_info_))) {
      LOG_WARN("failed to extract info", K(ret));
    } else if (OB_FAIL(ObAutoincNextvalExtra::init_autoinc_nextval_extra(
            allocator_,
            reinterpret_cast<ObRawExpr *&>(func_expr),
            autoinc_table_id,
            autoinc_col_id,
            autoinc_table_name,
            autoinc_column_name))) {
      LOG_WARN("failed to init autoinc_nextval_extra", K(ret));
    } else {
      expr = func_expr;
    }
  }
  return ret;
}

// build partid expr; set expr as its param
int ObDMLResolver::build_partid_expr(ObRawExpr *&expr, const uint64_t table_id)
{
  int ret = OB_SUCCESS;
  ObSysFunRawExpr *func_expr = NULL;
  ObConstRawExpr *table_id_expr = NULL;
  if (OB_ISNULL(session_info_) || OB_ISNULL(params_.expr_factory_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session info is NULL", K_(session_info), K_(params_.expr_factory));
  } else if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_FUN_SYS_PART_ID, func_expr))) {
    LOG_WARN("create part_id failed", K(ret));
  } else if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_UINT64, table_id_expr))) {
    LOG_WARN("create const raw expr failed", K(ret));
  } else {
    ObObj tid;
    tid.set_uint64(table_id);
    table_id_expr->set_value(tid);
    if (OB_FAIL(func_expr->add_param_expr(table_id_expr))) {
      LOG_WARN("add_param_expr failed", K(ret));
    } else if (OB_FAIL(func_expr->formalize(session_info_))) {
      LOG_WARN("failed to extract info", K(ret));
    } else {
      expr = func_expr;
    }
  }
  return ret;
}

int ObDMLResolver::resolve_all_basic_table_columns(const TableItem &table_item, bool include_hidden, ObIArray<ColumnItem> *column_items)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  ColumnItem *col_item = NULL;
  if (OB_ISNULL(schema_checker_) || OB_ISNULL(stmt)) {
    ret = OB_NOT_INIT;
    LOG_WARN("resolver status is invalid", K_(schema_checker), K(stmt));
  } else if (OB_ISNULL(session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session_info_ is null", K(ret));
  } else if (OB_UNLIKELY(!table_item.is_basic_table()) && OB_UNLIKELY(!table_item.is_fake_cte_table()) ) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table isn't basic table", K_(table_item.type));
  } else {
    const ObTableSchema* table_schema = NULL;
    //如果select table是index table,那么*展开应该是index table的所有列而不是主表的所有列
    if (OB_FAIL(schema_checker_->get_table_schema(session_info_->get_effective_tenant_id(), table_item.ref_id_, table_schema, table_item.is_link_table()))) {
      LOG_WARN("fail to get table schema", K(ret), K(table_item.ref_id_));
    } else {
      ObColumnIterByPrevNextID iter(*table_schema);
      const ObColumnSchemaV2 *column_schema = NULL;
      while (OB_SUCC(ret) && OB_SUCC(iter.next(column_schema))) {
        if (OB_ISNULL(column_schema)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("The column is null", K(ret));
        } else if (column_schema->is_shadow_column()) {
          // don't show shadow columns for select * from idx
          continue;
        } else if (column_schema->is_invisible_column()) {
          // don't show invisible columns for select * from tbl_name
          continue;
        } else if (!include_hidden
            && column_schema->is_hidden()) {
          // jump hidden column, but if it is sync ddl user,  not jump __pk_increment
          continue;
        } else if (OB_FAIL(resolve_basic_column_item(table_item, column_schema->get_column_name_str(),
                                                     include_hidden, col_item))) {
          LOG_WARN("resolve column item failed", K(ret));
        } else if (column_items != NULL) {
          if (OB_FAIL(column_items->push_back(*col_item))) {
            LOG_WARN("push back column item failed", K(ret));
          }
        }
      }
      if (ret != OB_ITER_END) {
        LOG_WARN("Failed to iterate all table columns. iter quit. ", K(ret));
      } else {
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_all_generated_table_columns(const TableItem &table_item,
    ObIArray<ColumnItem> &column_items)
{
  int ret = OB_SUCCESS;
  auto stmt = get_stmt();
  if (OB_ISNULL(schema_checker_) || OB_ISNULL(stmt)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!table_item.is_generated_table() && !table_item.is_temp_table()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("table is not generated table or ref query is NULL", K(ret));
  } else if (OB_ISNULL(table_item.ref_query_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("table is not generated table or ref query is NULL", K(ret));
  } else {
    const ObString column_name; // not used, keep empty
    for (int64_t i = 0; OB_SUCC(ret) && i < table_item.ref_query_->get_select_item_size(); i++) {
      const uint64_t col_id = OB_APP_MIN_COLUMN_ID + i;
      ColumnItem *col_item = NULL;
      if (OB_FAIL(resolve_generated_table_column_item(table_item, column_name, col_item, stmt, col_id))) {
        LOG_WARN("resolve generate table item failed", K(ret));
      } else if (OB_ISNULL(col_item)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("column item is NULL", K(ret));
      } else if (OB_FAIL(column_items.push_back(*col_item))) {
        LOG_WARN("array push back failed", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_and_split_sql_expr(const ParseNode &node, ObIArray<ObRawExpr*> &and_exprs)
{
  int ret = OB_SUCCESS;
  ObRawExpr *expr = NULL;
  // where condition will be canonicalized, all continous AND will be merged
  if (OB_ISNULL(params_.expr_factory_) || OB_ISNULL(session_info_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("dml resolve not init", K_(params_.expr_factory), K_(session_info));
  } else if (node.type_ != T_OP_AND) {
    ObExprResolveContext ctx(*params_.expr_factory_, session_info_->get_timezone_info(),
                             OB_NAME_CASE_INVALID);
    ctx.stmt_ = static_cast<ObStmt*>(get_stmt());
    ctx.query_ctx_ = params_.query_ctx_;
    ctx.session_info_ = params_.session_info_;
    ObRawExprCanonicalizerImpl canonicalizer(ctx);
    if (OB_FAIL(resolve_sql_expr(node, expr))) {
      LOG_WARN("resolve sql expr failed", K(ret));
    } else if ( !is_resolving_view_ && OB_FAIL(canonicalizer.canonicalize(expr))) { // canonicalize expression
      LOG_WARN("resolve canonicalize expression", K(ret));
    } else if (OB_FAIL(expr->formalize(session_info_))) {
      LOG_WARN("failed to formalize expr", K(ret));
    } else if (expr->get_expr_type() == T_OP_AND) {
      // no T_OP_AND under another T_OP_AND, which is ensured by canonicalize
      ObOpRawExpr *and_expr = static_cast<ObOpRawExpr *>(expr);
      for (int64_t i = 0; OB_SUCC(ret) && i < and_expr->get_param_count(); i++) {
        ObRawExpr *sub_expr = and_expr->get_param_expr(i);
        OZ((and_exprs.push_back)(sub_expr));
      }
    } else {
      OZ((and_exprs.push_back)(expr));
    }
  } else {
    for (int i = 0; OB_SUCC(ret) && i < node.num_child_; i++) {
      if (OB_FAIL(SMART_CALL(resolve_and_split_sql_expr(*(node.children_[i]), and_exprs)))) {
        LOG_WARN("resolve and split sql expr failed", K(ret), K(i));
      }
    }
  }
  return ret;
}

// 解析所有condition expr，并在这些condition expr前面按需增加bool expr
// 只在新执行引擎开启时增加bool expr
int ObDMLResolver::resolve_and_split_sql_expr_with_bool_expr(const ParseNode &node,
                                                        ObIArray<ObRawExpr*> &and_exprs)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(params_.expr_factory_) || OB_ISNULL(session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("params_.expr_factory_ or session_info_ is NULL", K(ret));
  } else if (OB_FAIL(resolve_and_split_sql_expr(node, and_exprs))) {
    LOG_WARN("resolve_and_split_sql_expr failed", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < and_exprs.count(); ++i) {
      ObRawExpr *new_expr = NULL;
      if (OB_FAIL(ObRawExprUtils::try_create_bool_expr(and_exprs.at(i), new_expr,
                                                          *params_.expr_factory_))) {
        LOG_WARN("try create bool expr failed", K(ret), K(i));
      } else {
        and_exprs.at(i) = new_expr;
      }
    }
  }
  return ret;
}


int ObDMLResolver::add_column_ref_to_set(ObRawExpr *&expr, ObIArray<TableItem*> *table_list)
{
  int ret = OB_SUCCESS;
  bool already_has = false;
  ObDMLStmt * stmt = get_stmt();

  if (OB_ISNULL(stmt)) {
    ret = OB_NOT_INIT;
    LOG_WARN("stmt is NULL");
  } else if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("NULL pointer to expr", K(expr));
  } else if (OB_ISNULL(table_list)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("NULL ObIArray", K(table_list));
  } else if (OB_UNLIKELY(T_REF_COLUMN != expr->get_expr_type())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("wrong type", K(expr->get_expr_type()));
  } else {
    ObColumnRefRawExpr *col_expr = static_cast<ObColumnRefRawExpr*>(expr);
    if (OB_UNLIKELY(OB_INVALID_ID == col_expr->get_table_id())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid table id", K(col_expr->get_table_id()));
    } else {
      TableItem *table = stmt->get_table_item_by_id(col_expr->get_table_id());
      for (int64_t i = 0; OB_SUCC(ret) && !already_has && i < table_list->count(); i++) {
        TableItem *cur_table = table_list->at(i);
        if (OB_ISNULL(cur_table) || OB_ISNULL(table)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("NULL pointer", K(cur_table), K(table));
        } else if (cur_table->table_id_ == table->table_id_) {
          already_has = true;
        }
      }
      if (OB_SUCC(ret) && !already_has
          && OB_FAIL(table_list->push_back(const_cast<TableItem*>(table)))) {
        LOG_WARN("failed to push back", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_current_of(const ParseNode &node,
                                      ObDMLStmt &stmt,
                                      ObIArray<ObRawExpr*> &and_exprs)
{
  int ret = OB_SUCCESS;
  ObRawExpr *cursor_expr = NULL;
  ObRawExpr *equal_expr = NULL;
  if (OB_ISNULL(params_.secondary_namespace_)) {
    // secondary_namespace_ 为空, 说明不是在PL中
    ret = OB_UNIMPLEMENTED_FEATURE;
    LOG_WARN("ORA-03001: unimplemented feature");
  }
  CK(T_SP_EXPLICIT_CURSOR_ATTR == node.type_,
     OB_NOT_NULL(params_.expr_factory_),
     OB_NOT_NULL(params_.schema_checker_),
     (stmt.is_update_stmt() || stmt.is_delete_stmt()));
  if (OB_SUCC(ret)) {
    if (1 == stmt.get_table_size()) {
      ColumnItem *col_item = NULL;
      ObRawExpr *rowid_expr = NULL;
      TableItem *item = stmt.get_table_item(0);
      OZ (resolve_rowid_expr(&stmt, *item, rowid_expr));
      OZ (resolve_sql_expr(node, cursor_expr));
      OZ (ObRawExprUtils::create_equal_expr(*params_.expr_factory_,
                                            params_.session_info_,
                                            rowid_expr,
                                            cursor_expr,
                                            equal_expr));
      OZ (and_exprs.push_back(equal_expr));
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("current of for multi-table not supported", K(stmt.get_table_size()), K(ret));
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "current of for multi-table");
    }
  }
  return ret;
}

int ObDMLResolver::resolve_where_clause(const ParseNode *node)
{
  int ret = OB_SUCCESS;

  if (node) {
    current_scope_ = T_WHERE_SCOPE;
    ObDMLStmt *stmt = get_stmt();

    set_has_oracle_join(false);

    CK(OB_NOT_NULL(stmt), OB_NOT_NULL(node->children_[0]), node->type_ == T_WHERE_CLAUSE);
    if (T_SP_EXPLICIT_CURSOR_ATTR == node->children_[0]->type_) {
      OZ (resolve_current_of(*node->children_[0], *stmt, stmt->get_condition_exprs()));
    } else {
      OZ(resolve_and_split_sql_expr_with_bool_expr(*node->children_[0],
                                                   stmt->get_condition_exprs()));
    }
    OZ(generate_outer_join_tables());
    OZ(deduce_generated_exprs(stmt->get_condition_exprs()));
    OZ(check_equal_conditions_for_resource_group(stmt->get_condition_exprs()));
  }
  return ret;
}

int ObDMLResolver::check_equal_conditions_for_resource_group(const ObIArray<ObRawExpr*> &filters)
{
  int ret = OB_SUCCESS;
  LOG_TRACE("check_equal_conditions_for_resource_group", K(filters), K(session_info_->is_inner()),
            K(params_.enable_res_map_), K(stmt_->get_query_ctx()->res_map_rule_id_),
            K(session_info_->get_current_query_string()), K(session_info_->get_current_query_string().length()));
  if (OB_ISNULL(session_info_) || OB_ISNULL(schema_checker_) ||
      OB_ISNULL(stmt_->get_query_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session info or schema checker is null", K(ret), K(session_info_), K(schema_checker_));
  } else if (!session_info_->is_inner() && params_.enable_res_map_) {
    for (int64_t i = 0; i < filters.count() && OB_SUCC(ret)
          && OB_INVALID_ID == stmt_->get_query_ctx()->res_map_rule_id_; i++) {
      const ObRawExpr *expr = filters.at(i);
      if (OB_ISNULL(expr)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("expr is null", K(ret));
      } else if (expr->has_flag(CNT_CONST) && expr->has_flag(CNT_COLUMN) &&
                OB_FAIL(recursive_check_equal_condition(*expr))) {
        LOG_WARN("recursive check equal condition failed", K(ret));
      }
    }
  }
  return ret;
}


int ObDMLResolver::recursive_check_equal_condition(const ObRawExpr &expr)
{
  int ret = OB_SUCCESS;
  ObResourceColMappingRuleManager &rule_cache = G_RES_MGR.get_col_mapping_rule_mgr();
  if (T_OP_EQ == expr.get_expr_type()) {
    const ObRawExpr *left = NULL;
    const ObRawExpr *right = NULL;
    if (OB_UNLIKELY(2 != expr.get_param_count()) ||
        OB_ISNULL(left = expr.get_param_expr(0)) ||
        OB_ISNULL(right = expr.get_param_expr(1))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("expr is null", K(ret));
    } else {
      const ObColumnRefRawExpr *col_expr = NULL;
      const ObConstRawExpr *const_expr = NULL;
      if (T_REF_COLUMN == left->get_expr_type()) {
        col_expr = static_cast<const ObColumnRefRawExpr*>(left);
      } else if (T_FUN_SYS_CAST == left->get_expr_type()
                && T_REF_COLUMN == left->get_param_expr(0)->get_expr_type()) {
        col_expr = static_cast<const ObColumnRefRawExpr*>(left->get_param_expr(0));
      } else if (left->has_flag(IS_CONST)) {
        const_expr = static_cast<const ObConstRawExpr *>(left);
      } else if (T_FUN_SYS_CAST == left->get_expr_type()
                && left->get_param_expr(0)->has_flag(IS_CONST)) {
        const_expr = static_cast<const ObConstRawExpr *>(left->get_param_expr(0));
      }
      if (NULL != col_expr) {
        if (right->has_flag(IS_CONST)) {
          const_expr = static_cast<const ObConstRawExpr *>(right);
        } else if (T_FUN_SYS_CAST == right->get_expr_type()
                  && right->get_param_expr(0)->has_flag(IS_CONST)) {
          const_expr = static_cast<const ObConstRawExpr *>(right->get_param_expr(0));
        }
      } else if (NULL != const_expr) {
        if (T_REF_COLUMN == right->get_expr_type()) {
          col_expr = static_cast<const ObColumnRefRawExpr*>(right);
        } else if (T_FUN_SYS_CAST == right->get_expr_type()
                  && T_REF_COLUMN == right->get_param_expr(0)->get_expr_type()) {
          col_expr = static_cast<const ObColumnRefRawExpr*>(right->get_param_expr(0));
        }
      }
      if (NULL != col_expr && NULL != const_expr
          && OB_FAIL(check_column_with_res_mapping_rule(col_expr, const_expr))) {
        LOG_WARN("check column with resource mapping rule failed", K(ret), KPC(col_expr), KPC(const_expr));
      }
    }
  }
  if (OB_SUCC(ret)) {
    for (int64_t i = 0; i < expr.get_param_count() && OB_SUCC(ret)
          && OB_INVALID_ID == stmt_->get_query_ctx()->res_map_rule_id_; i++) {
      const ObRawExpr *child = expr.get_param_expr(i);
      if (OB_ISNULL(child)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("expr is null", K(ret));
      } else if (child->has_flag(CNT_CONST) && child->has_flag(CNT_COLUMN) &&
                OB_FAIL(recursive_check_equal_condition(*child))) {
        LOG_WARN("recursive check equal condition failed", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::check_column_with_res_mapping_rule(const ObColumnRefRawExpr *col_expr,
                                                      const ObConstRawExpr *const_expr)
{
  int ret = OB_SUCCESS;
  share::ObResourceColMappingRuleManager &col_rule_mgr = G_RES_MGR.get_col_mapping_rule_mgr();
  uint64_t db_id = session_info_->get_database_id();
  uint64_t tenant_id = session_info_->get_effective_tenant_id();
  const ObObj &value = const_expr->get_value();
  ObNameCaseMode case_mode = OB_NAME_CASE_INVALID;
  const TableItem *table_item = NULL;
  LOG_TRACE("check_column_with_res_mapping_rule", K(value), KPC(col_expr));
  if (!value.is_unknown()) {
    // do nothing.
  } else if (!col_expr->get_database_name().empty() && OB_FAIL(schema_checker_->get_database_id(
        tenant_id, col_expr->get_database_name(), db_id))) {
    LOG_WARN("get database id failed", K(ret));
  } else if (OB_FAIL(session_info_->get_name_case_mode(case_mode))) {
    LOG_WARN("get name case mode faield", K(ret));
  } else if (FALSE_IT(table_item = static_cast<ObDMLStmt*>(stmt_)->get_table_item_by_id(col_expr->get_table_id()))) {
  } else if (OB_NOT_NULL(table_item)) {
    uint64_t rule_id = col_rule_mgr.get_column_mapping_rule_id(
          tenant_id, db_id, table_item->table_name_, col_expr->get_column_name(),
          case_mode);
    LOG_TRACE("get_column_mapping_rule_id", K(stmt_->get_query_ctx()->res_map_rule_id_), K(rule_id));
    if (OB_INVALID_ID == stmt_->get_query_ctx()->res_map_rule_id_ && OB_INVALID_ID != rule_id) {
      const ParamStore *param_store = params_.param_list_;
      if (OB_NOT_NULL(param_store) && OB_LIKELY(value.get_unknown() < param_store->count())) {
        const ObObjParam &param = param_store->at(value.get_unknown());
        const ObString raw_sql = session_info_->get_current_query_string();
        ObString param_text;
        ObCollationType cs_type = CS_TYPE_INVALID;
        if (OB_ISNULL(allocator_)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("allocator is null", K(ret));
        } else if (OB_FAIL(session_info_->get_collation_connection(cs_type))) {
          LOG_WARN("get collation connection failed", K(ret));
        } else if (OB_FAIL(ObObjCaster::get_obj_param_text(param, raw_sql, *allocator_,
                                                           cs_type, param_text))) {
          LOG_WARN("get obj param text failed", K(ret));
        } else if (!param_text.empty()) {
          // Resource manager works only if param is string or numeric type.
          // For example, there is a mapping rule on t.c1.
          // When execute select * from t where c1 = date '2020-01-01', rule_id in the plan is INVALID.

          // Set rule_id and param_idx only if get non-empty param text.
          // get_param_text return non-empty text when param is string or numeric type.
          // This logic works because c1 = '2020-01-01', and c1 = date '2020-01-01' match different plans.
          stmt_->get_query_ctx()->res_map_rule_id_ = rule_id;
          stmt_->get_query_ctx()->res_map_rule_param_idx_ = value.get_unknown();
          uint64_t group_id = G_RES_MGR.get_col_mapping_rule_mgr().get_column_mapping_group_id(
                                tenant_id, rule_id, session_info_->get_user_name(), param_text);
          if (OB_INVALID_ID == group_id) {
              // OB_INVALID_ID means current user+param_value is not defined in mapping rule,
              // get group_id according to current user.
            if (OB_FAIL(G_RES_MGR.get_mapping_rule_mgr().get_group_id_by_user(
                          tenant_id, session_info_->get_user_id(), group_id))) {
              LOG_WARN("get group id by user failed", K(ret));
            } else if (OB_INVALID_ID == group_id) {
              // if not set consumer_group for current user, use OTHER_GROUP by default.
              group_id = 0;
            }
          }
          if (OB_SUCC(ret)) {
            session_info_->set_expect_group_id(group_id);
            if (group_id == THIS_WORKER.get_group_id()) {
              // do nothing if equals to current group id.
            } else if (session_info_->get_is_in_retry()
                        && OB_NEED_SWITCH_CONSUMER_GROUP
                            == session_info_->get_retry_info().get_last_query_retry_err()) {
              LOG_ERROR("use unexpected group when retry, maybe set packet retry failed before",
                        K(group_id), K(THIS_WORKER.get_group_id()), K(rule_id));
            } else {
              ret = OB_NEED_SWITCH_CONSUMER_GROUP;
            }
          }
        }
      }
    }
  } else {
    LOG_TRACE("table item is null", KPC(stmt_));
  }
  return ret;
}

int ObDMLResolver::resolve_order_clause(const ParseNode *order_by_node)
{
  int ret = OB_SUCCESS;
  if (order_by_node) {
    ObDMLStmt *stmt = get_stmt();
    current_scope_ = T_ORDER_SCOPE;
    const ParseNode *sort_list = NULL;
    const ParseNode *siblings_node = NULL;
    if (OB_UNLIKELY(order_by_node->type_ != T_ORDER_BY)
        || OB_UNLIKELY(order_by_node->num_child_ != 2)
        || OB_ISNULL(order_by_node->children_)
        || OB_ISNULL(sort_list = order_by_node->children_[0])
        || OB_ISNULL(stmt)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid parameter", K(order_by_node), K(sort_list), KPC(stmt), K(ret));
    } else if (FALSE_IT(siblings_node = order_by_node->children_[1])) {
    } else if (NULL != siblings_node) {
      if (OB_LIKELY(stmt->is_hierarchical_query())) {
        if (0 < static_cast<ObSelectStmt*>(stmt)->get_group_expr_size()) {
          // Either group by or order by siblings exists, but not both
          // eg: select max(c2) from t1 start with c1 = 1 connect by nocycle prior c1 = c2 group by c1,c2 order siblings by c1, c2;
          ret = OB_ERR_INVALID_SIBLINGS;
          LOG_WARN("ORDER SIBLINGS BY clause not allowed here", K(ret));
        } else {
          static_cast<ObSelectStmt *>(stmt)->set_order_siblings(true);
        }
      } else {
        ret = OB_ERR_INVALID_SIBLINGS;
        LOG_WARN("ORDER SIBLINGS BY clause not allowed here", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      //第一次使用的时候判断是否有group by的order item，如果有的话按照既定规则order by item覆盖，因此需要进行判断
      if (stmt->get_order_item_size() != 0) {
        stmt->get_order_items().reset();
      }
      for (int32_t i = 0; OB_SUCC(ret) && i < sort_list->num_child_; i++) {
        ParseNode *sort_node = sort_list->children_[i];
        OrderItem order_item;
        if (OB_FAIL(resolve_order_item(*sort_node, order_item))) {
          LOG_WARN("resolve order item failed", K(ret));
        } else if (OB_FAIL(stmt->add_order_item(order_item))) {
          // add the order-by item
          LOG_WARN("Add order expression error", K(ret));
        } else if (NULL != siblings_node && OB_NOT_NULL(order_item.expr_)
            && order_item.expr_->has_specified_pseudocolumn()) {
          ret = OB_ERR_CBY_PSEUDO_COLUMN_NOT_ALLOWED;
          LOG_WARN("Specified pseudo column or operator not allowed here", K(ret));
        }
        if (OB_ERR_AGGREGATE_ORDER_FOR_UNION == ret) {
          LOG_USER_ERROR(OB_ERR_AGGREGATE_ORDER_FOR_UNION, i+1);
        }
      }
    } // end of for
  } // end of if
  return ret;
}

int ObDMLResolver::resolve_order_item(const ParseNode &sort_node, OrderItem &order_item)
{
  int ret = OB_SUCCESS;
  ObRawExpr *expr;
  if (ObResolverUtils::set_direction_by_mode(sort_node, order_item)) {
    LOG_WARN("failed to set direction by mode", K(ret));
  }
  if (OB_FAIL(ret)) {
    // do nothing
  } else if (OB_UNLIKELY(sort_node.children_[0]->type_ == T_INT)) {
    ret = OB_ERR_PARSER_SYNTAX;
    SQL_RESV_LOG(WARN, "index order item not support in update");
  } else if (OB_FAIL(resolve_sql_expr(*(sort_node.children_[0]), expr))) {
    SQL_RESV_LOG(WARN, "resolve sql expression failed", K(ret));
  } else {
    order_item.expr_ = expr;
  }
  return ret;
}

int ObDMLResolver::add_column_to_stmt(const TableItem &table_item,
                                      const share::schema::ObColumnSchemaV2 &col,
                                      common::ObIArray<ObColumnRefRawExpr*> &column_exprs,
                                      ObDMLStmt *stmt)
{
  int ret = OB_SUCCESS;
  stmt = (NULL == stmt) ? get_stmt() : stmt;
  if (NULL == stmt) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get stmt fail", K(stmt));
  } else {
    ColumnItem *column_item = NULL;
    if (table_item.is_generated_table() || table_item.is_temp_table()) {
      column_item = ObResolverUtils::find_col_by_base_col_id(*stmt, table_item, col.get_column_id(),
                                            col.get_table_id());
      if (NULL == column_item) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("all basic table's column should add to updatable view before", K(ret));
      }
    } else if (table_item.is_basic_table()) {
      column_item = stmt->get_column_item_by_id(table_item.table_id_, col.get_column_id());
      if (NULL == column_item) {
        if (OB_FAIL(resolve_basic_column_item(table_item, col.get_column_name_str(), true, column_item, stmt))) {
          LOG_WARN("fail to add column item to array", K(ret));
        } else if (OB_ISNULL(column_item) || OB_ISNULL(column_item->expr_)) {
          ret = OB_ERR_BAD_FIELD_ERROR;
          LOG_WARN("failed to add column item", K(ret), K(col.get_column_name_str()));
        }
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("should be generated table or base table", K(ret));
    }


    if (OB_SUCC(ret)) {
      if (OB_FAIL(add_var_to_array_no_dup(column_exprs, column_item->expr_))) {
        LOG_WARN("fail to add column item to array", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::add_all_rowkey_columns_to_stmt(const TableItem &table_item,
                                                  ObIArray<ObColumnRefRawExpr*> &column_exprs,
                                                  ObDMLStmt *stmt /*= NULL*/)
{
  int ret = OB_SUCCESS;
  const ObTableSchema *table_schema = NULL;
  const ObColumnSchemaV2 *column_schema = NULL;
  uint64_t rowkey_column_id = 0;
  uint64_t base_table_id = table_item.get_base_table_item().ref_id_;
  stmt = (NULL == stmt) ? get_stmt() : stmt;
  if (OB_ISNULL(stmt) || OB_ISNULL(schema_checker_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get stmt fail", K(ret), K(stmt), K(schema_checker_));
  } else if (OB_ISNULL(params_.session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("params_.session_info_ is null", K(ret));
  } else if (OB_FAIL(schema_checker_->get_table_schema(params_.session_info_->get_effective_tenant_id(), base_table_id, table_schema, table_item.is_link_table()))) {
    LOG_WARN("table schema not found", K(base_table_id), K(table_item));
  } else if (NULL == table_schema) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get invalid table schema", K(table_item));
  } else {
    const ObRowkeyInfo &rowkey_info = table_schema->get_rowkey_info();
    for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_info.get_size(); ++i) {
      if (OB_FAIL(rowkey_info.get_column_id(i, rowkey_column_id))) {
        LOG_WARN("get rowkey info failed", K(ret), K(i), K(rowkey_info));
      } else if (OB_FAIL(get_column_schema(base_table_id,
                                           rowkey_column_id,
                                           column_schema,
                                           true,
                                           table_item.is_link_table()))) {
        LOG_WARN("get column schema failed", K(base_table_id), K(rowkey_column_id));
      } else if (OB_FAIL(add_column_to_stmt(table_item, *column_schema, column_exprs, stmt))) {
        LOG_WARN("add column to stmt failed", K(ret), K(table_item), KPC(column_schema));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_limit_clause(const ParseNode *node)
{
  int ret = OB_SUCCESS;
  if (node) {
    current_scope_ = T_LIMIT_SCOPE;
    ObDMLStmt *stmt = get_stmt();
    ParseNode *limit_node = NULL;
    ParseNode *offset_node = NULL;
    if (node->type_ == T_LIMIT_CLAUSE) {
      limit_node = node->children_[0];
      offset_node = node->children_[1];
    } else if (node->type_ == T_COMMA_LIMIT_CLAUSE) {
      limit_node = node->children_[1];
      offset_node = node->children_[0];
    }
    ObRawExpr* limit_count = NULL;
    ObRawExpr* limit_offset = NULL;
    // resolve the question mark with less value first
    if (limit_node != NULL && limit_node->type_ == T_QUESTIONMARK && offset_node != NULL
        && offset_node->type_ == T_QUESTIONMARK && limit_node->value_ > offset_node->value_) {
      if (OB_FAIL(ObResolverUtils::resolve_const_expr(params_, *offset_node, limit_offset, NULL))
          || OB_FAIL(ObResolverUtils::resolve_const_expr(params_, *limit_node, limit_count, NULL))) {
        LOG_WARN("Resolve limit/offset error", K(ret));
      }
    } else {
      if (limit_node != NULL) {
        if (limit_node->type_ != T_QUESTIONMARK && limit_node->type_ != T_INT
            && limit_node->type_ != T_COLUMN_REF) {
          ret = OB_ERR_RESOLVE_SQL;
          LOG_WARN("Wrong type of limit value");
        } else {
          if (OB_FAIL(ObResolverUtils::resolve_const_expr(params_, *limit_node, limit_count, NULL))) {
            LOG_WARN("Resolve limit error", K(ret));
          }
        }
      }
      if (ret == OB_SUCCESS && offset_node != NULL) {
        if (offset_node->type_ != T_INT && offset_node->type_ != T_UINT64
            && offset_node->type_ != T_QUESTIONMARK && offset_node->type_ != T_COLUMN_REF) {
          ret = OB_ERR_RESOLVE_SQL;
          LOG_WARN("Wrong type of limit value", K(ret), K(offset_node->type_));
        } else if (OB_FAIL(ObResolverUtils::resolve_const_expr(params_, *offset_node, limit_offset, NULL))) {
          LOG_WARN("Resolve offset error", K(ret));
        }
      }
    }
    CK(session_info_)
    if (OB_SUCC(ret)) {
      // make sure limit expr is int value in static typing engine.
      ObRawExpr **exprs[] = { &limit_count, &limit_offset };
      for (int64_t i = 0; i < ARRAYSIZEOF(exprs) && OB_SUCC(ret); i++) {
        ObExprResType dst_type;
        dst_type.set_int();
        ObSysFunRawExpr *cast_expr = NULL;
        if (NULL != (*exprs[i]) && !ob_is_int_tc((*exprs[i])->get_result_type().get_type())) {
          OZ(ObRawExprUtils::create_cast_expr(
                  *params_.expr_factory_, *exprs[i], dst_type, cast_expr, session_info_));
          CK(NULL != cast_expr);
          if (OB_SUCC(ret)) {
            *exprs[i] = cast_expr;
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      stmt->set_limit_offset(limit_count, limit_offset);
    }
  }
  return ret;
}

// Forbit select with order by limit exists in subquery in Oralce mode
// eg: select 1 from t1 where c1 in (select d1 from t2 order by c1); --error
// 如果subquery中同时存在fetch clause,则是允许存在order by:
// eg: select 1 from t1 where c1 in (select d1 from t2 order by c1 fetch next 1 rows only); --right
int ObDMLResolver::check_order_by_for_subquery_stmt(const ObSubQueryInfo &info)
{
  int ret = OB_SUCCESS;
  LOG_DEBUG("fail to check select stmt order by clause", K(ret), K(current_scope_), K(info));
  if (is_oracle_mode() && T_FROM_SCOPE != current_scope_) {
    LOG_DEBUG("fail to check select stmt order by clause", K(ret), K(current_scope_), K(info));
    if (OB_ISNULL(info.ref_expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to check select stmt order by clause", K(ret));
    } else if (!info.ref_expr_->is_cursor() //游标表达式允许ORDER BY子句
               && (OB_NOT_NULL(info.ref_expr_->get_ref_stmt())
                   && !info.ref_expr_->get_ref_stmt()->has_fetch()) //fetch表达式允许ORDER BY子句
               && OB_FAIL(check_stmt_order_by(info.ref_expr_->get_ref_stmt()))) {
      LOG_WARN("fail to check select stmt order by clause", K(ret));
    }
  }
  return ret;
}

int ObDMLResolver::check_stmt_order_by(const ObSelectStmt *stmt)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(stmt)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("stmt is null after resolve", K(ret));
  } else {
    if (stmt->has_order_by()) {
      ret = OB_ERR_PARSER_SYNTAX;
      LOG_WARN("order by is forbit to exists in subquery ", K(ret));
    } else if (stmt->is_set_stmt()) {
      ObSEArray<ObSelectStmt*, 2> child_stmts;
      if (OB_FAIL(stmt->get_child_stmts(child_stmts))) {
        LOG_WARN("fail to get child stmts", K(ret));
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < child_stmts.count(); ++i) {
        const ObSelectStmt *sub_stmt = child_stmts.at(i);
        if (OB_FAIL(SMART_CALL(check_stmt_order_by(sub_stmt)))) {
          LOG_WARN("fail to check sub stmt order by", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_subquery_info(const ObIArray<ObSubQueryInfo> &subquery_info)
{
  int ret = OB_SUCCESS;
  if (current_level_ + 1 >= OB_MAX_SUBQUERY_LAYER_NUM && subquery_info.count() > 0) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "Too many levels of subquery");
  }
  //resolve subquery in insert all need reset the flag.
  bool is_multi_table_insert = params_.is_multi_table_insert_;
  params_.is_multi_table_insert_ = false;
  for (int64_t i = 0; OB_SUCC(ret) && i < subquery_info.count(); i++) {
    const ObSubQueryInfo &info = subquery_info.at(i);
    ObSelectResolver subquery_resolver(params_);
    subquery_resolver.set_current_level(current_level_ + 1);
    subquery_resolver.set_current_view_level(current_view_level_);
    subquery_resolver.set_parent_namespace_resolver(this);
    set_query_ref_expr(info.ref_expr_);
    if (OB_FAIL(add_cte_table_to_children(subquery_resolver))) {
      LOG_WARN("add CTE table to children failed", K(ret));
    } else if (OB_FAIL(subquery_resolver.add_parent_gen_col_exprs(gen_col_exprs_))) {
      LOG_WARN("failed to add parent gen col exprs", K(ret));
    } else if (info.parents_expr_info_.has_member(IS_AGG)) {
      subquery_resolver.set_parent_aggr_level(current_level_);
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(do_resolve_subquery_info(info, subquery_resolver))) {
        LOG_WARN("do resolve subquery info failed", K(ret));
      }
    }
    set_query_ref_expr(NULL);
  }
  params_.is_multi_table_insert_ = is_multi_table_insert;
  return ret;
}

int ObDMLResolver::do_resolve_subquery_info(const ObSubQueryInfo &subquery_info, ObChildStmtResolver &child_resolver)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  ObSelectStmt *sub_stmt = NULL;

  if (OB_ISNULL(subquery_info.sub_query_) || OB_ISNULL(subquery_info.ref_expr_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("subquery info is invalid", K_(subquery_info.sub_query), K_(subquery_info.ref_expr));
  } else if (OB_UNLIKELY(T_SELECT != subquery_info.sub_query_->type_)) {
    ret = OB_ERR_ILLEGAL_TYPE;
    LOG_WARN("Unknown statement type in subquery", "stmt_type", subquery_info.sub_query_->type_);
  } else {
    subquery_info.ref_expr_->set_expr_level(current_level_);
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(child_resolver.resolve_child_stmt(*(subquery_info.sub_query_)))) {
    LOG_WARN("resolve select subquery failed", K(ret));
  } else {
    sub_stmt = child_resolver.get_child_stmt();
    subquery_info.ref_expr_->set_output_column(sub_stmt->get_select_item_size());
    //将子查询select item的result type保存到ObUnaryRef中
    for (int64_t i = 0; OB_SUCC(ret) && i < sub_stmt->get_select_item_size(); ++i) {
      ObRawExpr *target_expr = sub_stmt->get_select_item(i).expr_;
      if (OB_ISNULL(target_expr)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("target expr is null");
      } else {
        const ObExprResType &column_type = target_expr->get_result_type();
        if (OB_FAIL(subquery_info.ref_expr_->add_column_type(column_type))) {
          LOG_WARN("add column type to subquery ref expr failed", K(ret));
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    subquery_info.ref_expr_->set_ref_stmt(sub_stmt);
    if (OB_FAIL(stmt->add_subquery_ref(const_cast<ObSubQueryInfo&>(subquery_info).ref_expr_))) {
      LOG_WARN("failed to add subquery reference", K(ret));
    } else {
      if (OB_FAIL(check_order_by_for_subquery_stmt(subquery_info))) {
        LOG_WARN("check subquery order by failed", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::check_expr_param(const ObRawExpr &expr)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  if (OB_ISNULL(stmt)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else if ((lib::is_mysql_mode() || stmt->is_insert_stmt()) && T_REF_QUERY == expr.get_expr_type()) {
    const ObQueryRefRawExpr &ref_query = static_cast<const ObQueryRefRawExpr&>(expr);
    if (1 != ref_query.get_output_column()) {
      ret = OB_ERR_INVALID_COLUMN_NUM;
      LOG_USER_ERROR(OB_ERR_INVALID_COLUMN_NUM, (int64_t)1);
      LOG_WARN("ref_query output column", K(ret), K(ref_query.get_output_column()));
    }
  } else if (lib::is_oracle_mode() && T_REF_QUERY == expr.get_expr_type() && T_ORDER_SCOPE == current_scope_) {
    const ObQueryRefRawExpr &ref_query = static_cast<const ObQueryRefRawExpr&>(expr);
    if (1 != ref_query.get_output_column()) {
      ret = OB_ERR_TOO_MANY_VALUES;
      LOG_USER_ERROR(OB_ERR_TOO_MANY_VALUES);
      LOG_WARN("ref_query output column", K(ret), K(ref_query.get_output_column()));
    }
  } else if (T_OP_ROW == expr.get_expr_type()){
    const ObRawExpr *e = &expr;
    // need check row expr child, e.g.: +((c1, c2)) is resolved to: ROW(ROW(c1, c2))
    while (OB_SUCC(ret) && T_OP_ROW == e->get_expr_type() && 1 == e->get_param_count()) {
      e = e->get_param_expr(0);
      CK(NULL != e);
    }
    if (OB_SUCC(ret) && 1 != e->get_param_count()) {
      ret = OB_ERR_INVALID_COLUMN_NUM;
      LOG_USER_ERROR(OB_ERR_INVALID_COLUMN_NUM, (int64_t)1);
      LOG_WARN("op_row output column", K(ret), K(e->get_param_count()));
    }
  }
  return ret;
}

int ObDMLResolver::resolve_partitions(const ParseNode *part_node,
                                      const ObTableSchema &table_schema,
                                      TableItem &table_item)
{
  int ret = OB_SUCCESS;
  if (NULL != part_node) {
    OB_ASSERT(1 == part_node->num_child_ && part_node->children_[0]->num_child_ > 0);
    const ParseNode *name_list = part_node->children_[0];
    ObString partition_name;
    ObSEArray<ObObjectID, 4> part_ids;
    ObSEArray<ObString, 4> part_names;
    for (int i = 0; OB_SUCC(ret) && i < name_list->num_child_; i++) {
      ObSEArray<ObObjectID, 16> partition_ids;
      partition_name.assign_ptr(name_list->children_[i]->str_value_,
                                static_cast<int32_t>(name_list->children_[i]->str_len_));
      //here just conver partition_name to its lowercase
      ObCharset::casedn(CS_TYPE_UTF8MB4_GENERAL_CI, partition_name);
      ObPartGetter part_getter(table_schema);
      if (T_USE_PARTITION == part_node->type_) {
        if (OB_FAIL(part_getter.get_part_ids(partition_name, partition_ids))) {
          LOG_WARN("failed to get part ids", K(ret), K(partition_name));
          if (OB_UNKNOWN_PARTITION == ret && lib::is_mysql_mode()) {
            LOG_USER_ERROR(OB_UNKNOWN_PARTITION, partition_name.length(), partition_name.ptr(),
                          table_schema.get_table_name_str().length(),
                          table_schema.get_table_name_str().ptr());
          }
        }
      } else if (OB_FAIL(part_getter.get_subpart_ids(partition_name, partition_ids))) {
        LOG_WARN("failed to get subpart ids", K(ret), K(partition_name));
      }

      if (OB_SUCC(ret)) {
        if (OB_FAIL(append_array_no_dup(part_ids, partition_ids))) {
          LOG_WARN("Push partition id error", K(ret));
        } else if (OB_FAIL(part_names.push_back(partition_name))) {
          LOG_WARN("failed to push back partition name", K(ret));
        } else {
          LOG_INFO("part ids", K(partition_name), K(partition_ids));
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(table_item.part_ids_.assign(part_ids))) {
        LOG_WARN("failed to assign part ids", K(ret));
      } else if (OB_FAIL(table_item.part_names_.assign(part_names))) {
        LOG_WARN("failed to assign part names", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::check_basic_column_generated(const ObColumnRefRawExpr *col_expr,
                                                ObDMLStmt *dml_stmt,
                                                bool &is_generated)
{
  int ret = OB_SUCCESS;
  is_generated = false;
  const TableItem *table_item = NULL;
  const ColumnItem *view_column_item = NULL;
  if (OB_ISNULL(col_expr) || OB_ISNULL(dml_stmt)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("expr or stmt is null", K(ret), K(col_expr), K(dml_stmt));
  } else if (col_expr->is_generated_column()) {
    is_generated = true;
  } else if (OB_ISNULL(table_item = dml_stmt->get_table_item_by_id(col_expr->get_table_id()))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get table item item by id failed", K(ret), KPC(col_expr), KPC(dml_stmt));
  } else if (false == ((table_item->is_generated_table() || table_item->is_temp_table())
                        && OB_NOT_NULL(table_item->view_base_item_))) {
    //do thing
  } else if (OB_ISNULL(view_column_item = dml_stmt->get_column_item_by_id(
                                                                      col_expr->get_table_id(),
                                                                      col_expr->get_column_id()))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get column item item by id failed", K(ret), KPC(col_expr));
  } else {
    ObSelectStmt *select_stmt = NULL;
    ColumnItem *basic_column_item = NULL;
    while((table_item->is_generated_table() || table_item->is_temp_table()) && OB_NOT_NULL(table_item->view_base_item_)) {
      select_stmt = table_item->ref_query_;
      table_item = table_item->view_base_item_;
    }
    if (dml_stmt->has_instead_of_trigger()) {
    } else if (OB_ISNULL(select_stmt) || OB_ISNULL(table_item)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ref_query_ is null", K(ret));
    } else if (OB_ISNULL(basic_column_item = select_stmt->get_column_item_by_id(
                                                            table_item->table_id_,
                                                            view_column_item->base_cid_))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get column item item by id failed", K(ret));
    } else if (OB_ISNULL(basic_column_item->expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("expr of column item is null", K(ret));
    } else if (basic_column_item->expr_->is_generated_column()) {
      is_generated = true;
    }
  }
  return ret;
}

// Check the pad flag on generated_column is consistent with the sql_mode on session.
// For the upgraded cluster, the flag is not set, so only returns error if the dependent column
// is char type and the generated column is stored or used by an index
int ObDMLResolver::check_pad_generated_column(const ObSQLSessionInfo &session_info,
                                              const ObTableSchema &table_schema,
                                              const ObColumnSchemaV2 &column_schema,
                                              bool is_link)
{
  UNUSED(is_link);
  int ret = OB_SUCCESS;
  if (!column_schema.is_generated_column()) {
    // do nothing
  } else if (is_pad_char_to_full_length(session_info.get_sql_mode())
             == column_schema.has_column_flag(PAD_WHEN_CALC_GENERATED_COLUMN_FLAG)) {
    // do nothing
  } else {
    bool has_char_dep_column = false;
    bool is_stored_column = column_schema.is_stored_generated_column();
    ObSEArray<uint64_t, 5> cascaded_columns;
    ObSEArray<ObAuxTableMetaInfo, 16> simple_index_infos;
    if (OB_FAIL(column_schema.get_cascaded_column_ids(cascaded_columns))) {
      LOG_WARN("failed to get cascaded_column_ids", K(column_schema));
    } else if (OB_FAIL(table_schema.get_simple_index_infos(simple_index_infos))) {
      LOG_WARN("get simple_index_infos failed", K(ret));
    }
    for (int64_t i = 0; OB_SUCC(ret) && !has_char_dep_column && i < cascaded_columns.count(); ++i) {
      uint64_t column_id = cascaded_columns.at(i);
      const ObColumnSchemaV2 *cascaded_col_schema = table_schema.get_column_schema(column_id);
      if (OB_ISNULL(cascaded_col_schema)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to get column", K(table_schema), K(column_id), K(ret));
      } else if (ObCharType == cascaded_col_schema->get_data_type()
                 || ObNCharType == cascaded_col_schema->get_data_type()) {
        has_char_dep_column = true;
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && !is_stored_column && i < simple_index_infos.count(); ++i) {
      const ObTableSchema *index_table_schema = NULL;
      if (OB_FAIL(schema_checker_->get_table_schema(table_schema.get_tenant_id(), simple_index_infos.at(i).table_id_, index_table_schema))) {
        LOG_WARN("get_table_schema failed", "table id", simple_index_infos.at(i).table_id_, K(ret));
      } else if (OB_ISNULL(index_table_schema)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("table schema should not be null", K(ret));
      } else if (OB_FAIL(index_table_schema->has_column(column_schema.get_column_id(), is_stored_column))) {
        LOG_WARN("falied to check if column is in index schema", K(ret));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (has_char_dep_column && is_stored_column) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("change PAD_CHAR option after created generated column",
          K(session_info.get_sql_mode()), K(column_schema), K(ret));
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "change PAD_CHAR option after created generated column");
    }
  }
  return ret;
}

int ObDMLResolver::build_padding_expr(const ObSQLSessionInfo *session,
                                      const ColumnItem* column,
                                      ObRawExpr *&expr)
{
  int ret = OB_SUCCESS;
  CK(OB_NOT_NULL(session));
  CK(OB_NOT_NULL(column));
  CK(OB_NOT_NULL(expr));
  CK(OB_NOT_NULL(get_stmt()));
  const TableItem *table_item = NULL;
  CK(OB_NOT_NULL(table_item = get_stmt()->get_table_item_by_id(column->table_id_)));
  if (OB_SUCC(ret)) {
    if (!get_stmt()->has_instead_of_trigger()) {
      const ObColumnSchemaV2 *column_schema = NULL;
      const uint64_t tid = OB_INVALID_ID == column->base_tid_ ? column->table_id_ : column->base_tid_;
      const uint64_t cid = OB_INVALID_ID == column->base_cid_ ? column->column_id_ : column->base_cid_;
      if (OB_FAIL(get_column_schema(tid, cid, column_schema, true))) {
        LOG_WARN("fail to get column schema", K(ret), K(*column));
      } else if (NULL == column_schema) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get column schema fail", K(column_schema));
      } else if (OB_FAIL(build_padding_expr(session, column_schema, expr))) {
        LOG_WARN("fail to build padding expr", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::build_padding_expr(const ObSQLSessionInfo *session,
                                      const ObColumnSchemaV2 *column_schema,
                                      ObRawExpr *&expr)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(session) || OB_ISNULL(column_schema) || OB_ISNULL(expr) || OB_ISNULL(params_.expr_factory_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(session), K(column_schema), K(expr), K_(params_.expr_factory));
  } else if (ObObjMeta::is_binary(column_schema->get_data_type(), column_schema->get_collation_type())) {
    if (OB_FAIL(ObRawExprUtils::build_pad_expr(*params_.expr_factory_,
                                               false,
                                               column_schema,
                                               expr,
                                               this->session_info_))) {
      LOG_WARN("fail to build pading expr for binary", K(ret));
    }
  } else if (ObCharType == column_schema->get_data_type()
             || ObNCharType == column_schema->get_data_type()) {
    if (is_pad_char_to_full_length(session->get_sql_mode())) {
      if (OB_FAIL(ObRawExprUtils::build_pad_expr(*params_.expr_factory_,
                                                 true,
                                                 column_schema,
                                                 expr,
                                                 this->session_info_))) {
        LOG_WARN("fail to build pading expr for char", K(ret));
      }
    } else {
      if (OB_FAIL(ObRawExprUtils::build_trim_expr(column_schema, *params_.expr_factory_, session_info_, expr))) {
        LOG_WARN("fail to build trime expr for char", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::build_nvl_expr(const ColumnItem *column_item, ObRawExpr *&expr1, ObRawExpr *&expr2)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(params_.expr_factory_) || OB_ISNULL(session_info_) ||
      OB_ISNULL(column_item) || OB_ISNULL(expr1) || OB_ISNULL(expr2)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("column schema is NULL", K_(params_.expr_factory), K(session_info_),
             K(column_item), K(expr1), K(expr2), K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_nvl_expr(*params_.expr_factory_, column_item, expr1, expr2))) {
    LOG_WARN("fail to build nvl_expr", K(ret));
  } else if (OB_FAIL(expr1->formalize(session_info_))) {
    LOG_WARN("fail to formalize expr", K(ret));
  }
  return ret;
}

int ObDMLResolver::build_nvl_expr(const ColumnItem *column_item, ObRawExpr *&expr)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(column_item) || OB_ISNULL(params_.expr_factory_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("column schema is NULL", K(column_item), K_(params_.expr_factory), K(ret));
  } else if (column_item->get_column_type()->is_timestamp() && column_item->is_not_null_for_write()) {
    bool explicit_value = false;
    if (NULL == session_info_) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("session info is NULL", K(ret));
    } else if (OB_FAIL(session_info_->get_explicit_defaults_for_timestamp(explicit_value))) {
      LOG_WARN("fail to get explicit_defaults_for_timestamp", K(ret));
    } else if (!explicit_value) {
      if (OB_FAIL(ObRawExprUtils::build_nvl_expr(*params_.expr_factory_, column_item, expr))) {
        LOG_WARN("fail to build nvl_expr", K(ret));
      } else if (OB_FAIL(expr->formalize(session_info_))) {
        LOG_WARN("fail to formalize expr", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::build_nvl_expr(const ObColumnSchemaV2 *column_schema, ObRawExpr *&expr)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  if (OB_ISNULL(column_schema) || OB_ISNULL(stmt)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("column schema is NULL", K(column_schema), K(stmt));
  } else {
    ColumnItem *column_item = NULL;
    if (NULL == (column_item = stmt->get_column_item_by_id(column_schema->get_table_id(),
                                                           column_schema->get_column_id()))) {
      LOG_WARN("fail to get column item", K(ret),
               "table_id", column_schema->get_table_id(),
               "column_id", column_schema->get_column_id());
    } else if (OB_FAIL(build_nvl_expr(column_item, expr))) {
      LOG_WARN("fail to build nvl expr", K(ret));
    }

  }
  return ret;
}

//特殊处理c1 is null （c1是自增列的问题）
int ObDMLResolver::resolve_autoincrement_column_is_null(ObRawExpr *&expr)
{
  int ret = OB_SUCCESS;
  bool sql_auto_is_null = false;
  if (OB_ISNULL(session_info_) || OB_ISNULL(params_.expr_factory_) || OB_ISNULL(expr)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("session info is NULL", K_(session_info), K_(params_.expr_factory), K(expr));
  } else if (!is_mysql_mode()) {
    //nothing to do
  } else if (OB_UNLIKELY(expr->get_expr_type() != T_OP_IS) || OB_ISNULL(expr->get_param_expr(0))
  || OB_ISNULL(expr->get_param_expr(1))
  || OB_UNLIKELY(expr->get_param_expr(0)->get_expr_type() != T_REF_COLUMN)
  || OB_UNLIKELY(expr->get_param_expr(1)->get_expr_type() != T_NULL)) {
    LOG_WARN("invalid argument for resolve auto_increment column", K(*expr));
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(session_info_->get_sql_auto_is_null(sql_auto_is_null))) {
    LOG_WARN("fail to get sql_auto_is_null", K(ret));
  } else if (!sql_auto_is_null) {
    //nothing to do
  } else if (OB_FAIL(ObRawExprUtils::build_equal_last_insert_id_expr(
              *params_.expr_factory_, expr, session_info_))) {
    LOG_WARN("fail to build eqaul last_insert_id_expr", K(ret), K(*expr));
  }
  return ret;
}

bool ObDMLResolver::is_need_add_additional_function(const ObRawExpr *expr)
{
  bool bret = false;
  if (OB_ISNULL(expr)) {
    LOG_WARN("invalid argument to check whether to add additional function", K(expr));
  } else if (T_FUN_COLUMN_CONV == expr->get_expr_type()) {
    bret = false;
  } else {
    bret = true;
  }
  return bret;
}

// 新引擎下不能像老引擎一样直接给column conv的child加pad expr,因为新引擎中column conv
// 的转换功能是依赖于cast expr,column conv执行时不会进行cast操作,
// 而是调用cast expr的eval_func来做.
// eg: column_conv -> cast_expr -> child_expr 直接加pad expr后有可能覆盖cast expr,变为
//     column_conv -> pad_expr -> cast_expr -> child_expr,所以需要先erase inner expr,
//     再增加pad, 最后进行formalize, 最终结果为:
//     column_conv -> cast_expr -> pad_expr -> child_expr
int ObDMLResolver::try_add_padding_expr_for_column_conv(const ColumnItem *column,
                                                        ObRawExpr *&expr)
{
  int ret = OB_SUCCESS;
  CK(OB_NOT_NULL(session_info_));
  CK(OB_NOT_NULL(column));
  CK(OB_NOT_NULL(expr));
  CK(OB_NOT_NULL(params_.query_ctx_));
  if (OB_SUCC(ret) && T_FUN_COLUMN_CONV == expr->get_expr_type()
      && !params_.query_ctx_->is_prepare_stmt()) {
    CK(ObExprColumnConv::PARAMS_COUNT_WITH_COLUMN_INFO == expr->get_param_count()
      || ObExprColumnConv::PARAMS_COUNT_WITHOUT_COLUMN_INFO == expr->get_param_count());
    CK(OB_NOT_NULL(expr->get_param_expr(4)));
    if (OB_SUCC(ret)) {
      ObRawExpr *&ori_child = expr->get_param_expr(4);
      ObRawExpr *real_child = NULL;
      OZ(ObRawExprUtils::erase_inner_added_exprs(ori_child, real_child));
      CK(OB_NOT_NULL(real_child));
      if (OB_SUCC(ret) && real_child->get_expr_type() != T_FUN_PAD
          && real_child->get_expr_type() != T_FUN_INNER_TRIM) {
        if (OB_FAIL(build_padding_expr(session_info_, column, real_child))) {
          LOG_WARN("fail to build padding expr", K(ret));
        } else {
          ObRawExpr *&ref_child = expr->get_param_expr(4);
          CK(OB_NOT_NULL(ref_child));
          CK(OB_NOT_NULL(real_child));
          OX(ref_child = real_child);
          OZ(expr->formalize(session_info_));
        }
      }
    } else if (OB_SUCC(ret)) {
      ObRawExpr *&ori_child = expr->get_param_expr(4);
      if (ori_child->get_expr_type() != T_FUN_PAD
          && ori_child->get_expr_type() != T_FUN_INNER_TRIM) {
        if (OB_FAIL(build_padding_expr(session_info_, column, ori_child))) {
          LOG_WARN("fail to build padding expr", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObDMLResolver::add_additional_function_according_to_type(const ColumnItem *column,
                                                             ObRawExpr *&expr,
                                                             ObStmtScope scope,
                                                             bool need_padding)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(column) || OB_ISNULL(expr)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(column), K(expr));
  } else if (OB_ISNULL(column->expr_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid column expr", K(ret), K(column), K(column->expr_));
  } else if (OB_ISNULL(params_.query_ctx_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("query ctx is null", K(ret));
  } else if (!is_need_add_additional_function(expr)) {
    //用于处理values(c1)函数中,c1为char/binary；
    if (need_padding && OB_FAIL(try_add_padding_expr_for_column_conv(column, expr))) {
      LOG_WARN("fail try add padding expr for column conv expr", K(ret));
    }
  } else {
    if (OB_SUCC(ret)) {
      if (T_INSERT_SCOPE == scope && column->is_auto_increment()) {
        // In the old engine, nextval() expr returned ObObj with different types:
        // return ObUInt64Type for generate type if input obj is zero or the original input obj.
        // Not acceptable in static typing engine, so convert to the defined data type first.
        if (OB_FAIL(ObRawExprUtils::build_column_conv_expr(*params_.expr_factory_,
                                                           *params_.allocator_,
                                                           *column->get_expr(),
                                                           expr,
                                                           session_info_))) {
          LOG_WARN("fail to build column conv expr", K(ret), K(column));
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(build_autoinc_nextval_expr(
                expr,
                column->base_tid_,column->base_cid_,
                column->get_expr()->get_table_name(),
                column->get_expr()->get_column_name()))) {
          LOG_WARN("fail to build nextval expr", K(ret), K(column->base_cid_));
        }
      } else if (T_INSERT_SCOPE == scope && column->expr_->is_default_on_null_identity_column()) {
        ObInsertStmt *insert_stmt = NULL;
        ObRawExpr *sequence_expr = NULL;
        if (OB_ISNULL(insert_stmt = static_cast<ObInsertStmt*>(stmt_))){
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("error unexpected", K(insert_stmt), K(ret));
        }
        ObDefaultValueUtils utils(insert_stmt, &params_, static_cast<ObDMLResolver*>(this));
        if (OB_SUCC(ret)) {
          if (OB_FAIL(utils.build_default_expr_for_identity_column(*column, sequence_expr, T_INSERT_SCOPE))) {
            LOG_WARN("build default expr for identity column failed", K(ret));
          } else if (OB_ISNULL(sequence_expr)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("expr should not be null", K(ret));
          } else if (OB_FAIL(build_nvl_expr(column, expr, sequence_expr))) {
            LOG_WARN("fail to build nvl expr", K(column), K(*expr), K(*sequence_expr), K(ret));
          }
        }
      } else if (column->get_column_type()->is_timestamp()) {
        if (OB_FAIL(build_nvl_expr(column, expr))) {
          LOG_WARN("fail to build nvl expr", K(column), K(*expr), K(ret));
        }
      }
    }

    if (OB_SUCC(ret)
        && ObSchemaUtils::is_label_se_column(column->expr_->get_column_flags()) && !session_info_->get_ddl_info().is_ddl()) {
      ObSysFunRawExpr *label_value_check_expr = NULL;
      if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_FUN_LABEL_SE_LABEL_VALUE_CHECK,
                                                         label_value_check_expr))) {
        LOG_WARN("fail to create raw expr", K(ret));
      } else {
        ObString func_name = ObString::make_string(N_OLS_LABEL_VALUE_CHECK);
        label_value_check_expr->set_func_name(func_name);
        if (OB_FAIL(label_value_check_expr->add_param_expr(expr))) {
          LOG_WARN("fail to add parm", K(ret));
        } else if (OB_FAIL(label_value_check_expr->formalize(session_info_))) {
          LOG_WARN("failed to do formalize", K(ret));
        }
      }
      if (OB_SUCC(ret)) {
        expr = label_value_check_expr;
      }
    }

    if (OB_SUCC(ret)) {
      if (need_padding && !params_.query_ctx_->is_prepare_stmt()) {
        OZ(build_padding_expr(session_info_, column, expr));
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(ObRawExprUtils::build_column_conv_expr(*params_.expr_factory_,
                                                  *params_.allocator_,
                                                  *column->get_expr(), expr, session_info_))) {
          LOG_WARN("fail to build column conv expr", K(ret));
        } else if (column->is_geo_ && T_FUN_COLUMN_CONV == expr->get_expr_type()) {
          // 1. set geo sub type to cast mode to column covert expr when update
          // 2. check geo type while doing column covert.
          const ObColumnRefRawExpr *raw_expr = column->get_expr();
          if (OB_ISNULL(raw_expr)) {
            ret = OB_ERR_NULL_VALUE;
            LOG_WARN("raw expr in column item is null", K(ret));
          } else {
            ObGeoType geo_type = raw_expr->get_geo_type();
            ObConstRawExpr *type_expr = static_cast<ObConstRawExpr *>(expr->get_param_expr(0));
            ObObj obj;
            obj.set_int(ObInt32Type, static_cast<uint32_t>(geo_type) << 16 | ObGeometryType);
            type_expr->set_value(obj);
          }
        }
      }
    }
  } //end else
  return ret;
}

int ObDMLResolver::resolve_generated_column_expr(const ObString &expr_str,
    const TableItem &table_item, const ObColumnSchemaV2 *column_schema,
    const ObColumnRefRawExpr &column, ObRawExpr *&ref_expr,
    const bool used_for_generated_column, ObDMLStmt *stmt/* = NULL */)
{
  int ret = OB_SUCCESS;
  ObArray<ObQualifiedName> columns;
  ObRawExprFactory *expr_factory = NULL;
  ObSQLSessionInfo *session_info = NULL;
  const ObTableSchema *table_schema = NULL;
  const bool allow_sequence = !used_for_generated_column;
  if (OB_ISNULL(expr_factory = params_.expr_factory_)
     || OB_ISNULL(session_info = params_.session_info_)
     || OB_ISNULL(schema_checker_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("expr_factory is null", K_(params_.expr_factory), K_(params_.session_info));
  } else if (OB_NOT_NULL(column_schema) &&
             OB_FAIL(schema_checker_->get_table_schema(session_info->get_effective_tenant_id(), column_schema->get_table_id(),
                                                       table_schema))) {
    LOG_WARN("get table schema error", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_generated_column_expr(expr_str,
                                                                 *expr_factory, *session_info,
                                                                 ref_expr, columns,
                                                                 table_schema,
                                                                 allow_sequence,
                                                                 this,
                                                                 schema_checker_))) {
    LOG_WARN("build generated column expr failed", K(ret));
  } else if (!used_for_generated_column && !columns.empty()) {
    for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); i++) {
      bool is_all_sys_func = columns.at(i).is_sys_func();
      if (!is_all_sys_func) {
        ret = OB_ERR_UNEXPECTED;
        ret = update_errno_if_sequence_object(columns.at(i), ret);
        LOG_WARN("no need referece other column, it should not happened", K(expr_str), K(ret));
      }
    }
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    ColumnItem *col_item = NULL;
    ObRawExpr *real_ref_expr = NULL;
    ObArray<ObRawExpr*> real_exprs;
    if (columns.at(i).is_sys_func()) {
      if (OB_FAIL(resolve_qualified_identifier(columns.at(i), columns, real_exprs, real_ref_expr))) {
        LOG_WARN("resolve sysfunc expr failed", K(columns.at(i)), K(ret));
      } else if (OB_ISNULL(real_ref_expr)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("expr is NULL", K(ret));
      } else if (!real_ref_expr->is_sys_func_expr()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid exor", K(*real_ref_expr), K(ret));
      } else {
        ObSysFunRawExpr *sys_func_expr = static_cast<ObSysFunRawExpr*>(real_ref_expr);
        if (OB_FAIL(sys_func_expr->check_param_num())) {
          LOG_WARN("sys func check param failed", K(ret));
        }
      }
    } else if (columns.at(i).is_pl_udf()) {
      if (OB_FAIL(resolve_qualified_identifier(columns.at(i), columns, real_exprs, real_ref_expr))) {
        LOG_WARN("resolve sysfunc expr failed", K(columns.at(i)), K(ret));
      } else if (OB_ISNULL(real_ref_expr)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("expr is NULL", K(ret));
      } else if (!real_ref_expr->is_udf_expr()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid exor", K(*real_ref_expr), K(ret));
      }
    } else {
      if (OB_FAIL(resolve_basic_column_item(table_item, columns.at(i).col_name_,
                                             false, col_item, stmt))) {
        LOG_WARN("resolve basic column item failed", K(ret));
      } else if (OB_ISNULL(col_item) || OB_ISNULL(col_item->expr_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("column item is null", K(col_item));
      } else {
        real_ref_expr = col_item->expr_;
      }
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(real_exprs.push_back(ref_expr))) {
        LOG_WARN("push back error", K(ret));
      } else if (OB_FAIL(ObRawExprUtils::replace_ref_column(ref_expr, columns.at(i).ref_expr_, real_ref_expr))) {
        LOG_WARN("replace column reference expr failed", K(ret));
      } else { /*do nothing*/ }
    }
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(check_pad_generated_column(*session_info, *table_schema, *column_schema))) {
      LOG_WARN("check pad generated column failed", K(ret));
    } else if (OB_FAIL(ObRawExprUtils::build_pad_expr_recursively(*expr_factory, *session_info,
        *table_schema, *column_schema, ref_expr))) {
      LOG_WARN("build padding expr for column_ref failed", K(ret));
    } else if (OB_FAIL(build_padding_expr(session_info, column_schema, ref_expr))) {
      LOG_WARN("build padding expr for self failed", K(ret));
    } else if (OB_FAIL(ref_expr->formalize(session_info))) {
      LOG_WARN("formailize column reference expr failed", K(ret));
    } else if (ObRawExprUtils::need_column_conv(column.get_result_type(), *ref_expr)) {
      if (OB_FAIL(ObRawExprUtils::build_column_conv_expr(*expr_factory, *allocator_,
                                                         column, ref_expr, session_info))) {
        LOG_WARN("build column convert expr failed", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_generated_column_expr_temp(TableItem *table_item, const ObTableSchema &table_schema)
{
  int ret = OB_SUCCESS;
  bool has_index = false;
  const ObColumnSchemaV2 *col_schema = NULL;
  if (OB_ISNULL(params_.expr_factory_) || OB_ISNULL(schema_checker_) ||
      OB_ISNULL(params_.session_info_) || OB_ISNULL(table_item)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dml resolver param isn't inited", K_(params_.expr_factory), K_(schema_checker), K_(params_.session_info));
  } else if (table_schema.has_generated_column()) {
    ObArray<uint64_t> column_ids;
    ObRawExpr *expr = NULL;
    if (OB_FAIL(table_schema.get_generated_column_ids(column_ids))) {
      LOG_WARN("get generated column ids failed", K(ret));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < column_ids.count(); ++i) {
      ObString expr_def;
      if (OB_ISNULL(col_schema = table_schema.get_column_schema(column_ids.at(i)))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get column schema failed", K(column_ids.at(i)));
      } else if (!col_schema->is_generated_column()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("column schema is not generated column", K(*col_schema));
      } else if (OB_FAIL(schema_checker_->check_column_has_index(col_schema->get_tenant_id(),
                                                                 col_schema->get_table_id(),
                                                                 col_schema->get_column_id(),
                                                                 has_index))) {
        LOG_WARN("check column whether has index failed", K(ret));
      } else if (!col_schema->is_stored_generated_column() && !has_index
                 && !col_schema->is_spatial_generated_column()) {
        //do nothing
        //匹配被物化到存储中的生成列，减少冗余计算
        //heap table的生成列作为分区键时，也会作为主键进行物化
      } else if (session_info_->get_ddl_info().is_ddl() && col_schema->is_fulltext_column()) {
        // do not need fulltext column, because we won't build index using fulltext column
      } else if (OB_FAIL(col_schema->get_cur_default_value().get_string(expr_def))) {
        LOG_WARN("get string from current default value failed", K(ret), K(col_schema->get_cur_default_value()));
      } else if (OB_FAIL(ObSQLUtils::convert_sql_text_from_schema_for_resolve(*allocator_,
                                                                              session_info_->get_dtc_params(),
                                                                              expr_def))) {
            LOG_WARN("fail to convert for resolve", K(ret));
      } else if (OB_FAIL(ObRawExprUtils::build_generated_column_expr(expr_def, *params_.expr_factory_, *params_.session_info_,
                                                                     table_item->table_id_,
                                                                     table_schema,
                                                                     *col_schema,
                                                                     expr,
                                                                     false,
                                                                     NULL,
                                                                     schema_checker_))) {
        LOG_WARN("build generated column expr failed", K(ret));
      } else {
        GenColumnExprInfo gen_col_info;
        gen_col_info.dependent_expr_ = expr;
        gen_col_info.stmt_ = get_stmt();
        gen_col_info.table_item_ = table_item;
        gen_col_info.column_name_ = col_schema->get_column_name_str();
        if (OB_FAIL(gen_col_exprs_.push_back(gen_col_info))) {
          LOG_WARN("failed to push back gen col info", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObDMLResolver::find_generated_column_expr(ObRawExpr *&expr, bool &is_found)
{
  int ret = OB_SUCCESS;
  CK( OB_NOT_NULL(expr) );

  is_found = false;
  if (current_scope_ != T_INSERT_SCOPE && current_scope_ != T_UPDATE_SCOPE &&
      !expr->is_const_expr()) {
    // find all the possible const param constraint first
    OC( (find_const_params_for_gen_column)(*expr));

    int64_t found_idx = 0;
    ObColumnRefRawExpr *ref_expr = NULL;
    ObExprEqualCheckContext check_ctx;
    check_ctx.override_const_compare_ = true;
    check_ctx.ignore_implicit_cast_ = true;
    for (int64_t i = 0; OB_SUCC(ret) && !is_found && i < gen_col_exprs_.count(); ++i) {
      if (OB_ISNULL(gen_col_exprs_.at(i).dependent_expr_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("gen col expr is null");
      } else if (gen_col_exprs_.at(i).dependent_expr_->same_as(*expr, &check_ctx) && OB_SUCC(check_ctx.err_code_)) {
        is_found = true;
        found_idx = i;
      } else if (OB_FAIL(ret)) {
        LOG_WARN("compare expr same as failed", K(ret));
      }
    }
    if (OB_SUCC(ret) && is_found) {
      // if found, store the const params
      CK( OB_NOT_NULL(params_.param_list_) );
      CK( OB_NOT_NULL(params_.query_ctx_) );
      ObPCConstParamInfo const_param_info;
      ObObj const_param;
      for (int64_t i = 0; OB_SUCC(ret) && i < check_ctx.param_expr_.count(); i++) {
        int64_t param_idx = check_ctx.param_expr_.at(i).param_idx_;
        CK( param_idx < params_.param_list_->count());
        if (OB_SUCC(ret)) {
          const_param.meta_ = params_.param_list_->at(i).meta_;
          const_param.v_ = params_.param_list_->at(param_idx).v_;
        }
        OC( (const_param_info.const_idx_.push_back)(param_idx) );
        OC( (const_param_info.const_params_.push_back)(const_param) );
      }
      if (check_ctx.param_expr_.count() > 0) {
        OC( (params_.query_ctx_->all_plan_const_param_constraints_.push_back)(const_param_info) );
        LOG_DEBUG("plan const constraint", K(params_.query_ctx_->all_plan_const_param_constraints_));
      }
      ObDMLStmt *stmt = gen_col_exprs_.at(found_idx).stmt_;
      TableItem *table_item = gen_col_exprs_.at(found_idx).table_item_;
      const ObString &column_name = gen_col_exprs_.at(found_idx).column_name_;
      ColumnItem *col_item = NULL;
      if (OB_ISNULL(table_item)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpect null table item", K(ret));
      } else if (OB_FAIL(resolve_basic_column_item(*table_item, column_name, true, col_item, stmt))) {
        LOG_WARN("resolve basic column item failed", K(ret), K(table_item), K(column_name));
      } else if (OB_ISNULL(col_item) || OB_ISNULL(ref_expr = col_item->expr_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("col_item is invalid", K(col_item), K(ref_expr));
      } else {
        expr = ref_expr;
      }
    }
  }
  return ret;
}

int ObDMLResolver::find_const_params_for_gen_column(const ObRawExpr &expr)
{
  int ret = OB_SUCCESS;

  CK( OB_NOT_NULL(params_.query_ctx_) );

  for (int64_t i = 0; OB_SUCC(ret) && i < gen_col_exprs_.count(); i++) {
    CK( OB_NOT_NULL(gen_col_exprs_.at(i).dependent_expr_) );
    ObExprEqualCheckContext check_context;
    ObPCConstParamInfo const_param_info;

    check_context.err_code_ = OB_SUCCESS;
    check_context.override_const_compare_ = false;
    check_context.ignore_implicit_cast_ = true;

    if (OB_SUCC(ret) &&
        gen_col_exprs_.at(i).dependent_expr_->same_as(expr, &check_context)) {
      if (OB_FAIL(check_context.err_code_)) {
        LOG_WARN("failed to compare exprs", K(ret));
      } else if (check_context.param_expr_.count() > 0) {
        // generate column may not contain const param, so check this
        const_param_info.const_idx_.reset();
        const_param_info.const_params_.reset();
        for (int64_t i = 0; OB_SUCC(ret) && i < check_context.param_expr_.count(); i++) {
          ObExprEqualCheckContext::ParamExprPair &param_expr = check_context.param_expr_.at(i);
          CK( OB_NOT_NULL(param_expr.expr_),
              param_expr.param_idx_ >= 0 );
          if (OB_SUCC(ret)) {
            OC( (const_param_info.const_idx_.push_back)(param_expr.param_idx_) );

            const ObConstRawExpr *c_expr = dynamic_cast<const ObConstRawExpr *>(param_expr.expr_);
            CK( OB_NOT_NULL(c_expr) );
            OC( (const_param_info.const_params_.push_back)(c_expr->get_value()) );
          }
        }
        OC( (params_.query_ctx_->all_possible_const_param_constraints_.push_back)(const_param_info) );
        LOG_DEBUG("found all const param constraints", K(params_.query_ctx_->all_possible_const_param_constraints_));
      }
    }
  }
  return ret;
}

int ObDMLResolver::deduce_generated_exprs(ObIArray<ObRawExpr*> &exprs)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObRawExpr*, 8> generate_exprs;
  for (int64_t i = 0; OB_SUCC(ret) && !params_.is_from_create_view_ && i < exprs.count(); ++i) {
    ObRawExpr *expr = exprs.at(i);
    if (OB_ISNULL(expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("expr is null", K(exprs), K(i), K(expr), K(ret));
    } else {
      ObColumnRefRawExpr *column_expr = NULL;
      ObRawExpr *value_expr = NULL;
      ObRawExpr *escape_expr = NULL;
      ObRawExpr *param_expr1 = NULL;
      ObRawExpr *param_expr2 = NULL;
      ObItemType type = expr->get_expr_type();
      // oracle 模式下不支持 lob 进行 in/not in 计算
      if (lib::is_oracle_mode()
          && (T_OP_IN == expr->get_expr_type() || T_OP_NOT_IN == expr->get_expr_type()
              || T_OP_EQ == expr->get_expr_type() || T_OP_NE == expr->get_expr_type()
              || T_OP_SQ_EQ == expr->get_expr_type() || T_OP_SQ_NE == expr->get_expr_type())) {
        if (OB_ISNULL(param_expr1 = expr->get_param_expr(0))
            || OB_ISNULL(param_expr2 = expr->get_param_expr(1))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("expr is null", K(ret), K(*expr),
                   K(expr->get_param_expr(0)), K(expr->get_param_expr(1)));
        } else if (param_expr1->get_result_type().is_lob()
                   || param_expr2->get_result_type().is_lob()
                   || param_expr1->get_result_type().is_lob_locator()
                   || param_expr2->get_result_type().is_lob_locator()) {
          ret = OB_ERR_INVALID_TYPE_FOR_OP;
          LOG_WARN("oracle lob can't be the param of this operation type",
                   K(ret), K(expr->get_expr_type()),
                   KPC(param_expr1), KPC(param_expr2),
                   K(param_expr1->get_result_type()), K(param_expr2->get_result_type()));
        }
      }
      if (OB_FAIL(ret)) {
      } else if (IS_BASIC_CMP_OP(expr->get_expr_type())
                 || IS_SINGLE_VALUE_OP(expr->get_expr_type())) {
        //only =/</<=/>/>=/IN/like can deduce generated exprs
        if (OB_ISNULL(param_expr1 = expr->get_param_expr(0)) || OB_ISNULL(param_expr2 = expr->get_param_expr(1))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("expr is null", K(*expr), K(expr->get_param_expr(0)), K(expr->get_param_expr(1)), K(ret));
        } else if (T_OP_LIKE == expr->get_expr_type()) {
          /*
          https://work.aone.alibaba-inc.com/issue/33030027
          err1: should add const expr for expr2
          err2: if expr2 is 'a%d' deduce is error
                c1 like 'a%d' DOESN'T MEAN:
                substr(c1, 1, n) like substr('a%d', 1, n)
                because after %, there is normal char.
          */
          column_expr = NULL;
        } else if (T_OP_IN == expr->get_expr_type()) {
          if (T_OP_ROW == param_expr2->get_expr_type()
              && !param_expr2->has_generalized_column()
              && param_expr1->get_result_type().is_string_type()) {
            bool all_match = true;
            for (int64_t j = 0; OB_SUCC(ret) && all_match && j < param_expr2->get_param_count(); ++j) {
              if (OB_ISNULL(param_expr2->get_param_expr(j))) {
                ret = OB_ERR_UNEXPECTED;
                LOG_WARN("param expr2 is null");
              } else if (!param_expr2->get_param_expr(j)->get_result_type().is_string_type()
                  || param_expr2->get_param_expr(j)->get_collation_type() != param_expr1->get_collation_type()) {
                all_match = false;
              }
            }
            if (OB_SUCC(ret) && all_match) {
              column_expr = static_cast<ObColumnRefRawExpr*>(expr->get_param_expr(0));
              value_expr = expr->get_param_expr(1);
            }
          }
        } else if (param_expr1->is_column_ref_expr() && param_expr2->is_const_raw_expr()) {
          if (param_expr1->get_result_type().is_string_type() //only for string and same collation
              && param_expr2->get_result_type().is_string_type()
              && param_expr1->get_collation_type() == param_expr2->get_collation_type()) {
            column_expr = static_cast<ObColumnRefRawExpr*>(expr->get_param_expr(0));
            value_expr = expr->get_param_expr(1);
          }
        } else if (param_expr1->is_const_raw_expr() && param_expr2->is_column_ref_expr()) {
          if (param_expr1->get_result_type().is_string_type()
              && param_expr2->get_result_type().is_string_type()
              && param_expr1->get_collation_type() == param_expr2->get_collation_type()) {
            type = get_opposite_compare_type(expr->get_expr_type());
            column_expr = static_cast<ObColumnRefRawExpr*>(expr->get_param_expr(1));
            value_expr = expr->get_param_expr(0);
          }
        }

        //only column op const
        if (OB_SUCC(ret) && NULL != column_expr && column_expr->has_generated_column_deps()) {
          for (int64_t j = 0; OB_SUCC(ret) && j < gen_col_exprs_.count(); ++j) {
            const ObRawExpr *dep_expr = gen_col_exprs_.at(j).dependent_expr_;
            const ObRawExpr *substr_expr = NULL;
            if (OB_ISNULL(dep_expr)) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("generated column expr is null", K(gen_col_exprs_), K(j), K(ret));
            } else if (ObRawExprUtils::has_prefix_str_expr(*dep_expr, *column_expr, substr_expr)) {
              ObRawExpr *new_expr = NULL;
              ObColumnRefRawExpr *left_expr = NULL;
              ObDMLStmt *stmt = gen_col_exprs_.at(j).stmt_;
              TableItem *table_item = gen_col_exprs_.at(j).table_item_;
              const ObString &column_name = gen_col_exprs_.at(j).column_name_;
              ColumnItem *col_item = NULL;
              ObItemType gen_type = type;
              if (T_OP_GT == type) {
                gen_type = T_OP_GE;
              } else if (T_OP_LT == type) {
                gen_type = T_OP_LE;
              }
              if (OB_ISNULL(table_item)) {
                ret = OB_ERR_UNEXPECTED;
                LOG_WARN("unexpect null table item", K(ret));
              } else if (OB_FAIL(resolve_basic_column_item(*table_item, column_name, true, col_item, stmt))) {
                LOG_WARN("resolve basic column reference failed", K(ret));
              } else if (OB_ISNULL(col_item) || OB_ISNULL(left_expr = col_item->expr_)) {
                ret = OB_ERR_UNEXPECTED;
                LOG_WARN("column item is invalid", K(col_item), K(left_expr));
              } else if (OB_FAIL(build_prefix_index_compare_expr(*left_expr,
                                                                 const_cast<ObRawExpr*>(substr_expr),
                                                                 gen_type,
                                                                 *value_expr,
                                                                 escape_expr,
                                                                 new_expr))) {
                LOG_WARN("build prefix index compare expr failed", K(ret));
              } else if (OB_FAIL(generate_exprs.push_back(new_expr))) {
                LOG_WARN("push back error", K(ret));
              } else { /*do nothing*/ }
            }
          }
        }
      }
    }
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(append(exprs, generate_exprs))) {
      LOG_WARN("append error", K(ret));
    } else if (OB_ISNULL(get_stmt())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get stmt is null", K(get_stmt()), K(ret));
    } else if (OB_FAIL(get_stmt()->add_deduced_exprs(generate_exprs))) {
      LOG_WARN("add generated exprs failed", K(generate_exprs), K(ret));
    } else { /*do nothing*/ }
  }
  return ret;
}

int ObDMLResolver::resolve_geo_mbr_column()
{
  int ret = OB_SUCCESS;
  // try to get mbr generated column
  for (int64_t j = 0; OB_SUCC(ret) && j < gen_col_exprs_.count(); ++j) {
    const ObRawExpr *dep_expr = gen_col_exprs_.at(j).dependent_expr_;
    if (dep_expr->get_expr_type() == T_FUN_SYS_SPATIAL_MBR) {
      ObColumnRefRawExpr *left_expr = NULL;
      ObDMLStmt *stmt = gen_col_exprs_.at(j).stmt_;
      TableItem *table_item = gen_col_exprs_.at(j).table_item_;
      const ObString &column_name = gen_col_exprs_.at(j).column_name_;
      ColumnItem *col_item = NULL;
      if (OB_ISNULL(table_item)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpect null table item", K(ret));
      } else if (OB_FAIL(resolve_basic_column_item(*table_item, column_name, true, col_item, stmt))) {
        LOG_WARN("resolve basic column reference failed", K(ret));
      } else if (OB_ISNULL(col_item) || OB_ISNULL(left_expr = col_item->expr_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("column item is invalid", K(col_item), K(left_expr));
      } else {
        left_expr->set_explicited_reference();
        if (OB_FAIL(stmt->add_column_item(*col_item))) {
          LOG_WARN("push back error", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObDMLResolver::add_synonym_obj_id(const ObSynonymChecker &synonym_checker, bool error_with_exist)
{
  int ret = OB_SUCCESS;
  if (synonym_checker.has_synonym()) {
    if (OB_FAIL(add_object_versions_to_dependency(DEPENDENCY_SYNONYM,
                                                 SYNONYM_SCHEMA,
                                                 synonym_checker.get_synonym_ids()))) {
      LOG_WARN("add synonym version failed", K(ret));
    }
  }
  return ret;
}

int ObDMLResolver::resolve_table_relation_factor(const ParseNode *node,
                                                 uint64_t &dblink_id,
                                                 uint64_t &database_id,
                                                 ObString &table_name,
                                                 ObString &synonym_name,
                                                 ObString &synonym_db_name,
                                                 ObString &db_name,
                                                 ObString &dblink_name)
{
  bool is_db_explicit = false;
  UNUSED(is_db_explicit);
  return resolve_table_relation_factor(node,
                                       dblink_id,
                                       database_id,
                                       table_name,
                                       synonym_name,
                                       synonym_db_name,
                                       db_name,
                                       dblink_name,
                                       is_db_explicit);
}

int ObDMLResolver::resolve_table_relation_factor(const ParseNode *node,
                                                 uint64_t tenant_id,
                                                 uint64_t &dblink_id,
                                                 uint64_t &database_id,
                                                 common::ObString &table_name,
                                                 common::ObString &synonym_name,
                                                 common::ObString &synonym_db_name,
                                                 common::ObString &db_name,
                                                 common::ObString &dblink_name,
                                                 ObSynonymChecker &synonym_checker)
{
  bool is_db_explicit = false;
  return resolve_table_relation_factor(node,
                                       tenant_id,
                                       dblink_id,
                                       database_id,
                                       table_name,
                                       synonym_name,
                                       synonym_db_name,
                                       db_name,
                                       dblink_name,
                                       is_db_explicit,
                                       synonym_checker);
}

int ObDMLResolver::resolve_table_relation_factor(const ParseNode *node,
                                                 uint64_t tenant_id,
                                                 uint64_t &dblink_id,
                                                 uint64_t &database_id,
                                                 common::ObString &table_name,
                                                 common::ObString &synonym_name,
                                                 common::ObString &synonym_db_name,
                                                 common::ObString &dblink_name,
                                                 common::ObString &db_name)
{
  bool is_db_explicit = false;
  UNUSED(is_db_explicit);
  return resolve_table_relation_factor(node,
                                       tenant_id,
                                       dblink_id,
                                       database_id,
                                       table_name,
                                       synonym_name,
                                       synonym_db_name,
                                       db_name,
                                       dblink_name,
                                       is_db_explicit);
}

int ObDMLResolver::resolve_table_relation_factor(const ParseNode *node,
                                                 uint64_t &dblink_id,
                                                 uint64_t &database_id,
                                                 ObString &table_name,
                                                 ObString &synonym_name,
                                                 ObString &synonym_db_name,
                                                 ObString &db_name,
                                                 ObString &dblink_name,
                                                 bool &is_db_explicit)
{
  return resolve_table_relation_factor(node, session_info_->get_effective_tenant_id(), dblink_id,
                                       database_id, table_name, synonym_name, synonym_db_name,
                                       db_name, dblink_name, is_db_explicit);
}


int ObDMLResolver::add_object_version_to_dependency(share::schema::ObDependencyTableType table_type,
                                                    share::schema::ObSchemaType schema_type,
                                                    uint64_t object_id)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(get_stmt()) || OB_ISNULL(schema_checker_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("stmt or schema_checker is null", K(get_stmt()), K_(schema_checker));
  } else if (OB_ISNULL(params_.session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("params_.session_info_ is null", K(ret));
  } else {
    int64_t schema_version = OB_INVALID_VERSION;
    ObSchemaObjVersion obj_version;
    bool is_pl_schema_type =
         PACKAGE_SCHEMA == schema_type
         || ROUTINE_SCHEMA == schema_type
         || UDT_SCHEMA == schema_type
         || TRIGGER_SCHEMA == schema_type;
    if (OB_FAIL(schema_checker_->get_schema_version(
        is_pl_schema_type && (OB_SYS_TENANT_ID == pl::get_tenant_id_by_object_id(object_id)) ? OB_SYS_TENANT_ID : params_.session_info_->get_effective_tenant_id(),
        object_id,
        schema_type,
        schema_version))) {
      LOG_WARN("get schema version failed", K(params_.session_info_->get_effective_tenant_id()),
               K(object_id), K(table_type), K(schema_type), K(ret));
    } else if (OB_UNLIKELY(OB_INVALID_VERSION == schema_version)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("object schema is unknown",
               K(object_id), K(table_type), K(schema_type), K(ret));
    } else {
      obj_version.object_id_ = object_id;
      obj_version.object_type_ = table_type,
      obj_version.version_ = schema_version;
      if (OB_FAIL(get_stmt()->add_global_dependency_table(obj_version))) {
        LOG_WARN("add global dependency table failed",
                 K(ret), K(table_type), K(schema_type));
      }
    }
  }
  return ret;
}

// 将对象加入 schema version 依赖集合中
// 当对象 schema 版本变更时，通过依赖检查对象是否需要重新生成
int ObDMLResolver::add_object_versions_to_dependency(ObDependencyTableType table_type,
                                                    ObSchemaType schema_type,
                                                    const ObIArray<uint64_t> &object_ids)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < object_ids.count(); ++i) {
    if (OB_FAIL(add_object_version_to_dependency(table_type, schema_type, object_ids.at(i)))) {
      LOG_WARN("add object versions to dependency failed");
    }
  }
  return ret;
}

int ObDMLResolver::resolve_table_relation_factor(const ParseNode *node,
                                                 uint64_t tenant_id,
                                                 uint64_t &dblink_id,
                                                 uint64_t &database_id,
                                                 ObString &table_name,
                                                 ObString &synonym_name,
                                                 ObString &synonym_db_name,
                                                 ObString &db_name,
                                                 ObString &dblink_name,
                                                 bool &is_db_explicit)
{
  ObSynonymChecker synonym_checker;
  return resolve_table_relation_factor(node, tenant_id, dblink_id,
                                       database_id, table_name, synonym_name, synonym_db_name,
                                       db_name, dblink_name, is_db_explicit, synonym_checker);
}

int ObDMLResolver::resolve_table_relation_factor(const ParseNode *node,
                                                 uint64_t tenant_id,
                                                 uint64_t &dblink_id,
                                                 uint64_t &database_id,
                                                 ObString &table_name,
                                                 ObString &synonym_name,
                                                 ObString &synonym_db_name,
                                                 ObString &db_name,
                                                 ObString &dblink_name,
                                                 bool &is_db_explicit,
                                                 ObSynonymChecker &synonym_checker)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session info is NULL", K(ret));
  } else if (OB_FAIL(resolve_dblink_name(node, dblink_name))) {
    LOG_WARN("resolve dblink name failed", K(ret));
  } else {
    if (dblink_name.empty()) {
      if (OB_FAIL(resolve_table_relation_factor_normal(node, tenant_id, database_id,
                                                       table_name, synonym_name,
                                                       synonym_db_name, db_name,
                                                       is_db_explicit, synonym_checker))) {
        LOG_WARN("resolve table relation factor failed", K(ret), K(table_name));
        // table_name may be dblink table, here to test is,
        if (OB_ERR_SYNONYM_TRANSLATION_INVALID == ret ||
            OB_TABLE_NOT_EXIST == ret) {
          int tmp_ret = ret;
          ret = OB_SUCCESS;
          if (OB_FAIL(resolve_dblink_with_synonym(tenant_id, table_name, dblink_name,
                                                  db_name, dblink_id))) {
            LOG_WARN("try synonym with dblink failed", K(ret));
            ret = tmp_ret;
            synonym_name.reset();
            synonym_db_name.reset();
          } else if (OB_INVALID_ID == dblink_id) {
            ret = tmp_ret;
            synonym_name.reset();
            synonym_db_name.reset();
          } else { /* do nothing */ }
        } else if (OB_FAIL(ret)) {
          synonym_name.reset();
          synonym_db_name.reset();
        }
      }
    } else {
      if (OB_FAIL(resolve_table_relation_factor_dblink(node, tenant_id,
                                                       dblink_name, dblink_id,
                                                       table_name, db_name))) {
        LOG_WARN("resolve table relation factor from dblink failed", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_dblink_name(const ParseNode *table_node, ObString &dblink_name)
{
  int ret = OB_SUCCESS;
  dblink_name.reset();
  if (!OB_ISNULL(table_node) && table_node->num_child_ > 2 &&
      !OB_ISNULL(table_node->children_) && !OB_ISNULL(table_node->children_[2])) {
    int32_t dblink_name_len = static_cast<int32_t>(table_node->children_[2]->str_len_);
    dblink_name.assign_ptr(table_node->children_[2]->str_value_, dblink_name_len);
  }
  return ret;
}

int ObDMLResolver::resolve_dblink_with_synonym(uint64_t tenant_id, ObString &table_name,
                                              ObString &dblink_name,
                                              ObString &db_name, uint64_t &dblink_id)
{
  int ret = OB_SUCCESS;
   // dblink name must be something like 'db_name.tbl_name@dblink', or 'tbl_name@dblink'
  ObString tmp_table_name;
  ObString dblink_user_name;
  CK (OB_NOT_NULL(allocator_));
  OZ (ob_write_string(*allocator_, table_name, tmp_table_name));
  ObString tbl_sch_name = tmp_table_name.split_on('@');
  if (tbl_sch_name.empty()) {
    // do nothing; not a valid dblink name format
  } else if (tmp_table_name.empty()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tmp_table_name is empty", K(ret));
  } else {
    OZ (schema_checker_->get_dblink_id(tenant_id, tmp_table_name, dblink_id));
    OZ (schema_checker_->get_dblink_user(tenant_id, tmp_table_name, dblink_user_name, *allocator_));
    if (OB_FAIL(ret)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected error", K(ret));
    } else if (OB_INVALID_ID == dblink_id) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalide dblink_id", K(ret));
    } else {
      OZ (ob_write_string(*allocator_, tmp_table_name, dblink_name));
      ObString remote_schema_name;
      CK (!tbl_sch_name.empty());
      OX (remote_schema_name = tbl_sch_name.split_on('.'));
      if (OB_FAIL(ret)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected error", K(ret));
      } else {
        ObString &tmp_db_name = dblink_user_name;
        if (!remote_schema_name.empty() && (0 != dblink_user_name.case_compare(remote_schema_name))) {
          tmp_db_name = remote_schema_name;
        }
        // convert db_name to upper, for the field in all_object is upper
        if (OB_SUCC(ret)) {
          char letter;
          char *src_ptr = tmp_db_name.ptr();
          for(ObString::obstr_size_t i = 0; i < tmp_db_name.length(); ++i) {
            letter = src_ptr[i];
            if(letter >= 'a' && letter <= 'z'){
              src_ptr[i] = static_cast<char>(letter - 32);
            }
          }
        }
        OZ (ob_write_string(*allocator_, tbl_sch_name, table_name));
        OZ (ob_write_string(*allocator_, tmp_db_name, db_name));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_table_relation_factor_normal(const ParseNode *node,
                                                        uint64_t tenant_id,
                                                        uint64_t &database_id,
                                                        ObString &table_name,
                                                        ObString &synonym_name,
                                                        ObString &synonym_db_name,
                                                        ObString &db_name,
                                                        ObSynonymChecker &synonym_checker)
{
  bool is_db_explicit = false;
  UNUSED(is_db_explicit);
  return resolve_table_relation_factor_normal(node, tenant_id, database_id,
                                              table_name, synonym_name, synonym_db_name, db_name,
                                              is_db_explicit, synonym_checker);
}

int ObDMLResolver::resolve_table_relation_factor_normal(const ParseNode *node,
                                                        uint64_t tenant_id,
                                                        uint64_t &database_id,
                                                        ObString &table_name,
                                                        ObString &synonym_name,
                                                        ObString &synonym_db_name,
                                                        ObString &db_name,
                                                        bool &is_db_explicit,
                                                        ObSynonymChecker &synonym_checker)
{
  int ret = OB_SUCCESS;
  database_id = OB_INVALID_ID;
  is_db_explicit = false;
  ObString orig_name;
  ObString out_db_name;
  ObString out_table_name;
  synonym_db_name.reset();

  if (OB_FAIL(resolve_table_relation_node_v2(node, table_name, db_name, is_db_explicit))) {
    LOG_WARN("failed to resolve table relation node!", K(ret));
  } else if (FALSE_IT(orig_name.assign_ptr(table_name.ptr(), table_name.length()))) {
  } else if (FALSE_IT(synonym_db_name.assign_ptr(db_name.ptr(), db_name.length()))) {
  } else if (OB_FAIL(schema_checker_->get_database_id(tenant_id, db_name, database_id))) {
    if (OB_SCHEMA_EAGAIN != ret) {
      ret = OB_ERR_BAD_DATABASE;
      LOG_WARN("Invalid database name, database not exist", K(db_name), K(tenant_id), K(ret));
    }
  } else if (lib::is_oracle_mode() && 0 == db_name.case_compare(OB_SYS_DATABASE_NAME)) {
    ret = OB_ERR_BAD_DATABASE;
    LOG_WARN("Invalid database name, cannot access oceanbase db on Oracle tenant", K(db_name), K(tenant_id), K(ret));
  } else if (OB_FAIL(resolve_table_relation_recursively(tenant_id,
                                                        database_id,
                                                        table_name,
                                                        db_name,
                                                        synonym_checker,
                                                        is_db_explicit))) {
    if (OB_TABLE_NOT_EXIST == ret) {
      if (synonym_checker.has_synonym()) {
        ret = OB_ERR_SYNONYM_TRANSLATION_INVALID;
        LOG_WARN("Synonym translation is no longer valid");
        LOG_USER_ERROR(OB_ERR_SYNONYM_TRANSLATION_INVALID, to_cstring(orig_name));
      }
    }
    // synonym_db_name.reset();
    // synonym_name.reset();
    synonym_name = orig_name;
    LOG_WARN("fail to resolve table relation recursively", K(tenant_id), K(ret));
  } else if (false == synonym_checker.has_synonym()) {
    synonym_name.reset();
    synonym_db_name.reset();
  } else {
    synonym_name = orig_name;
    ObStmt *stmt = get_basic_stmt();
    // 一般的对synonym操作的dml语句stmt不会是NULL，但是类似于desc synonym_name的语句，运行到这里
    // stmt还未生成，因为还未生成从虚拟表select的语句，所以stmt为NULL
    // https://work.aone.alibaba-inc.com/issue/21978477
    if (OB_NOT_NULL(stmt)) {
      if (OB_FAIL(add_synonym_obj_id(synonym_checker, false/* error_with_exist */))) {
        LOG_WARN("add_synonym_obj_id failed", K(ret));
      }
    }
  }

  //table_name and db_name memory may from schema, so deep copy the content to SQL memory
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("allocator is NULL", K(ret));
  } else if (OB_FAIL(ob_write_string(*allocator_, table_name, out_table_name))) {
    LOG_WARN("fail to deep copy string", K(table_name), K(ret));
  } else if (OB_FAIL(ob_write_string(*allocator_, db_name, out_db_name))) {
    LOG_WARN("fail to deep copy string", K(db_name), K(ret));
  } else {
    table_name = out_table_name;
    db_name = out_db_name;
  }
  return ret;
}

int ObDMLResolver::resolve_table_relation_factor_dblink(const ParseNode *table_node,
                                                        const uint64_t tenant_id,
                                                        const ObString &dblink_name,
                                                        uint64_t &dblink_id,
                                                        ObString &table_name,
                                                        ObString &database_name)
{
  int ret = OB_SUCCESS;
  // db name node may null
  ParseNode *dbname_node = table_node->children_[0];
  if (OB_ISNULL(table_node) || OB_ISNULL(table_node->children_) ||
      OB_ISNULL(table_node->children_[1])) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("table node or children is NULL", K(ret));
  } else if (OB_FAIL(schema_checker_->get_dblink_id(tenant_id, dblink_name, dblink_id))) {
    LOG_WARN("failed to get dblink info", K(ret), K(dblink_name));
  } else if (OB_INVALID_ID == dblink_id) {
    ret = OB_DBLINK_NOT_EXIST_TO_ACCESS;
    LOG_WARN("dblink not exist", K(ret), K(tenant_id), K(dblink_name));
  } else {
    if (OB_ISNULL(allocator_)) {
      ret = OB_ERR_NULL_VALUE;
      LOG_WARN("allocator is null", K(ret));
    } else if (OB_FAIL(schema_checker_->get_dblink_user(tenant_id, dblink_name,
                                                        database_name, *allocator_))){
      LOG_WARN("failed to get dblink user name", K(tenant_id), K(database_name));
    } else if (database_name.empty()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("dblink user name is empty", K(ret));
    } else {
      ObString tmp_dbname;
      if (OB_NOT_NULL(dbname_node)) {
        int32_t database_name_len = static_cast<int32_t>(dbname_node->str_len_);
        tmp_dbname.assign_ptr(dbname_node->str_value_, database_name_len);
        // the one saved in the schema may different from the one in the parse node, check it.
        if (0 != database_name.case_compare(tmp_dbname)) {
          LOG_WARN("user name is not same", K(database_name), K(tmp_dbname));
          //At the beginning, OB dblink only considers accessing a specific user of remote.
          //So if it finds that the user accessed by sql is not the specified user, it will directly report an error.
          //This restriction should now be lifted.
          //
          //Example:
          //There are local users user1@tenant1, remote users user1@tenant2 and user2@tenant2.
          //user1@tenant1 establishes a dblink connected to user1@tenant2,
          //theoretically user1@tenant2 should be able to use this connection to access user2@tenant2.
          database_name = tmp_dbname;
        } else {
          // do nothing
        }
      } else {
        // do nothing
      }
      // database name may lower char, translate to upper, for all_object's user name is upper
      // why we use database_name, but not dbname_node, because dbname_node is not always exist
      char letter;
      char *src_ptr = database_name.ptr();
      for(ObString::obstr_size_t i = 0; i < database_name.length(); ++i) {
        letter = src_ptr[i];
        if(letter >= 'a' && letter <= 'z'){
          src_ptr[i] = static_cast<char>(letter - 32);
        }
      }
    }
    if (OB_SUCC(ret)) {
      int32_t table_name_len = static_cast<int32_t>(table_node->children_[1]->str_len_);
      table_name.assign_ptr(table_node->children_[1]->str_value_, table_name_len);
    }
  }
  return ret;
}

int ObDMLResolver::add_reference_obj_table(const uint64_t dep_obj_id,
                                           const uint64_t dep_db_id,
                                           const ObObjectType dep_obj_type,
                                           const ObDependencyTableType ref_table_type,
                                           const ObSchemaType ref_schema_type,
                                           const uint64_t ref_obj_id)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(get_stmt()) || OB_ISNULL(schema_checker_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("stmt or schema_checker is null", K(get_stmt()), K_(schema_checker));
  } else if (OB_ISNULL(params_.session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("params_.session_info_ is null", K(ret));
  } else {
    int64_t schema_version = OB_INVALID_VERSION;
    ObSchemaObjVersion obj_version;
    bool is_pl_schema_type =
         PACKAGE_SCHEMA == ref_schema_type
         || ROUTINE_SCHEMA == ref_schema_type
         || UDT_SCHEMA == ref_schema_type
         || TRIGGER_SCHEMA == ref_schema_type;
    if (OB_FAIL(schema_checker_->get_schema_version(
        is_pl_schema_type && (OB_SYS_TENANT_ID == pl::get_tenant_id_by_object_id(ref_obj_id)) ? OB_SYS_TENANT_ID : params_.session_info_->get_effective_tenant_id(),
        ref_obj_id,
        ref_schema_type,
        schema_version))) {
      LOG_WARN("get schema version failed", K(params_.session_info_->get_effective_tenant_id()),
                K(ref_obj_id), K(ref_table_type), K(ref_schema_type), K(ret));
    } else if (OB_UNLIKELY(OB_INVALID_VERSION == schema_version)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("object schema is unknown",
                K(ref_obj_id), K(ref_table_type), K(ref_schema_type), K(ret));
    } else {
      obj_version.object_id_ = ref_obj_id;
      obj_version.object_type_ = ref_table_type,
      obj_version.version_ = schema_version;
      if (OB_FAIL(get_stmt()->add_ref_obj_version(
          dep_obj_id, dep_db_id, dep_obj_type, obj_version, *allocator_))) {
        LOG_WARN("add reference obj table failed", K(ret), K(dep_obj_id), K(dep_obj_type),
          K(ref_table_type), K(ref_schema_type));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_table_relation_recursively(uint64_t tenant_id,
                                                      uint64_t &database_id,
                                                      ObString &table_name,
                                                      ObString &db_name,
                                                      ObSynonymChecker &synonym_checker,
                                                      bool is_db_explicit)
{
  int ret = OB_SUCCESS;
  bool exist_with_synonym = false;
  ObString object_table_name;
  uint64_t object_db_id;
  uint64_t synonym_id;
  ObString object_db_name;
  ObReferenceObjTable *ref_obj_tbl = NULL;
  ObSchemaGetterGuard *schema_guard = NULL;
  const ObDatabaseSchema *database_schema = NULL;
  const ObSimpleTableSchemaV2 *table_schema = NULL;
  if (!params_.is_from_show_resolver_) {
    CK (OB_NOT_NULL(get_stmt()));
    CK (OB_NOT_NULL(ref_obj_tbl = get_stmt()->get_ref_obj_table()));
  }
  CK (OB_NOT_NULL(schema_guard = schema_checker_->get_schema_guard()));
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(check_table_exist_or_not(tenant_id, database_id, table_name, db_name))) {
    if (OB_TABLE_NOT_EXIST == ret) {      //try again, with synonym
      ret = OB_SUCCESS;
      if (OB_FAIL(schema_checker_->get_synonym_schema(tenant_id, database_id, table_name,
                                                      object_db_id, synonym_id, object_table_name,
                                                      exist_with_synonym, !is_db_explicit))) {
        LOG_WARN("get synonym schema failed", K(tenant_id), K(database_id), K(table_name), K(ret));
      } else if (exist_with_synonym) {
        synonym_checker.set_synonym(true);
        if (OB_FAIL(synonym_checker.add_synonym_id(synonym_id))) {
          LOG_WARN("fail to add synonym id", K(synonym_id), K(database_id), K(table_name), K(object_table_name), K(ret));
        } else if (OB_FAIL(schema_checker_->get_database_schema(tenant_id, object_db_id, database_schema))) {
          LOG_WARN("get db schema failed", K(tenant_id), K(object_db_id), K(ret));
        } else if (OB_ISNULL(database_schema)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get db schema succeed, but schema pointer is null", K(tenant_id), K(object_db_id), K(ret));
        } else {
          db_name = database_schema->get_database_name_str();
          table_name = object_table_name;
          database_id = object_db_id;
          if (OB_FAIL(SMART_CALL(resolve_table_relation_recursively(tenant_id,
                                                                    database_id,
                                                                    table_name,
                                                                    db_name,
                                                                    synonym_checker,
                                                                    is_db_explicit)))) {
            LOG_WARN("fail to resolve table relation", K(tenant_id), K(database_id), K(table_name), K(ret));
          } else if (is_resolving_view_ && 1 == current_view_level_
            && !params_.is_from_show_resolver_) {
            uint64_t dep_obj_id = params_.is_from_create_view_ ? OB_INVALID_ID : view_ref_id_;
            uint64_t dep_db_id = params_.is_from_create_view_ ? OB_INVALID_ID : database_id;
            if (OB_FAIL(add_reference_obj_table(dep_obj_id, dep_db_id, ObObjectType::VIEW,
                DEPENDENCY_SYNONYM, SYNONYM_SCHEMA, synonym_id))) {
              LOG_WARN("failed to add reference obj table", K(ret), K(dep_obj_id), K(dep_db_id));
            }
          }
        }
      } else {
        if (is_resolving_view_ && 1 == current_view_level_ && !params_.is_from_create_view_
          && !params_.is_from_show_resolver_ && lib::is_oracle_mode()) {
          if (OB_FAIL(ref_obj_tbl->set_need_del_schema_dep_obj(view_ref_id_, database_id,
              ObObjectType::VIEW, *allocator_))) {
            LOG_WARN("failed to set need delete schema dependency obj", K(ret), K_(view_ref_id));
          }
        }
        ret = OB_TABLE_NOT_EXIST;
        LOG_WARN("synonym not exist", K(tenant_id), K(database_id), K(table_name), K(ret));
      }
    }
  } else if (is_resolving_view_ && !params_.is_from_show_resolver_
            && !synonym_checker.has_synonym()) {
    uint64_t dep_obj_id = params_.is_from_create_view_ ? OB_INVALID_ID : view_ref_id_;
    uint64_t dep_db_id = params_.is_from_create_view_ ? OB_INVALID_ID : database_id;
    OZ (schema_guard->get_simple_table_schema(tenant_id, database_id, table_name,
       false/*is_index*/, table_schema));
    CK (OB_NOT_NULL(table_schema));
    if (OB_SUCC(ret) && 1 == current_view_level_
      && OB_FAIL(add_reference_obj_table(dep_obj_id, dep_db_id, ObObjectType::VIEW,
      table_schema->is_view_table() ? DEPENDENCY_VIEW : DEPENDENCY_TABLE,
      TABLE_SCHEMA, table_schema->get_table_id()))) {
      LOG_WARN("failed to add reference obj table", K(ret), K(dep_obj_id), K(dep_db_id));
    }
  }
  return ret;
}

int ObDMLResolver::check_table_exist_or_not(uint64_t tenant_id,
                                               uint64_t &database_id,
                                               ObString &table_name,
                                               ObString &db_name)
{
  int ret = OB_SUCCESS;
  database_id = OB_INVALID_ID;
  if (OB_FAIL(schema_checker_->get_database_id(tenant_id, db_name, database_id))) {
    if (OB_SCHEMA_EAGAIN != ret) {
      ret = OB_ERR_BAD_DATABASE;
      LOG_WARN("Invalid database name, database not exist", K(db_name), K(tenant_id));
    }
  } else {
    bool is_exist = false;
    bool select_index_enabled = false;
    const bool is_hidden = session_info_->is_table_name_hidden();
    if (OB_FAIL(session_info_->is_select_index_enabled(select_index_enabled))) {
      LOG_WARN("fail to get select_index_enabled", K(ret));
    } else if ((select_index_enabled && is_select_resolver()) || session_info_->get_ddl_info().is_ddl()) {
      if (OB_FAIL(schema_checker_->check_table_or_index_exists(
                  tenant_id, database_id, table_name, is_hidden, is_exist))) {
        LOG_WARN("fail to check table or index exist", K(tenant_id), K(database_id),
                     K(table_name), K(ret));
      }
    } else {
      const bool is_index = false;
      if (OB_FAIL(schema_checker_->check_table_exists(
                  tenant_id, database_id, table_name, is_index, is_hidden, is_exist))) {
        LOG_WARN("fail to check table or index exist", K(tenant_id), K(database_id),
                     K(table_name), K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      if (!is_exist) {
        ret = OB_TABLE_NOT_EXIST;
        LOG_INFO("table not exist", K(tenant_id), K(database_id), K(table_name), K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_function_table_column_item(const TableItem &table_item,
                                                      const ObDataType &data_type,
                                                      const ObString &column_name,
                                                      uint64_t column_id,
                                                      ColumnItem *&col_item)
{
  int ret = OB_SUCCESS;
  ObColumnRefRawExpr *col_expr = NULL;
  ColumnItem column_item;
  sql::ObExprResType result_type;
  ObDMLStmt *stmt = get_stmt();
  CK (OB_NOT_NULL(stmt));
  CK (OB_NOT_NULL(params_.expr_factory_));
  CK (OB_LIKELY(table_item.is_function_table()));
  OZ (params_.expr_factory_->create_raw_expr(T_REF_COLUMN, col_expr));
  CK (OB_NOT_NULL(col_expr));
  OX (col_expr->set_ref_id(table_item.table_id_, column_id));
  OX (result_type.set_meta(data_type.get_meta_type()));
  OX (result_type.set_accuracy(data_type.get_accuracy()));
  OX (col_expr->set_result_type(result_type));
  if (table_item.get_object_name().empty()) {
    OX (col_expr->set_column_name(column_name));
  } else {
    OX (col_expr->set_column_attr(table_item.get_object_name(), column_name));
  }
  OX (col_expr->set_database_name(table_item.database_name_));
  if (OB_SUCC(ret) && ob_is_enumset_tc(col_expr->get_data_type())) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("not support enum set in table function", K(ret));
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "enum set in table function");
  }
  OX (column_item.expr_ = col_expr);
  OX (column_item.table_id_ = col_expr->get_table_id());
  OX (column_item.column_id_ = col_expr->get_column_id());
  OX (column_item.column_name_ = col_expr->get_column_name());
  OZ (col_expr->extract_info());
  OZ (stmt->add_column_item(column_item));
  OX (col_item = stmt->get_column_item(stmt->get_column_size() - 1));
  return ret;
}

int ObDMLResolver::resolve_function_table_column_item(const TableItem &table_item,
                                                      const ObString &column_name,
                                                      ColumnItem *&col_item)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  CK (OB_NOT_NULL(stmt));
  CK (OB_LIKELY(table_item.is_function_table()));
  if (OB_FAIL(ret)) {
    // do nothing ...
  } else if (NULL != (col_item = stmt->get_column_item(table_item.table_id_, column_name))) {
    //exist, ignore resolve...
  } else {
    ObSEArray<ColumnItem, 16> col_items;
    OZ (ObResolverUtils::check_function_table_column_exist(table_item,
                                                           params_,
                                                           column_name));
    OZ (resolve_function_table_column_item(table_item, col_items));
    for (int64_t i = 0; OB_SUCC(ret) && i < stmt->get_column_size(); ++i) {
      ColumnItem* column_item = stmt->get_column_item(i);
      CK (OB_NOT_NULL(column_item));
      if (OB_SUCC(ret)
          && column_item->table_id_ == table_item.table_id_
          && ObCharset::case_compat_mode_equal(column_item->column_name_, column_name)) {
        col_item = column_item;
        break;
      }
    }
    if (OB_SUCC(ret) && OB_ISNULL(col_item)) {
      ret = OB_ERR_BAD_FIELD_ERROR;
      LOG_WARN("not found column in table function", K(ret), K(column_name));
    }
  }
  return ret;
}

int ObDMLResolver::resolve_function_table_column_item(const TableItem &table_item,
                                                      ObIArray<ColumnItem> &col_items)
{
  int ret = OB_SUCCESS;
  ObRawExpr *table_expr = NULL;
  ColumnItem *col_item = NULL;
  ObDMLStmt *stmt = get_stmt();
  const ObUserDefinedType *user_type = NULL;

  CK (OB_NOT_NULL(stmt));

  CK (OB_LIKELY(table_item.is_function_table()));
  CK (OB_NOT_NULL(table_expr = table_item.function_table_expr_));
  OZ (table_expr->deduce_type(session_info_));
  CK (table_expr->get_udt_id() != OB_INVALID_ID);

  CK (OB_NOT_NULL(schema_checker_))
  ObPLPackageGuard package_guard(params_.session_info_->get_effective_tenant_id());
  OZ (ObResolverUtils::get_user_type(
    params_.allocator_, params_.session_info_, params_.sql_proxy_,
    schema_checker_->get_schema_guard(),
    package_guard,
    table_expr->get_udt_id(), user_type));
  CK (OB_NOT_NULL(user_type));
  if (OB_SUCC(ret) && !user_type->is_collection_type()) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("function table get udf with return type is not table type",
             K(ret), K(user_type->is_collection_type()));
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "udf return type is not table type in function table");
  }
  const ObCollectionType *coll_type = NULL;
  CK (OB_NOT_NULL(coll_type = static_cast<const ObCollectionType*>(user_type)));
  if (OB_SUCC(ret)
      && !coll_type->get_element_type().is_obj_type()
      && !coll_type->get_element_type().is_record_type()) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("not supported udt type", K(ret), K(coll_type->get_user_type_id()));
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "current udt type");
  }
  // 数组的元素类型是普通类型的情况
  if (OB_SUCC(ret) && coll_type->get_element_type().is_obj_type()) {
    CK (OB_NOT_NULL(coll_type->get_element_type().get_data_type()));
    if (OB_FAIL(ret)) { // do nothing ...
    } else if (NULL != (col_item = stmt->get_column_item(table_item.table_id_, ObString("COLUMN_VALUE")))) {
      //exist, ignore resolve...
    } else {
      OZ (resolve_function_table_column_item(table_item,
                                            *(coll_type->get_element_type().get_data_type()),
                                            ObString("COLUMN_VALUE"),
                                            OB_APP_MIN_COLUMN_ID,
                                            col_item));
    }
    CK (OB_NOT_NULL(col_item));
    OZ (col_items.push_back(*col_item));
  }
  // 数组的元素类型是Object的情况, 此时应该输出多列
  if (OB_SUCC(ret) && coll_type->get_element_type().is_record_type()) {
    ObPLPackageGuard package_guard(params_.session_info_->get_effective_tenant_id());
    const ObRecordType *record_type = NULL;
    const ObUserDefinedType *user_type = NULL;
    CK (OB_NOT_NULL(schema_checker_))
    OZ (ObResolverUtils::get_user_type(
      params_.allocator_, params_.session_info_, params_.sql_proxy_,
      schema_checker_->get_schema_guard(),
      package_guard,
      coll_type->get_element_type().get_user_type_id(),
      user_type));
    CK (OB_NOT_NULL(user_type));
    if (OB_SUCC(ret) && !user_type->is_record_type()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("elem type is not record type",
               K(ret), K(user_type->get_type()),
               K(coll_type->get_element_type().get_user_type_id()),
               K(coll_type->get_user_type_id()));
    }
    CK (OB_NOT_NULL(record_type = static_cast<const ObRecordType *>(user_type)));
    for (int64_t i = 0; OB_SUCC(ret) && i < record_type->get_member_count(); ++i) {
      const ObPLDataType *pl_type = record_type->get_record_member_type(i);
      ObString column_name;
      OX (col_item = NULL);
      CK (OB_NOT_NULL(pl_type));
      CK (OB_NOT_NULL(record_type->get_record_member_name(i)));
      OZ (ob_write_string(*(params_.allocator_), *(record_type->get_record_member_name(i)), column_name));
      CK (OB_NOT_NULL(column_name));
      if (OB_SUCC(ret) && !pl_type->is_obj_type()) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "table(coll(object)) : object`s element is not basic type");
        LOG_WARN("table(coll(object)) : object`s element is not basic type not supported", K(ret), KPC(pl_type));
      }
      CK (OB_NOT_NULL(pl_type->get_data_type()));
      if (OB_FAIL(ret)) { // do nothing ...
      } else if (NULL != (col_item = stmt->get_column_item(table_item.table_id_, column_name))) {
        //exist, ignore resolve...
      } else {
        OZ (resolve_function_table_column_item(table_item,
                                               *(pl_type->get_data_type()),
                                               column_name,
                                               OB_APP_MIN_COLUMN_ID + i,
                                               col_item));
      }
      CK (OB_NOT_NULL(col_item));
      OZ (col_items.push_back(*col_item));
    }
  }

  return ret;
}

/*  resolve_generated_table_column_item will traverse the select_items again if the name not exists.
  if the col_name is not exist, the function traverses the select_items utill find the select item wil the same name
  and then copy this selected item to the col_item.
  @param select_item_offset:
    the argument select_item_offset is used to tell the function to traverse select_items from the select_item_offset-th select item.
  @param skip_check:
    bugfix: https://work.aone.alibaba-inc.com/issue/36334616
    if the all the three conditions are true, we can skip the check and directly copy the select_item to column_item:
    1. the function is called directly or indirectly from reslove_star. (e.g., in select * from xxxx)
    2. is oracle mode
    3. the column to be checked is a duplicable column in joined table (excepet the using).
      for example:  t1 (c1, c2, c3), t2 (c2, c3, c4)
      in 'select * from t1 left join t2 using (c3)', c2 is the duplicable column, c3 is not duplicable since c3 is in using condition.
*/
int ObDMLResolver::resolve_generated_table_column_item(const TableItem &table_item,
                                                       const common::ObString &column_name,
                                                       ColumnItem *&col_item,
                                                       ObDMLStmt *stmt /* = NULL */,
                                                       const uint64_t column_id /* = OB_INVALID_ID */,
                                                       const int64_t select_item_offset /* = 0 */,
                                                       const bool skip_check /* = false */)
{
  int ret = OB_SUCCESS;
  ObColumnRefRawExpr *col_expr = NULL;
  ColumnItem column_item;
  if (NULL == stmt) {
    stmt = get_stmt();
  }
  if (OB_ISNULL(stmt) || OB_ISNULL(schema_checker_) || OB_ISNULL(params_.expr_factory_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema checker is null", K(stmt), K_(schema_checker), K_(params_.expr_factory));
  } else if (OB_UNLIKELY(!table_item.is_generated_table() &&
                         !table_item.is_fake_cte_table() &&
                         !table_item.is_temp_table())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("not generated table", K_(table_item.type));
  }
  bool found = false;
  if (OB_SUCC(ret) && !(skip_check)) {
    if (OB_INVALID_ID != column_id) {
      col_item = stmt->get_column_item_by_id(table_item.table_id_, column_id);
    } else {
      col_item = stmt->get_column_item(table_item.table_id_, column_name);
    }
    found = NULL != col_item;
  }
  if (OB_SUCC(ret) && !found) {
    ObSelectStmt *ref_stmt = table_item.ref_query_;
    bool is_break = false;
    if (OB_ISNULL(ref_stmt)) {
      ret = OB_NOT_INIT;
      LOG_WARN("generate table ref stmt is null");
    }

    int64_t i = select_item_offset;
    for (; OB_SUCC(ret) && !is_break && i < ref_stmt->get_select_item_size(); ++i) {
      SelectItem &ref_select_item = ref_stmt->get_select_item(i);
      if (column_id != OB_INVALID_ID
            ? i + OB_APP_MIN_COLUMN_ID == column_id
            : ObCharset::case_compat_mode_equal(column_name, ref_select_item.alias_name_)) {
        ObRawExpr *select_expr = ref_select_item.expr_;
        if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_REF_COLUMN, col_expr))) {
          LOG_WARN("create column expr failed", K(ret));
        } else if (OB_ISNULL(select_expr)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("select expr is null");
        } else if (OB_FAIL(select_expr->deduce_type(session_info_))) {
          LOG_WARN("deduce select expr type failed", K(ret));
        } else {
          //because of view table, generated table item may be has database_name and table name,
          //also alias name maybe be empty
          col_expr->set_ref_id(table_item.table_id_, i + OB_APP_MIN_COLUMN_ID);
          col_expr->set_result_type(select_expr->get_result_type());
          col_expr->set_column_attr(table_item.get_table_name(), ref_select_item.alias_name_);
          col_expr->set_database_name(table_item.database_name_);
          col_expr->set_unpivot_mocked_column(ref_select_item.is_unpivot_mocked_column_);
          //set enum_set_values
          if (ob_is_enumset_tc(select_expr->get_data_type())) {
            if (OB_FAIL(col_expr->set_enum_set_values(select_expr->get_enum_set_values()))) {
              LOG_WARN("failed to set_enum_set_values", K(ret));
            }
          }
          is_break = true;

          if (OB_FAIL(ret)) {
            //do nothing
          } else if (OB_FAIL(ObResolverUtils::resolve_default_value_and_expr_from_select_item(ref_select_item,
                                                                                     column_item,
                                                                                     ref_stmt))) {
            if (ret == OB_ERR_BAD_FIELD_ERROR) {
              // ignore the NOT_FOUND error, since it might be rowid.
              ret = OB_SUCCESS;
            }
          } else if (OB_FAIL(erase_redundant_generated_table_column_flag(*ref_stmt, select_expr,
                                                                         *col_expr))) {
            LOG_WARN("erase redundant generated table column flag failed", K(ret));
          } else {
            if (select_expr->is_column_ref_expr()) {
              ObColumnRefRawExpr *col_ref = static_cast<ObColumnRefRawExpr *>(select_expr);
              if (!ObCharset::case_insensitive_equal(OB_HIDDEN_LOGICAL_ROWID_COLUMN_NAME, col_ref->get_column_name())) {
                col_expr->set_joined_dup_column(col_ref->is_joined_dup_column());
                ColumnItem *item = ref_stmt->get_column_item_by_id(col_ref->get_table_id(), col_ref->get_column_id());
                if (OB_ISNULL(item)) {
                  ret = OB_ERR_UNEXPECTED;
                  LOG_WARN("column item should not be null", K(ret));
                } else {
                  column_item.base_tid_ = item->base_tid_;
                  column_item.base_cid_ = item->base_cid_;
                }
              }
            }
          }
        }
      }
    }
    if (OB_FAIL(ret)) {
      // do nothing
    } else if (OB_ISNULL(col_expr)) {
      ret = OB_ERR_BAD_FIELD_ERROR;
      LOG_WARN("col_expr is nully, it maybe rowid", K(ret), K(column_name), K(column_id));
    } else if (ObCharset::case_insensitive_equal(OB_HIDDEN_LOGICAL_ROWID_COLUMN_NAME,
                                                 col_expr->get_column_name())) {
      if (stmt->is_select_stmt()) {
        ObRawExpr *empty_rowid_expr = NULL;
        if (OB_FAIL(ObRawExprUtils::build_empty_rowid_expr(*params_.expr_factory_,
                                                           table_item.table_id_,
                                                           empty_rowid_expr))) {
          LOG_WARN("build empty rowid expr failed", K(ret));
        } else if (OB_ISNULL(empty_rowid_expr) ||
                   OB_UNLIKELY(!empty_rowid_expr->is_column_ref_expr())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get unexpected error", K(ret), KPC(empty_rowid_expr));
        } else {
          col_expr = static_cast<ObColumnRefRawExpr*>(empty_rowid_expr);
        }
      }
    } else {/*do nothing*/}
    //init column item
    if (OB_SUCC(ret)) {
      column_item.expr_ = col_expr;
      column_item.table_id_ = col_expr->get_table_id();
      column_item.column_id_ = col_expr->get_column_id();
      column_item.column_name_ = col_expr->get_column_name();
      if (OB_FAIL(col_expr->extract_info())) {
        LOG_WARN("extract column expr info failed", K(ret));
      } else if (OB_FAIL(stmt->add_column_item(column_item))) {
        LOG_WARN("add column item to stmt failed", K(ret));
      } else {
        col_item = stmt->get_column_item(stmt->get_column_size() - 1);
      }
    }
  }
  return ret;
}

int ObDMLResolver::erase_redundant_generated_table_column_flag(const ObSelectStmt &ref_stmt,
                                                                  const ObRawExpr *ref_expr,
                                                                  ObColumnRefRawExpr &col_expr) const
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ref_expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ref_expr is null");
  } else if (ref_expr->is_column_ref_expr()) {
    bool is_null = false;
    const ObColumnRefRawExpr &ref_col_expr = static_cast<const ObColumnRefRawExpr&>(*ref_expr);
    if (OB_FAIL(ObOptimizerUtil::is_table_on_null_side(&ref_stmt, ref_col_expr.get_table_id(), is_null))) {
      LOG_WARN("is table on null side failed", K(ret));
    } else if (is_null) {
      col_expr.unset_result_flag(NOT_NULL_FLAG);
      col_expr.unset_result_flag(AUTO_INCREMENT_FLAG);
      col_expr.unset_result_flag(PRI_KEY_FLAG);
      col_expr.unset_result_flag(PART_KEY_FLAG);
      col_expr.unset_result_flag(MULTIPLE_KEY_FLAG);
    }
  }
  return ret;
}

int ObDMLResolver::build_prefix_index_compare_expr(ObRawExpr &column_expr,
                                                   ObRawExpr *prefix_expr,
                                                   ObItemType type,
                                                   ObRawExpr &value_expr,
                                                   ObRawExpr *escape_expr,
                                                   ObRawExpr *&new_op_expr)
{
  int ret = OB_SUCCESS;
  ObSysFunRawExpr *substr_expr = NULL;
  if (T_OP_LIKE == type) {
    //build value substr expr
    ObOpRawExpr *like_expr = NULL;
    if (OB_FAIL(ObRawExprUtils::create_substr_expr(*params_.expr_factory_,
                                                   params_.session_info_,
                                                   &value_expr,
                                                   prefix_expr->get_param_expr(1),
                                                   prefix_expr->get_param_expr(2),
                                                   substr_expr))) {
      LOG_WARN("create substr expr failed", K(ret));
    } else if (OB_FAIL(ObRawExprUtils::build_like_expr(*params_.expr_factory_,
                                                       params_.session_info_,
                                                       &column_expr,
                                                       substr_expr,
                                                       escape_expr,
                                                       like_expr))) {
      LOG_WARN("build like expr failed", K(ret));
    } else {
      new_op_expr = like_expr;
    }
  } else {
    ObRawExpr *right_expr = NULL;
    if (T_OP_IN == type) {
      ObOpRawExpr *row_expr = NULL;
      if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_OP_ROW, row_expr))) {
        LOG_WARN("create to_type expr failed", K(ret));
      } else if (OB_ISNULL(row_expr)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("to_type is null");
      } else {
        right_expr = row_expr;
        for (int64_t k = 0; OB_SUCC(ret) && k < value_expr.get_param_count(); ++k) {
          if (OB_FAIL(ObRawExprUtils::create_substr_expr(*params_.expr_factory_,
                                                         params_.session_info_,
                                                         value_expr.get_param_expr(k),
                                                         prefix_expr->get_param_expr(1),
                                                         prefix_expr->get_param_expr(2),
                                                         substr_expr))) {
            LOG_WARN("create substr expr failed", K(ret));
          } else if (OB_FAIL(row_expr->add_param_expr(substr_expr))) {
            LOG_WARN("set param expr failed", K(ret));
          }
        }
      }
    } else {
      if (OB_FAIL(ObRawExprUtils::create_substr_expr(*params_.expr_factory_,
                                                     params_.session_info_,
                                                     &value_expr,
                                                     prefix_expr->get_param_expr(1),
                                                     prefix_expr->get_param_expr(2),
                                                     substr_expr))) {
        LOG_WARN("create substr expr failed", K(ret));
      } else {
        right_expr = substr_expr;
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(ObRawExprUtils::create_double_op_expr(*params_.expr_factory_,
                                                        params_.session_info_,
                                                        type,
                                                        new_op_expr,
                                                        &column_expr,
                                                        right_expr))) {
        LOG_WARN("failed to create double op expr", K(ret), K(type), K(column_expr), KPC(right_expr));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_sample_clause(const ParseNode *sample_node,
                                         const uint64_t table_id)
{
  int ret = OB_SUCCESS;
  ObSelectStmt *stmt;
  if (OB_ISNULL(get_stmt())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("stmt should not be NULL", K(ret));
  } else if (OB_UNLIKELY(!get_stmt()->is_select_stmt())) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "sampling in dml statement");
  } else {
    stmt = static_cast<ObSelectStmt *>(get_stmt());
    enum SampleNode { METHOD = 0, PERCENT = 1, SEED = 2, SCOPE = 3};
    if (OB_ISNULL(sample_node) || OB_ISNULL(sample_node->children_[METHOD])
        || OB_ISNULL(sample_node->children_[PERCENT])) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("sample node should not be NULL", K(ret));
    } else {
      SampleInfo sample_info;
      sample_info.table_id_ = table_id;
      if (sample_node->children_[METHOD]->value_ == 2) {
        sample_info.method_ = SampleInfo::BLOCK_SAMPLE;
      } else {
        sample_info.method_ = SampleInfo::ROW_SAMPLE;
      }

      sample_info.percent_ = 0;
      if (sample_node->children_[PERCENT]->type_ == T_SFU_INT) {
        sample_info.percent_ = static_cast<double>(sample_node->children_[PERCENT]->value_);
      } else if (sample_node->children_[PERCENT]->type_ == T_SFU_DECIMAL) {
        ObString str_percent(sample_node->children_[PERCENT]->str_len_,
                             sample_node->children_[PERCENT]->str_value_);
        if (OB_FAIL(ObOptEstObjToScalar::convert_string_to_scalar_for_number(str_percent, sample_info.percent_))) {
          LOG_WARN("failed to convert string to number", K(ret));
        } else { /*do nothing*/}
      } else {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected node type for sample percent", K(ret));
      }
      if (OB_FAIL(ret)) {
        // do nothing
      } else {
        if (sample_node->children_[SEED] != NULL) {
          sample_info.seed_ = sample_node->children_[SEED]->value_;
        } else {
          // seed is set to -1 when not provided and we will pick a random seed in this case.
          sample_info.seed_ = -1;
        }
        // resolve sample scope
        if (OB_FAIL(ret)) {
          // do nothing
        } else if (OB_ISNULL(sample_node->children_[SCOPE])) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("sample scope should not be null", K(ret));
        } else if (sample_node->children_[SCOPE]->type_ == T_ALL) {
          sample_info.scope_ = SampleInfo::SampleScope::SAMPLE_ALL_DATA;
        } else if (sample_node->children_[SCOPE]->type_ == T_BASE) {
          sample_info.scope_ = SampleInfo::SampleScope::SAMPLE_BASE_DATA;
        } else if (sample_node->children_[SCOPE]->type_ == T_INCR) {
          sample_info.scope_ = SampleInfo::SampleScope::SAMPLE_INCR_DATA;
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unrecognized sample scope", K(ret), K(sample_node->children_[SCOPE]->type_));
        }
        if (OB_FAIL(ret)) {
          // do nothing
        } else if (OB_FAIL(stmt->add_sample_info(sample_info))) {
          LOG_WARN("add sample info failed", K(ret), K(sample_info));
        }
      }
      if (OB_FAIL(ret)) {
        // do nothing
      } else if (sample_info.percent_ < 0.000001 || sample_info.percent_ >= 100.0) {
        ret = OB_ERR_INVALID_SAMPLING_RANGE;
      } else if (sample_info.seed_ != -1 && (sample_info.seed_ > (4294967295))) {
        // 官方文档里的限制[0, 4294967295(2^32 - 1)]
        // 实际测试的时候ORACLE除了限制大于等于0以外，并没有对seed的数值做限制
        // 这里打日志记录一下
        LOG_WARN("seed value out of range");
      }
    }
  }

  return ret;
}

int ObDMLResolver::resolve_transpose_columns(const ParseNode &column_node,
    ObIArray<ObString> &columns)
{
  int ret = OB_SUCCESS;
  if (column_node.type_ == T_COLUMN_LIST) {
    if (OB_UNLIKELY(column_node.num_child_ <= 0)
        || OB_ISNULL(column_node.children_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("column_node is unexpected", K(column_node.num_child_),
               KP(column_node.children_), K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < column_node.num_child_; ++i) {
        const ParseNode *tmp_node = column_node.children_[i];
        if (OB_ISNULL(tmp_node)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("column_node is unexpected", KP(tmp_node), K(ret));
        } else {
          ObString column_name_(tmp_node->str_len_, tmp_node->str_value_);
          if (OB_FAIL(columns.push_back(column_name_))) {
            LOG_WARN("fail to push_back column_name_", K(column_name_), K(ret));
          }
        }
      }
    }
  } else {
    ObString column_name(column_node.str_len_, column_node.str_value_);
    if (OB_FAIL(columns.push_back(column_name))) {
      LOG_WARN("fail to push_back column_name", K(column_name), K(ret));
    }
  }
  return ret;
}

int ObDMLResolver::resolve_const_exprs(const ParseNode &expr_node,
    ObIArray<ObRawExpr *> &const_exprs)
{
  int ret = OB_SUCCESS;
  ObRawExpr *const_expr = NULL;
  if (expr_node.type_ == T_EXPR_LIST) {
    for (int64_t i = 0; OB_SUCC(ret) && i < expr_node.num_child_; ++i) {
      const ParseNode *tmp_node = expr_node.children_[i];
      const_expr = NULL;
      if (OB_ISNULL(tmp_node)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("tmp_node is unexpected", KP(tmp_node), K(ret));
      } else if (OB_FAIL(ObResolverUtils::resolve_const_expr(params_,
          *tmp_node, const_expr, NULL))) {
        LOG_WARN("fail to resolve_const_expr", K(ret));
      } else if (OB_UNLIKELY(!const_expr->is_const_expr())
                 || OB_UNLIKELY(const_expr->has_flag(ObExprInfoFlag::CNT_CUR_TIME))) {
        ret = OB_ERR_NON_CONST_EXPR_IS_NOT_ALLOWED_FOR_PIVOT_UNPIVOT_VALUES;
        LOG_WARN("non-constant expression is not allowed for pivot|unpivot values",
                 KPC(const_expr), K(ret));
      } else if (OB_FAIL(const_exprs.push_back(const_expr))) {
        LOG_WARN("fail to push_back const_expr", KPC(const_expr), K(ret));
      }
    }
  } else {
    if (OB_FAIL(ObResolverUtils::resolve_const_expr(params_,
        expr_node, const_expr, NULL))) {
      LOG_WARN("fail to resolve_const_expr", K(ret));
    } else if (OB_UNLIKELY(!const_expr->is_const_expr())
               || OB_UNLIKELY(const_expr->has_flag(ObExprInfoFlag::CNT_CUR_TIME))) {
      ret = OB_ERR_NON_CONST_EXPR_IS_NOT_ALLOWED_FOR_PIVOT_UNPIVOT_VALUES;
      LOG_WARN("non-constant expression is not allowed for pivot|unpivot values",
               KPC(const_expr), K(ret));
    } else if (OB_FAIL(const_exprs.push_back(const_expr))) {
      LOG_WARN("fail to push_back const_expr", KPC(const_expr), K(ret));
    }
  }
  return ret;
}

int ObDMLResolver::check_pivot_aggr_expr(ObRawExpr *expr) const
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(expr) || OB_UNLIKELY(!expr->has_flag(IS_AGG))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("aggr expr_ is unexpected", KPC(expr), K(ret));
  } else if (expr->get_expr_type() == T_FUN_GROUP_CONCAT) {
    //succ
  } else if (OB_UNLIKELY(static_cast<ObAggFunRawExpr *>(expr)->get_real_param_count() > 1)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("aggr expr_ has invalid argument", KPC(expr), K(ret));
  } else {
    switch (expr->get_expr_type()) {
      case T_FUN_MAX:
      case T_FUN_MIN:
      case T_FUN_SUM:
      case T_FUN_COUNT:
      case T_FUN_AVG:
      case T_FUN_APPROX_COUNT_DISTINCT:
      case T_FUN_STDDEV:
      case T_FUN_VARIANCE: {
        break;
      }
      default: {
        ret = OB_ERR_EXPECT_AGGREGATE_FUNCTION_INSIDE_PIVOT_OPERATION;
        LOG_WARN("expect aggregate function inside pivot operation", KPC(expr), K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_transpose_clause(const ParseNode &transpose_node,
    TransposeItem &transpose_item, ObIArray<ObString> &columns_in_aggrs)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(transpose_node.type_ != T_PIVOT && transpose_node.type_ != T_UNPIVOT)
      || OB_UNLIKELY(transpose_node.num_child_ != 4)
      || OB_ISNULL(transpose_node.children_)
      || OB_ISNULL(transpose_node.children_[0])
      || OB_ISNULL(transpose_node.children_[1])
      || OB_ISNULL(transpose_node.children_[2])
      || OB_ISNULL(transpose_node.children_[3])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("transpose_node is unexpected", K(transpose_node.type_), K(transpose_node.num_child_),
             KP(transpose_node.children_), K(ret));
  } else if (T_PIVOT == transpose_node.type_) {
    transpose_item.set_pivot();

    //pivot aggr
    const ParseNode &aggr_node = *transpose_node.children_[0];
    if (OB_UNLIKELY(aggr_node.type_ != T_PIVOT_AGGR_LIST)
        || OB_UNLIKELY(aggr_node.num_child_ <= 0)
        || OB_ISNULL(aggr_node.children_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("aggr_node is unexpected", K(aggr_node.type_), K(aggr_node.num_child_),
               KP(aggr_node.children_), K(ret));
    } else {
      ObArray<ObQualifiedName> qualified_name_in_aggr;
      for (int64_t i = 0; OB_SUCC(ret) && i < aggr_node.num_child_; ++i) {
        const ParseNode *tmp_node = aggr_node.children_[i];
        const ParseNode *alias_node = NULL;
        TransposeItem::AggrPair aggr_pair;
        qualified_name_in_aggr.reuse();
        if (OB_ISNULL(tmp_node)
            || OB_UNLIKELY(tmp_node->type_ != T_PIVOT_AGGR)
            || OB_UNLIKELY(tmp_node->num_child_ != 2)
            || OB_ISNULL(tmp_node->children_)
            || OB_ISNULL(tmp_node->children_[0])) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("tmp_node is unexpected", KP(tmp_node), K(ret));
        } else if (OB_FAIL(resolve_sql_expr(*tmp_node->children_[0],
                                            aggr_pair.expr_,
                                            &qualified_name_in_aggr))) {
          LOG_WARN("fail to resolve_sql_expr", K(ret));
        } else if (OB_FAIL(check_pivot_aggr_expr(aggr_pair.expr_))) {
          LOG_WARN("fail to check_aggr_expr", K(ret));
        } else if (NULL != (alias_node = tmp_node->children_[1])
                   && FALSE_IT(aggr_pair.alias_name_.assign_ptr(alias_node->str_value_,
                                                                alias_node->str_len_))) {
        } else if (OB_FAIL(transpose_item.aggr_pairs_.push_back(aggr_pair))) {
          LOG_WARN("fail to push_back aggr_pair", K(aggr_pair), K(ret));
        } else {
          for (int64_t j = 0; OB_SUCC(ret) && j < qualified_name_in_aggr.count(); ++j) {
            ObString &column_name = qualified_name_in_aggr.at(j).col_name_;
            if (!has_exist_in_array(columns_in_aggrs, column_name)) {
              if (OB_FAIL(columns_in_aggrs.push_back(column_name))) {
                LOG_WARN("fail to push_back column_name", K(column_name), K(ret));
              }
            }
          }
        }
      }
    }

    //pivot for
    if (OB_SUCC(ret)) {
      if (OB_FAIL(resolve_transpose_columns(*transpose_node.children_[1],
                                                transpose_item.for_columns_))) {
        LOG_WARN("fail to resolve_transpose_columns", K(ret));
      }
    }

    //pivot in
    if (OB_SUCC(ret)) {
      const ParseNode &in_node = *transpose_node.children_[2];
      if (OB_UNLIKELY(in_node.type_ != T_PIVOT_IN_LIST)
          || OB_UNLIKELY(in_node.num_child_ <= 0)
          || OB_ISNULL(in_node.children_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("in_node is unexpected", K(in_node.type_), K(in_node.num_child_),
                 KP(in_node.children_), K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < in_node.num_child_; ++i) {
          const ParseNode *column_node = in_node.children_[i];
          const ParseNode *alias_node = NULL;
          TransposeItem::InPair in_pair;
          if (OB_ISNULL(column_node)
              || OB_UNLIKELY(column_node->type_ != T_PIVOT_IN)
              || OB_UNLIKELY(column_node->num_child_ != 2)
              || OB_ISNULL(column_node->children_)
              || OB_ISNULL(column_node->children_[0])) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("column_node is unexpected", KP(column_node), K(ret));
          } else if (OB_FAIL(resolve_const_exprs(*column_node->children_[0],
                                                 in_pair.exprs_))) {
            LOG_WARN("fail to resolve_const_exprs", K(ret));
          } else if (OB_UNLIKELY(in_pair.exprs_.count() != transpose_item.for_columns_.count())) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("in expr number is equal for columns", K(in_pair.exprs_),
                     K(transpose_item.for_columns_), K(ret));
          } else if (NULL != (alias_node =  column_node->children_[1])) {
            if (OB_UNLIKELY(alias_node->str_len_ > OB_MAX_COLUMN_NAME_LENGTH)) {
              ret = OB_ERR_TOO_LONG_IDENT;
              LOG_WARN("alias name for pivot is too long", K(ret),
                       K(ObString(alias_node->str_len_, alias_node->str_value_)));
            } else {
              in_pair.pivot_expr_alias_.assign_ptr(alias_node->str_value_, alias_node->str_len_);
            }
          }
          if (OB_SUCC(ret) && OB_FAIL(transpose_item.in_pairs_.push_back(in_pair))) {
            LOG_WARN("fail to push_back in_pair", K(in_pair), K(ret));
          }
        }//end of for in node
      }
    }

    //alias
    if (OB_SUCC(ret)) {
      const ParseNode &alias = *transpose_node.children_[3];
      if (alias.str_len_ > 0 && alias.str_value_ != NULL) {
        transpose_item.alias_name_.assign_ptr(alias.str_value_, alias.str_len_);
      }
    }
  } else {
    transpose_item.set_unpivot();
    transpose_item.set_include_nulls(2 == transpose_node.value_);

    //unpivot column
    if (OB_FAIL(resolve_transpose_columns(*transpose_node.children_[0],
                                      transpose_item.unpivot_columns_))) {
      LOG_WARN("fail to resolve_transpose_columns", K(ret));

    //unpivot for
    } else if (OB_FAIL(resolve_transpose_columns(*transpose_node.children_[1],
                                                 transpose_item.for_columns_))) {
      LOG_WARN("fail to resolve_transpose_columns", K(ret));

    //unpivot in
    } else {
      const ParseNode &in_node = *transpose_node.children_[2];
      if (OB_UNLIKELY(in_node.type_ != T_UNPIVOT_IN_LIST)
          || OB_UNLIKELY(in_node.num_child_ <= 0)
          || OB_ISNULL(in_node.children_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("in_node is unexpected", K(in_node.type_), K(in_node.num_child_),
                 KP(in_node.children_), K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < in_node.num_child_; ++i) {
          const ParseNode *column_node = in_node.children_[i];
          TransposeItem::InPair in_pair;
          if (OB_ISNULL(column_node)
              || OB_UNLIKELY(column_node->type_ != T_UNPIVOT_IN)
              || OB_UNLIKELY(column_node->num_child_ != 2)
              || OB_ISNULL(column_node->children_)
              || OB_ISNULL(column_node->children_[0])) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("column_node is unexpectedl", KP(column_node), K(ret));
          } else if (OB_FAIL(resolve_transpose_columns(*column_node->children_[0],
                                                       in_pair.column_names_))) {
            LOG_WARN("fail to resolve_transpose_columns", K(ret));
          } else if (OB_UNLIKELY(in_pair.column_names_.count()
                                 != transpose_item.unpivot_columns_.count())) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unpivot column count is not match for in column count", K(transpose_item),
                     K(in_pair), K(ret));
          } else if (NULL != column_node->children_[1]) {
            if (OB_FAIL(resolve_const_exprs(*column_node->children_[1], in_pair.exprs_))) {
              LOG_WARN("fail to resolve_const_exprs", K(ret));
            } else if (OB_UNLIKELY(in_pair.exprs_.count() != transpose_item.for_columns_.count())){
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("in column count is not match in literal count", K(transpose_item),
                       K(in_pair), K(ret));
            } else if (OB_FAIL(transpose_item.in_pairs_.push_back(in_pair))) {
              LOG_WARN("fail to push_back in_pair", K(in_pair), K(ret));
            }
          } else if (OB_FAIL(transpose_item.in_pairs_.push_back(in_pair))) {
            LOG_WARN("fail to push_back in_pair", K(in_pair), K(ret));
          }
        }//end of for in node
      }
    }//end of in

    //alias
    if (OB_SUCC(ret)) {
      const ParseNode &alias = *transpose_node.children_[3];
      if (alias.str_len_ > 0 && alias.str_value_ != NULL) {
        transpose_item.alias_name_.assign_ptr(alias.str_value_, alias.str_len_);
      }
    }
  }
  LOG_DEBUG("finish resolve_transpose_clause", K(transpose_item), K(columns_in_aggrs), K(ret));
  return ret;
}

int ObDMLResolver::resolve_transpose_table(const ParseNode *transpose_node,
    TableItem *&table_item)
{
  int ret = OB_SUCCESS;
  void *ptr = NULL;
  if (transpose_node == NULL) {
    //do nothing
  } else if (OB_ISNULL(params_.expr_factory_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("expr_factory_ is null",  K(ret));
  } else if (OB_ISNULL(ptr = params_.expr_factory_->get_allocator().alloc(sizeof(TransposeItem)))) {
    ret = common::OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("no more memory to create TransposeItem");
  } else {
    TransposeItem *transpose_item = new(ptr) TransposeItem();
    TableItem *orig_table_item = table_item;
    table_item = NULL;
    ObSEArray<ObString, 16> columns_in_aggrs;
    ObSqlString transpose_def;
    if (OB_ISNULL(orig_table_item)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table_item or stmt is unexpected", KP(orig_table_item), K(ret));
    } else if (OB_FAIL(column_namespace_checker_.add_reference_table(orig_table_item))) {
      LOG_WARN("add reference table to namespace checker failed", K(ret));
    } else if (OB_FAIL(resolve_transpose_clause(*transpose_node,
                                                *transpose_item,
                                                columns_in_aggrs))) {
      LOG_WARN("resolve transpose clause failed", K(ret));
    } else if (OB_FAIL(get_transpose_target_sql(columns_in_aggrs,
                                                *orig_table_item,
                                                *transpose_item,
                                                transpose_def))) {
      LOG_WARN("fail to get_transpose_target_sql", KPC(orig_table_item), K(ret));
    } else if (OB_FAIL(remove_orig_table_item(*orig_table_item))) {
      LOG_WARN("remove_orig_table_item failed", K(ret));
    } else if (OB_FAIL(expand_transpose(transpose_def, *transpose_item, table_item))) {
      LOG_WARN("expand_transpose failed", K(ret));
    }
  }
  return ret;
}

int ObDMLResolver::expand_transpose(const ObSqlString &transpose_def,
    TransposeItem &transpose_item, TableItem *&table_item)
{
  int ret = OB_SUCCESS;
  ObParser parser(*params_.allocator_, session_info_->get_sql_mode());
  ParseResult transpose_result;
  if (OB_FAIL(parser.parse(transpose_def.string(), transpose_result))) {
    LOG_WARN("parse view defination failed", K(transpose_def.string()), K(ret));
  } else if (OB_ISNULL(transpose_result.result_tree_)
             || OB_ISNULL(transpose_result.result_tree_->children_)
             || OB_UNLIKELY(transpose_result.result_tree_->num_child_ < 1)
             || OB_ISNULL(transpose_result.result_tree_->children_[0])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("transpose_result.result_tree_ is null", K(transpose_result.result_tree_), K(ret));
  } else {
    //use select resolver
    ObSelectResolver select_resolver(params_);
    //from子查询和当前查询属于平级，因此current level和当前保持一致
    select_resolver.set_current_level(current_level_);
    select_resolver.set_is_sub_stmt(true);
    select_resolver.set_parent_namespace_resolver(parent_namespace_resolver_);
    select_resolver.set_current_view_level(current_view_level_);
    select_resolver.set_transpose_item(&transpose_item);
    if (OB_FAIL(add_cte_table_to_children(select_resolver))) {
      LOG_WARN("add_cte_table_to_children failed", K(ret));
    } else if (OB_FAIL(do_resolve_generate_table(*transpose_result.result_tree_->children_[0],
                                                 transpose_item.alias_name_,
                                                 select_resolver,
                                                 table_item))) {
      LOG_WARN("do_resolve_generate_table failed", K(ret));
    } else if (OB_FAIL(mark_unpivot_table(transpose_item, table_item))) {
      LOG_WARN("fail to mark_unpivot_table", KPC(table_item), K(ret));
    }
    LOG_DEBUG("finish expand_transpose", K(transpose_def), K(transpose_item), KPC(table_item));
  }
  return ret;
}

int ObDMLResolver::mark_unpivot_table(TransposeItem &transpose_item, TableItem *table_item)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table_item)
      || OB_UNLIKELY(!table_item->is_generated_table() && !table_item->is_temp_table())
      || OB_ISNULL(table_item->ref_query_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table_item or unexpected", KPC(table_item), K(ret));
  } else if (transpose_item.is_unpivot() && transpose_item.need_use_unpivot_op()) {
    ObSelectStmt &select_stmt = *table_item->ref_query_;
    for (int64_t i = select_stmt.get_unpivot_info().get_output_column_count();
         i < select_stmt.get_select_item_size();
         i++) {
      SelectItem &item = select_stmt.get_select_item(i);
      item.is_unpivot_mocked_column_ = true;
    }
  }
  return ret;
}

int get_column_item_idx_by_name(ObIArray<ColumnItem> &array, const ObString &var, int64_t &idx)
{
  int ret = OB_SUCCESS;
  idx = OB_INVALID_INDEX;
  const int64_t num = array.count();
  for (int64_t i = 0; i < num; i++) {
    if (var == array.at(i).column_name_) {
      idx = i;
      break;
    }
  }
  return ret;
}

int ObDMLResolver::get_transpose_target_sql(const ObIArray<ObString> &columns_in_aggrs,
    TableItem &table_item, TransposeItem &transpose_item, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  sql.reuse();
  //1.get columns
  ObSEArray<ColumnItem, 16> column_items;
  if (table_item.is_basic_table() || table_item.is_fake_cte_table()) {
    if (OB_FAIL(resolve_all_basic_table_columns(table_item, false, &column_items))) {
      LOG_WARN("resolve all basic table columns failed", K(ret));
    }
  } else if (table_item.is_generated_table() || table_item.is_temp_table()) {
    if (OB_FAIL(resolve_all_generated_table_columns(table_item, column_items))) {
      LOG_WARN("resolve all generated table columns failed", K(ret));
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("not support this table", K(table_item), K(ret));
  }

  int64_t idx = OB_INVALID_INDEX;
  if (transpose_item.is_pivot()) {
    //2.check and get groupby column
    for (int64_t i = 0; OB_SUCC(ret) && i < columns_in_aggrs.count(); ++i) {
      const ObString &column_name = columns_in_aggrs.at(i);
      if (OB_FAIL(get_column_item_idx_by_name(column_items, column_name, idx))) {
        LOG_WARN("fail to get_column_item_idx_by_name", K(column_name), K(ret));
      } else if (idx >= 0) {
        if (OB_FAIL(column_items.remove(idx))) {
          LOG_WARN("fail to remove column_item", K(idx), K(ret));
        }
      }
    }

    for (int64_t i = 0; OB_SUCC(ret) && i < transpose_item.for_columns_.count(); ++i) {
      ObString &for_column = transpose_item.for_columns_.at(i);
      if (OB_FAIL(get_column_item_idx_by_name(column_items, for_column, idx))) {
        LOG_WARN("fail to get_column_item_idx_by_name", K(for_column), K(ret));
      } else if (idx >= 0) {
        if (OB_FAIL(column_items.remove(idx))) {
          LOG_WARN("fail to remove column_item", K(idx), K(ret));
        }
      }
    }

    if (OB_SUCC(ret)) {
      transpose_item.old_column_count_ = column_items.count();
      if (OB_FAIL(get_target_sql_for_pivot(column_items, table_item, transpose_item, sql))) {
        LOG_WARN("fail to get_target_sql_for_pivot", K(ret));
      }
    }
    LOG_DEBUG("finish get_transpose_target_sql", K(ret), K(sql), K(transpose_item));
    transpose_item.reset();//no need aggr/for/in expr, reset here
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < transpose_item.in_pairs_.count(); ++i) {
      const common::ObIArray<ObString> &in_columns = transpose_item.in_pairs_.at(i).column_names_;
      for (int64_t j = 0; OB_SUCC(ret) && j < in_columns.count(); ++j) {
        const ObString &column_name = in_columns.at(j);
        if (OB_FAIL(get_column_item_idx_by_name(column_items, column_name, idx))) {
          LOG_WARN("fail to get_column_item_idx_by_name", K(column_name), K(ret));
        } else if (idx >= 0) {
          if (OB_FAIL(column_items.remove(idx))) {
            LOG_WARN("fail to remove column_item", K(idx), K(ret));
          }
        }
      }
    }

    if (OB_SUCC(ret)) {
      transpose_item.old_column_count_ = column_items.count();
      if (OB_FAIL(get_target_sql_for_unpivot(column_items, table_item, transpose_item, sql))) {
        LOG_WARN("fail to get_target_sql_for_unpivot", K(ret));
      }
    }
    LOG_DEBUG("finish get_transpose_target_sql", K(ret), K(sql), K(transpose_item));
  }
  return ret;
}

//for example
//
//t1(c1, c2, c3, c4)
//
//from_list(basic_table):
//t1
//pivot (
//  sum(c1) as sum,
//  max(c1)
//  for (c2, c3)
//  in ((1, 1) as "11",
//      (2, 2)
//      )
//)
//t11
//
//from_list(generated_table):
//(select * from t1)
//pivot (
//  sum(c1) as sum,
//  max(c1)
//  for (c2, c3)
//  in ((1, 1) as "11",
//      (2, 2)
//      )
//) t11
//
//from_list(target_table):
//(select
//  c4,
//  sum(case when (c2, c3) in ((1, 1)) then c1 end) as "11_sum",
//  max(case when (c2, c3) in ((1, 1)) then c1 end) as "11",
//  sum(case when (c2, c3) in ((2, 2)) then c1 end) as "2_2_sum",
//  max(case when (c2, c3) in ((2, 2)) then c1 end) as "2_2"
//from t1
//group by c4
//) t11
int ObDMLResolver::get_target_sql_for_pivot(const ObIArray<ColumnItem> &column_items,
    TableItem &table_item, TransposeItem &transpose_item, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  sql.reuse();
  if (transpose_item.is_pivot()) {
    if (!transpose_item.alias_name_.empty()) {
      if (OB_FAIL(sql.append("SELECT * FROM ( "))) {
        LOG_WARN("fail to append_fmt",K(ret));
      }
    }

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(sql.append("SELECT"))) {
      LOG_WARN("fail to append_fmt",K(ret));
    } else if ((table_item.is_generated_table() || table_item.is_temp_table()) && table_item.ref_query_ != NULL) {
      /* Now, do not print use_hash_aggregation hint for pivot. */
    }

    if (!column_items.empty()) {
      for (int64_t i = 0; OB_SUCC(ret) && i < column_items.count(); ++i) {
        const ObString &column_name = column_items.at(i).column_name_;
        if (OB_FAIL(sql.append_fmt(" \"%.*s\",", column_name.length(), column_name.ptr()))) {
          LOG_WARN("fail to append_fmt", K(column_name), K(ret));
        }
      }
    }
    const int64_t DISTINCT_LENGTH = strlen("DISTINCT");
    if (OB_SUCC(ret)) {
      SMART_VAR(char[OB_MAX_DEFAULT_VALUE_LENGTH], expr_str_buf) {
        MEMSET(expr_str_buf, 0 , sizeof(expr_str_buf));
        for (int64_t i = 0; OB_SUCC(ret) && i < transpose_item.in_pairs_.count(); ++i) {
          const TransposeItem::InPair &in_pair = transpose_item.in_pairs_.at(i);

          for (int64_t j = 0; OB_SUCC(ret) && j < transpose_item.aggr_pairs_.count(); ++j) {
            const TransposeItem::AggrPair &aggr_pair = transpose_item.aggr_pairs_.at(j);
            const char *format_str =
                ((static_cast<const ObAggFunRawExpr *>(aggr_pair.expr_))->is_param_distinct()
                    ? " %s(DISTINCT CASE WHEN ("
                    : " %s(CASE WHEN (");
            if (OB_FAIL(sql.append_fmt(format_str,
                                       ob_aggr_func_str(aggr_pair.expr_->get_expr_type())))) {
              LOG_WARN("fail to append_fmt", K(aggr_pair.expr_->get_expr_type()), K(ret));
            }
            for (int64_t k = 0; OB_SUCC(ret) && k < transpose_item.for_columns_.count(); ++k) {
              const ObString &column_name = transpose_item.for_columns_.at(k);
              if (OB_FAIL(sql.append_fmt("\"%.*s\",", column_name.length(), column_name.ptr()))) {
                LOG_WARN("fail to append_fmt", K(column_name), K(ret));
              }
            }

            if (OB_FAIL(ret)){
            } else if (OB_FAIL(sql.set_length(sql.length() - 1))) {
              LOG_WARN("fail to set_length", K(sql.length()), K(ret));
            } else if (OB_FAIL(sql.append(") in (("))) {
              LOG_WARN("fail to append", K(ret));
            }

            for (int64_t k = 0; OB_SUCC(ret) && k < in_pair.exprs_.count(); ++k) {
              ObRawExpr *expr = in_pair.exprs_.at(k);
              int64_t pos = 0;
              ObRawExprPrinter expr_printer(expr_str_buf, OB_MAX_DEFAULT_VALUE_LENGTH, &pos,
                                            TZ_INFO(params_.session_info_));
              if (OB_FAIL(expr_printer.do_print(expr, T_NONE_SCOPE, true))) {
                LOG_WARN("print expr definition failed", KPC(expr), K(ret));
              } else if (OB_FAIL(sql.append_fmt("%.*s,", static_cast<int32_t>(pos), expr_str_buf))) {
                LOG_WARN("fail to append_fmt", K(expr_str_buf), K(ret));
              }
            }

            if (OB_FAIL(ret)){
            } else if (OB_FAIL(sql.set_length(sql.length() - 1))) {
              LOG_WARN("fail to set_length", K(sql.length()), K(ret));
            } else if (OB_FAIL(sql.append("))"))) {
              LOG_WARN("fail to append", K(ret));
            }

            int64_t pos = 0;
            int32_t expr_name_length = strlen(ob_aggr_func_str(aggr_pair.expr_->get_expr_type())) + 1;
            if ((static_cast<const ObAggFunRawExpr *>(aggr_pair.expr_))->is_param_distinct()) {
              expr_name_length += DISTINCT_LENGTH;
            }
            ObRawExprPrinter expr_printer(expr_str_buf, OB_MAX_DEFAULT_VALUE_LENGTH, &pos,
                                          TZ_INFO(params_.session_info_));
            if (OB_FAIL(expr_printer.do_print(aggr_pair.expr_, T_NONE_SCOPE, true))) {
              LOG_WARN("print expr definition failed", KPC(aggr_pair.expr_), K(ret));
            } else if (OB_FAIL(sql.append_fmt(" THEN %.*s END) AS \"",
                                              static_cast<int32_t>(pos - expr_name_length - 1),
                                              expr_str_buf + expr_name_length))) {
              LOG_WARN("fail to append_fmt", K(aggr_pair.alias_name_), K(ret));
            } else {
              ObString tmp(pos, expr_str_buf);
              int64_t sql_length = sql.length();
              if (in_pair.pivot_expr_alias_.empty()) {
                for (int64_t k = 0; OB_SUCC(ret) && k < in_pair.exprs_.count(); ++k) {
                  ObRawExpr *expr = in_pair.exprs_.at(k);
                  int64_t pos = 0;
                  ObRawExprPrinter expr_printer(expr_str_buf, OB_MAX_DEFAULT_VALUE_LENGTH, &pos,
                                                TZ_INFO(params_.session_info_));
                  if (OB_FAIL(expr_printer.do_print(expr, T_NONE_SCOPE, true))) {
                    LOG_WARN("print expr definition failed", KPC(expr), K(ret));
                  } else if (OB_FAIL(sql.append_fmt("%.*s_", static_cast<int32_t>(pos),
                                                    expr_str_buf))) {
                    LOG_WARN("fail to append_fmt", K(expr_str_buf), K(ret));
                  }
                }
                if (OB_FAIL(ret)){
                } else if (OB_FAIL(sql.set_length(sql.length() - 1))) {
                  LOG_WARN("fail to set_length", K(sql.length()), K(ret));
                }
              } else {
                if (OB_FAIL(sql.append_fmt("%.*s", in_pair.pivot_expr_alias_.length(),
                                                    in_pair.pivot_expr_alias_.ptr()))) {
                  LOG_WARN("fail to append_fmt", K(in_pair.pivot_expr_alias_), K(ret));
                }
              }
              if (OB_FAIL(ret)){
              } else if (! aggr_pair.alias_name_.empty()) {
                if (OB_FAIL(sql.append_fmt("_%.*s", aggr_pair.alias_name_.length(),
                                                      aggr_pair.alias_name_.ptr()))) {
                  LOG_WARN("fail to append_fmt", K(aggr_pair.alias_name_), K(ret));
                }
              }
              if (OB_SUCC(ret)) {
                sql.set_length(MIN(sql.length(), sql_length + OB_MAX_COLUMN_NAME_LENGTH));
                if (OB_FAIL(sql.append("\","))) {
                  LOG_WARN("fail to append", K(ret));
                }
              }
            }
          }//end of aggr
        }//end of in
        if (OB_FAIL(ret)){
        } else if (OB_FAIL(sql.set_length(sql.length() - 1))) {
          LOG_WARN("fail to set_length", K(sql.length()), K(ret));
        } else if (OB_FAIL(format_from_subquery(transpose_item.alias_name_,
            table_item, expr_str_buf, sql))) {
          LOG_WARN("fail to format_from_subquery", K(table_item), K(ret));
        } else if (OB_FAIL(get_partition_for_transpose(table_item, sql))) {
          LOG_WARN("fail to get_partition_for_transpose", K(table_item), K(ret));
        }

        if (OB_FAIL(ret)){
        } else if (!column_items.empty()) {
          if (OB_FAIL(sql.append(" GROUP BY"))) {
            LOG_WARN("fail to append", K(ret));
          }
          for (int64_t i = 0; OB_SUCC(ret) && i < column_items.count(); ++i) {
            const ObString &column_name = column_items.at(i).column_name_;
            if (OB_FAIL(sql.append_fmt(" \"%.*s\",", column_name.length(), column_name.ptr()))) {
              LOG_WARN("fail to append_fmt", K(column_name), K(ret));
            }
          }
          if (OB_FAIL(ret)){
          } else if (OB_FAIL(sql.set_length(sql.length() - 1))) {
            LOG_WARN("fail to set_length", K(sql.length()), K(ret));
          }
        }

        if (OB_SUCC(ret)){
          if (!transpose_item.alias_name_.empty()) {
            if (OB_FAIL(sql.append_fmt(" ) %.*s", transpose_item.alias_name_.length(),
                                       transpose_item.alias_name_.ptr()))) {
              LOG_WARN("fail to append", K(ret));
            }
          }
        }
      }//end SMART_VAR
    }
  }
  return ret;
}

int ObDMLResolver::format_from_subquery(const ObString &unpivot_alias_name,
    TableItem &table_item, char *expr_str_buf, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(params_.query_ctx_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null pointer", K(ret));
  } else if (table_item.is_basic_table()) {
    if (OB_FAIL(sql.append_fmt(" FROM %.*s", table_item.table_name_.length(),
                                             table_item.table_name_.ptr()))) {
      LOG_WARN("fail to append_fmt", K(table_item.table_name_), K(ret));
    } else if (!table_item.alias_name_.empty()
               && table_item.alias_name_ != table_item.table_name_) {
      if (OB_FAIL(sql.append_fmt(" %.*s", table_item.alias_name_.length(),
                                          table_item.alias_name_.ptr()))) {
        LOG_WARN("fail to append_fmt", K(table_item.alias_name_), K(ret));
      }
    } else if (!unpivot_alias_name.empty()) {
      if (OB_FAIL(sql.append_fmt(" %.*s", unpivot_alias_name.length(),
                                          unpivot_alias_name.ptr()))) {
        LOG_WARN("fail to append_fmt", K(unpivot_alias_name), K(ret));
      }
    }
  } else {
    ObIArray<ObString> *column_list = NULL;
    bool is_set_subquery = false;
    int64_t pos = 0;
    ObSelectStmtPrinter stmt_printer(expr_str_buf, OB_MAX_DEFAULT_VALUE_LENGTH, &pos,
                                     static_cast<ObSelectStmt*>(table_item.ref_query_),
                                     params_.query_ctx_->get_timezone_info(), column_list, is_set_subquery);
    if (OB_FAIL(stmt_printer.do_print())) {
      LOG_WARN("fail to print generated table", K(ret));
    } else if (OB_FAIL(sql.append_fmt(" FROM (%.*s)", static_cast<int32_t>(pos), expr_str_buf))) {
      LOG_WARN("fail to append_fmt", K(ret));
    } else if (table_item.cte_type_ == TableItem::NOT_CTE
               && !table_item.table_name_.empty()) {
      if (OB_FAIL(sql.append_fmt(" %.*s", table_item.table_name_.length(),
                                          table_item.table_name_.ptr()))) {
        LOG_WARN("fail to append_fmt", K(ret));
      }
    } else if (!unpivot_alias_name.empty()) {
      if (OB_FAIL(sql.append_fmt(" %.*s", unpivot_alias_name.length(),
                                          unpivot_alias_name.ptr()))) {
        LOG_WARN("fail to append_fmt", K(unpivot_alias_name), K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::get_partition_for_transpose(TableItem &table_item, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (table_item.access_all_part()) {
    /* do nothing */
  } else if (OB_FAIL(sql.append(" PARTITION ("))) {
    LOG_WARN("fail to append", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < table_item.part_names_.count(); ++i) {
      ObString part_names_tmp(table_item.part_names_.at(i));
      if (OB_FAIL(sql.append_fmt("%.*s, ", part_names_tmp.length(), part_names_tmp.ptr()))) {
        LOG_WARN("fail to append", K(ret));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(sql.set_length(sql.length() - 2))) {
      LOG_WARN("fail to set_length", K(sql.length()), K(ret));
    } else if (OB_FAIL(sql.append(")"))) {
      LOG_WARN("fail to append", K(ret));
    }
  }
  return ret;
}

//for example
//
//t1(c1, c2, c3, c4)
//
//from_list(basic_table):
//t1
//unpivot exclude nulls (
//(sal1, sal2)
//for (deptno1, deptno2)
//in ((c2, c3),
//    (c3, c4) as ('c33', 'c44')
//)
//t11
//
//from_list(generated_table):
//(select * from t1)
//unpivot exclude nulls (
//(sal1, sal2)
//for (deptno1, deptno2)
//in ((c2, c3),
//    (c3, c4) as ('c33', 'c44')
//)
//) t11
//
//from_list(target_table):
// select * from
// (select c1, 'c2_c3' as deptno1, 'c2_c3' as deptno2, c2 as sal1, c3 as sal2 ,
//             'c33' as deptno1, 'c44' as deptno2, c3 as sal1, c4 as sal2
//  from pivoted_emp2
//  where (c2 is not null or c3 is not null)
//         and (c3 is not null or c4 is not null)
// )
int ObDMLResolver::get_target_sql_for_unpivot(const ObIArray<ColumnItem> &column_items,
    TableItem &table_item, TransposeItem &transpose_item, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  sql.reuse();
  if (transpose_item.is_unpivot()) {
    if (OB_FAIL(sql.append("SELECT /*+NO_REWRITE UNPIVOT*/* FROM (SELECT /*+NO_REWRITE*/"))) {
      LOG_WARN("fail to append",K(ret));
    } else if (!column_items.empty()) {
      for (int64_t i = 0; OB_SUCC(ret) && i < column_items.count(); ++i) {
        const ObString &column_name = column_items.at(i).column_name_;
        if (OB_FAIL(sql.append_fmt(" \"%.*s\",", column_name.length(), column_name.ptr()))) {
          LOG_WARN("fail to append_fmt", K(column_name), K(ret));
        }
      }
    }

    if (OB_SUCC(ret)) {
      SMART_VAR(char[OB_MAX_DEFAULT_VALUE_LENGTH], expr_str_buf) {
        MEMSET(expr_str_buf, 0, sizeof(expr_str_buf));
        for (int64_t i = 0; OB_SUCC(ret) && i < transpose_item.in_pairs_.count(); ++i) {
          const TransposeItem::InPair &in_pair = transpose_item.in_pairs_.at(i);
          for (int64_t j = 0; OB_SUCC(ret) && j < transpose_item.for_columns_.count(); ++j) {
            const ObString &for_column_name = transpose_item.for_columns_.at(j);

            if (in_pair.exprs_.empty()) {
              if (OB_FAIL(sql.append(" '"))) {
                LOG_WARN("fail to append_fmt",K(ret));
              }
              //TODO::use upper
              for (int64_t k = 0; OB_SUCC(ret) && k < in_pair.column_names_.count(); ++k) {
                const ObString &column_name = in_pair.column_names_.at(k);
                if (OB_FAIL(sql.append_fmt("%.*s_", column_name.length(), column_name.ptr()))) {
                  LOG_WARN("fail to append_fmt", K(column_name), K(ret));
                }
              }

              if (OB_FAIL(ret)){
              } else if (OB_FAIL(sql.set_length(sql.length() - 1))) {
                LOG_WARN("fail to set_length", K(sql.length()), K(ret));
              } else if (0 == i) {
                if (OB_FAIL(sql.append_fmt("' AS \"%.*s\",",
                                           for_column_name.length(), for_column_name.ptr()))) {
                  LOG_WARN("fail to append", K(for_column_name), K(ret));
                }
              } else if (OB_FAIL(sql.append_fmt("' AS \"%ld_%.*s\",",
                                                i, for_column_name.length(), for_column_name.ptr()))) {
                LOG_WARN("fail to append", K(for_column_name), K(ret));
              }
            } else {
              ObRawExpr *expr = in_pair.exprs_.at(j);
              int64_t pos = 0;
              ObRawExprPrinter expr_printer(expr_str_buf, OB_MAX_DEFAULT_VALUE_LENGTH, &pos, TZ_INFO(params_.session_info_));
              if (OB_FAIL(expr_printer.do_print(expr, T_NONE_SCOPE, true))) {
                LOG_WARN("print expr definition failed", KPC(expr), K(ret));
              } else if (0 == i) {
                if (OB_FAIL(sql.append_fmt(" %.*s AS \"%.*s\",",
                                            static_cast<int32_t>(pos), expr_str_buf,
                                            for_column_name.length(), for_column_name.ptr()))) {
                  LOG_WARN("fail to append", K(for_column_name), K(ret));
                }
              } else if (OB_FAIL(sql.append_fmt(" %.*s AS \"%ld_%.*s\",",
                                                static_cast<int32_t>(pos), expr_str_buf,
                                                i, for_column_name.length(), for_column_name.ptr()))) {
                LOG_WARN("fail to append", K(for_column_name), K(ret));
              }
            }
          }

          for (int64_t j = 0; OB_SUCC(ret) && j < in_pair.column_names_.count(); ++j) {
            const ObString &in_column_name = in_pair.column_names_.at(j);
            const ObString &unpivot_column_name = transpose_item.unpivot_columns_.at(j);
            if (0 == i) {
              if (OB_FAIL(sql.append_fmt(" \"%.*s\" AS \"%.*s\",",
                                         in_column_name.length(), in_column_name.ptr(),
                                         unpivot_column_name.length(), unpivot_column_name.ptr()))) {
                LOG_WARN("fail to append_fmt", K(in_column_name), K(unpivot_column_name), K(ret));
              }
            } else if (OB_FAIL(sql.append_fmt(" \"%.*s\" AS \"%ld_%.*s\",",
                                       in_column_name.length(), in_column_name.ptr(),
                                       i, unpivot_column_name.length(), unpivot_column_name.ptr()))) {
              LOG_WARN("fail to append_fmt", K(in_column_name), K(unpivot_column_name), K(ret));
            }
          }
        }

        if (OB_FAIL(ret)){
        } else if (OB_FAIL(sql.set_length(sql.length() - 1))) {
          LOG_WARN("fail to set_length", K(sql.length()), K(ret));
        } else if (OB_FAIL(format_from_subquery(transpose_item.alias_name_,
            table_item, expr_str_buf, sql))) {
          LOG_WARN("fail to format_from_subquery", K(table_item), K(ret));
        } else if (OB_FAIL(get_partition_for_transpose(table_item, sql))) {
          LOG_WARN("fail to get_partition_for_transpose", K(table_item), K(ret));
        } else if (transpose_item.is_exclude_null()) {
          if (OB_FAIL(sql.append(" WHERE"))) {
            LOG_WARN("fail to append", K(ret));
          }
          for (int64_t i = 0; OB_SUCC(ret) && i < transpose_item.in_pairs_.count(); ++i) {
            const TransposeItem::InPair &in_pair = transpose_item.in_pairs_.at(i);
            const char *format_str = (i != 0 ? " OR (" : " (");
            if (OB_FAIL(sql.append(format_str))) {
              LOG_WARN("fail to append", K(ret));
            }
            for (int64_t j = 0; OB_SUCC(ret) && j < in_pair.column_names_.count(); ++j) {
              const ObString &column_name = in_pair.column_names_.at(j);
              const char *format_str = (j != 0 ? " OR \"%.*s\" IS NOT NULL" : " \"%.*s\" IS NOT NULL");
              if (OB_FAIL(sql.append_fmt(format_str, column_name.length(), column_name.ptr()))) {
                LOG_WARN("fail to append_fmt", K(column_name), K(ret));
              }
            }
            if (OB_FAIL(sql.append(")"))) {
              LOG_WARN("fail to append", K(ret));
            }
          }
        }

        if (OB_SUCC(ret)) {
          if (OB_FAIL(sql.append(")"))) {
            LOG_WARN("fail to append", K(ret));
          } else if (!transpose_item.alias_name_.empty()) {
            if (OB_FAIL(sql.append_fmt(" %.*s", transpose_item.alias_name_.length(),
                                                transpose_item.alias_name_.ptr()))) {
              LOG_WARN("fail to append", K(ret));
            }
          }
        }
      }
    }
  }
  return ret;
}


int ObDMLResolver::remove_orig_table_item(TableItem &table_item)
{
  int ret = OB_SUCCESS;
  //need remove last saved table item
  ObDMLStmt *stmt = get_stmt();
  if (OB_ISNULL(stmt)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("stmt is unexpected", KP(stmt), K(ret));
  } else if (OB_FAIL(column_namespace_checker_.remove_reference_table(table_item.table_id_))) {
    LOG_WARN("failed to remove_reference_table", K(ret));
  } else if (OB_FAIL(stmt->remove_table_item(&table_item))) {
    LOG_WARN("failed to remove target table items", K(ret));
  } else if (OB_FAIL(stmt->remove_column_item(table_item.table_id_))) {
    LOG_WARN("failed to remove column items.", K(ret));
  } else if (OB_FAIL(stmt->remove_part_expr_items(table_item.table_id_))) {
    LOG_WARN("failed to remove part expr item", K(ret));
  } else if (OB_FAIL(stmt->rebuild_tables_hash())) {
    LOG_WARN("rebuild table hash failed", K(ret));
  }
  return ret;
}

int ObDMLResolver::update_errno_if_sequence_object(
    const ObQualifiedName &q_name,
    int old_ret)
{
  int ret = old_ret;
  if (!q_name.tbl_name_.empty() &&
      ObSequenceNamespaceChecker::is_curr_or_next_val(q_name.col_name_)) {
    ret = OB_OBJECT_NAME_NOT_EXIST;
    LOG_WARN("sequence not exists", K(q_name), K(old_ret), K(ret));
    LOG_USER_ERROR(OB_OBJECT_NAME_NOT_EXIST, "sequence");
  }
  return ret;
}

int ObDMLResolver::add_sequence_id_to_stmt(uint64_t sequence_id, bool is_currval)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = NULL;
  if (OB_ISNULL(stmt = get_stmt())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(stmt), K(ret));
  } else {
    bool exist = false;
    // 一般来说，同一个语句中 nextval 会比较少，因此用 for 搜索效率也不是问题
    const ObIArray<uint64_t> &ids = is_currval ? stmt->get_currval_sequence_ids() :
                                                 stmt->get_nextval_sequence_ids();

    FOREACH_CNT_X(id, ids, !exist) {
      if (*id == sequence_id) {
        exist = true;
      }
    }
    if (!exist) {
      // 如果是 CURRVAL 表达式，则指示 stmt 生成 SEQUENCE 算子，但不做具体事情
      //
      // 如果是 NEXTVAL 表达式，则添加到 STMT 中，提示 SEQUENCE 算子为它计算 NEXTVALUE
      //  note: 按照 Oracle 语义，一个语句中即使出现多次相同对象的 nextval
      //        也只计算一次。所以这里只需要保存唯一的 sequence_id 即可
      if (is_currval) {
        if (OB_FAIL(stmt->add_currval_sequence_id(sequence_id))) {
          LOG_WARN("failed to push back sequence id",K(ret));
        } else {
          // do nothing
        }
      } else if (OB_FAIL(stmt->add_nextval_sequence_id(sequence_id))) {
        LOG_WARN("fail push back sequence id",
                 K(sequence_id), K(ids), K(ret));
      } else {
        // do nothing
      }
      if (OB_SUCC(ret) && OB_FAIL(add_object_version_to_dependency(DEPENDENCY_SEQUENCE,
                                                                    SEQUENCE_SCHEMA,
                                                                    sequence_id))) {
        LOG_WARN("add object version to dependency failed", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::generate_check_constraint_exprs(const TableItem *table_item,
                                                   const share::schema::ObTableSchema *table_schema,
                                                   ObIArray<ObRawExpr*> &check_exprs,
                                                   ObIArray<int64_t> *check_flags/*default null*/)
{
  int ret = OB_SUCCESS;
  const ParseNode *node = NULL;
  ObDMLStmt *dml_stmt = static_cast<ObDMLStmt*>(stmt_);
  bool resolve_check_for_optimizer = (check_flags != NULL);
  ObSEArray<ObRawExpr *,4> tmp_check_constraint_exprs;

  if (OB_ISNULL(params_.session_info_) || OB_ISNULL(params_.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(params_.session_info_), K(params_.allocator_));
  } else {
    for (ObTableSchema::const_constraint_iterator iter = table_schema->constraint_begin(); OB_SUCC(ret) &&
         iter != table_schema->constraint_end(); iter ++) {
      ObRawExpr *check_constraint_expr = NULL;
      ObConstraint tmp_constraint;
      ObSEArray<ObQualifiedName, 1> columns;
      if ((*iter)->get_constraint_type() != CONSTRAINT_TYPE_CHECK
          && (*iter)->get_constraint_type() != CONSTRAINT_TYPE_NOT_NULL) {
        continue;
      } else if (!(*iter)->get_enable_flag() &&
                 (*iter)->is_validated() &&
                 !resolve_check_for_optimizer) {
        const ObSimpleDatabaseSchema *database_schema = NULL;
        if (OB_ISNULL(params_.schema_checker_->get_schema_guard())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("schema guard ptr is null ptr", K(ret), KP(params_.schema_checker_));
        } else if (OB_FAIL(params_.schema_checker_->get_schema_guard()->get_database_schema(
                   table_schema->get_tenant_id(), table_schema->get_database_id(), database_schema))) {
          LOG_WARN("get_database_schema failed", K(ret), K(table_schema->get_database_id()));
        } else if (OB_ISNULL(database_schema)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("database_schema is null", K(ret));
        } else {
          const ObString &origin_database_name = database_schema->get_database_name_str();
          if (origin_database_name.empty() || (*iter)->get_constraint_name_str().empty()) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("database name or cst name is null", K(ret), K(origin_database_name), K((*iter)->get_check_expr_str().empty()));
          } else {
            ret = OB_ERR_CONSTRAINT_CONSTRAINT_DISABLE_VALIDATE;
            LOG_USER_ERROR(OB_ERR_CONSTRAINT_CONSTRAINT_DISABLE_VALIDATE,
                           origin_database_name.length(), origin_database_name.ptr(),
                           (*iter)->get_constraint_name_str().length(), (*iter)->get_constraint_name_str().ptr());
            LOG_WARN("no insert on table with constraint disabled and validated", K(ret));
          }
        }
      } else if ((*iter)->get_constraint_type() == CONSTRAINT_TYPE_NOT_NULL) {
        continue;
      } else if (!(*iter)->get_enable_flag() &&
                 (*iter)->is_no_validate() &&
                 (!resolve_check_for_optimizer || !(*iter)->get_rely_flag())) {
        continue;
      } else if (OB_FAIL(ObRawExprUtils::parse_bool_expr_node_from_str(
                 (*iter)->get_check_expr_str(), *(params_.allocator_), node))) {
        LOG_WARN("parse expr node from string failed", K(ret));
      } else if (OB_FAIL(ObResolverUtils::resolve_check_constraint_expr(
                 params_, node, *table_schema, tmp_constraint, check_constraint_expr, NULL, &columns))) {
        LOG_WARN("resolve check constraint expr failed", K(ret));
      } else if (table_item->is_basic_table() &&
                 OB_FAIL(resolve_columns_for_partition_expr(check_constraint_expr,
                                                            columns,
                                                            *table_item,
                                                            table_schema->is_oracle_tmp_table()))) {
        LOG_WARN("resolve columns for partition expr failed", K(ret));
      } else if (OB_FAIL(tmp_check_constraint_exprs.push_back(check_constraint_expr))) {
        LOG_WARN("array push back failed", K(ret));
      } else if (resolve_check_for_optimizer) {
        int64_t check_flag = 0;
        if ((*iter)->get_enable_flag()) {
          check_flag |= ObDMLStmt::CheckConstraintFlag::IS_ENABLE_CHECK;
        }
        if ((*iter)->is_validated()) {
          check_flag |= ObDMLStmt::CheckConstraintFlag::IS_VALIDATE_CHECK;
        }
        if ((*iter)->get_rely_flag()) {
          check_flag |= ObDMLStmt::CheckConstraintFlag::IS_RELY_CHECK;
        }
        if (OB_FAIL(check_flags->push_back(check_flag))) {
          LOG_WARN("failed to push back", K(ret));
        }
      }
    }
  }
  if (OB_SUCC(ret) && !tmp_check_constraint_exprs.empty()) {
    if (OB_FAIL(deduce_generated_exprs(tmp_check_constraint_exprs))) {
      LOG_WARN("deduce generated exprs failed", K(ret));
    } else if (OB_FAIL(append(check_exprs, tmp_check_constraint_exprs))) {
      LOG_WARN("failed to append check constraint exprs", K(ret));
    }
  }
  return ret;
}

int ObDMLResolver::resolve_external_name(ObQualifiedName &q_name,
                                         ObIArray<ObQualifiedName> &columns,
                                         ObIArray<ObRawExpr*> &real_exprs,
                                         ObRawExpr *&expr)
{
  int ret = OB_SUCCESS;
  /*
   * 这里不判断params_.secondary_namespace_是否为空，如果NULL == params_.secondary_namespace_，
   * 说明是从纯SQL语境调用过来的，比如select f(1) from dual;其中f是一个pl的函数，
   * 这种情况我们只需要schema就能够处理
   */
  CK(OB_NOT_NULL(params_.allocator_));
  CK(OB_NOT_NULL(params_.expr_factory_));
  CK(OB_NOT_NULL(params_.session_info_));
  CK(OB_NOT_NULL(params_.schema_checker_));
  CK(OB_NOT_NULL(params_.schema_checker_->get_schema_guard()));
  if (OB_SUCC(ret) && OB_ISNULL(params_.sql_proxy_)) {
    CK (OB_NOT_NULL(params_.sql_proxy_ = GCTX.sql_proxy_));
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(ObResolverUtils::resolve_external_symbol(*params_.allocator_,
                                                         *params_.expr_factory_,
                                                         *params_.session_info_,
                                                         *params_.schema_checker_->get_schema_guard(),
                                                         params_.sql_proxy_,
                                                         &(params_.external_param_info_),
                                                         params_.secondary_namespace_,
                                                         q_name,
                                                         columns,
                                                         real_exprs,
                                                         expr,
                                                         params_.is_prepare_protocol_,
                                                         false, /*is_check_mode*/
                                                         true /*is_sql_scope*/))) {
      LOG_WARN_IGNORE_COL_NOTFOUND(ret, "failed to resolve var", K(q_name), K(ret));
    } else if (OB_ISNULL(expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("Invalid expr", K(expr), K(ret));
    } else if (expr->is_udf_expr()) {
      ObUDFRawExpr *udf_expr = static_cast<ObUDFRawExpr*>(expr);
      ObDMLStmt *stmt = get_stmt();
      ObSchemaObjVersion udf_version;
      CK (OB_NOT_NULL(udf_expr));
      if (OB_SUCC(ret) && udf_expr->need_add_dependency()) {
        OZ (udf_expr->get_schema_object_version(udf_version));
        CK (OB_NOT_NULL(stmt));
        OZ (stmt->add_global_dependency_table(udf_version));
      }
      //for udf without params, we just set called_in_sql = true,
      //if this expr go through pl :: build_raw_expr later,
      //the flag will change to false;
      OX (expr->set_is_called_in_sql(true));
    }
  }
  if (OB_ERR_SP_UNDECLARED_VAR == ret) {
    ret = OB_ERR_BAD_FIELD_ERROR;
  }
  return ret;
}

int ObDMLResolver::add_cte_table_to_children(ObChildStmtResolver& child_resolver) {
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < current_cte_tables_.count(); i++) {
    if (OB_FAIL(child_resolver.add_parent_cte_table_item(current_cte_tables_.at(i)))) {
      LOG_WARN("add cte table to children failed");
    } else {
    }
  }
  return ret;
}

/**
 * @bref 检测 oracle outer join 的 join condition 合法性.
 * 1. 检测 scope
 * 2. 检测谓词约束: in, or 的数量.
 */
int ObDMLResolver::check_oracle_outer_join_condition(const ObRawExpr *expr)
{
  int ret = OB_SUCCESS;;
  CK(OB_NOT_NULL(expr));
  if (OB_SUCC(ret) && (expr->has_flag(CNT_OUTER_JOIN_SYMBOL))) {
    if (OB_UNLIKELY(expr->has_flag(CNT_IN) || expr->has_flag(CNT_OR))) {
      /**
       * ORA-01719: OR 或 IN 操作数中不允许外部联接运算符 (+)
       * 01719. 00000 -  "outer join operator (+) not allowed in operand of OR or IN"
       * *Cause:    An outer join appears in an or clause.
       * *Action:   If A and B are predicates, to get the effect of (A(+) or B),
       *            try (select where (A(+) and not B)) union all (select where (B)).
       * ----
       * error: a(+) = b or c = d
       * OK: a(+) in (1) [IN is T_OP_EQ in here]
       */
      ObArray<uint64_t> right_tables;
      OZ(check_oracle_outer_join_in_or_validity(expr, right_tables));
    } else if (expr->has_flag(CNT_SUB_QUERY)) {
      /**
       * ORA-01799: 列不能外部联接到子查询
       * 01799. 00000 -  "a column may not be outer-joined to a subquery"
       * *Cause:    <expression>(+) <relop> (<subquery>) is not allowed.
       * *Action:   Either remove the (+) or make a view out of the subquery.
       *            In V6 and before, the (+) was just ignored in this case.
       * ----
       * error: a(+) = (select * from t1);
       */
      ret = OB_ERR_OUTER_JOIN_WITH_SUBQUERY;
      LOG_WARN("column may not be outer-joined to a subquery");
    } else if (has_ansi_join()) {
      /**
       * ORA-25156: 旧样式的外部联接 (+) 不能与 ANSI 联接一起使用
       * 25156. 00000 -  "old style outer join (+) cannot be used with ANSI joins"
       * *Cause:    When a query block uses ANSI style joins, the old notation
       *            for specifying outer joins (+) cannot be used.
       * *Action:   Use ANSI style for specifying outer joins also.
       * ----
       * error: select * from t1 left join t2 on t1.c1 = t2.c1 where t1.c1 = t2.c1(+)
       */
      ret = OB_ERR_OUTER_JOIN_WITH_ANSI_JOIN;
      LOG_WARN("old style outer join (+) cannot be used with ANSI joins");
    }
  }
  return ret;
}

// in some cases, oracle_outer_join is allowed in IN/OR
int ObDMLResolver::check_oracle_outer_join_in_or_validity(const ObRawExpr *expr,
                                                          ObIArray<uint64_t> &right_tables)
{
  int ret = OB_SUCCESS;;
  CK(OB_NOT_NULL(expr));
  if(OB_SUCC(ret)){
    switch (expr->get_expr_type()) {
      case T_OP_IN: {
        // ORA-1719  t1.c1(+) in (t2.c1, xxxx);  OB_ERR_OUTER_JOIN_AMBIGUOUS
        // ORA-1468  t1.c1(+) in (t2.c1(+), xxxx);  OB_ERR_MULTI_OUTER_JOIN_TABLE;
        // ORA-1416  t1.c1(+) in (t1.c1, xxxx);  OB_ERR_OUTER_JOIN_NESTED
        //@OK: t1.c1(+) in (1, 1);
        if(expr->get_param_count() == 2 && expr->has_flag(CNT_OUTER_JOIN_SYMBOL)) {
          const ObRawExpr * left_expr = expr->get_param_expr(0);
          const ObRawExpr * right_exprs = expr->get_param_expr(1);
          CK(OB_NOT_NULL(left_expr));
          CK(OB_NOT_NULL(right_exprs));
          if (OB_SUCC(ret)) {
            //ObArray<uint64_t> right_tables; // table with (+)
            ObArray<uint64_t> left_tables;  // table without (+)
            ObArray<uint64_t> le_left_tables;  // left tables of left expr;
            ObArray<uint64_t> le_right_tables; // right tables of left expr;
            OZ(extract_column_with_outer_join_symbol(left_expr, le_left_tables, le_right_tables));
            const int64_t param_cnt = right_exprs->get_param_count();
            /* check each expr separately from right to left.
            * select * from t1,t2 where t1.c1(+) in (t1.c1, t2.c1(+)); will raise ORA-1468
            * select * from t1,t2 where t1.c1(+) in (t2.c1(+), t1.c1); will raise ORA-1416
            */
            for (int64_t i = param_cnt - 1; OB_SUCC(ret) && i >= 0; i--) {
              const ObRawExpr * right_expr = right_exprs->get_param_expr(i);
              CK(OB_NOT_NULL(right_expr));
              if (OB_SUCC(ret)) {
                ret = check_single_oracle_outer_join_expr_validity(right_expr,
                                                                   le_left_tables,
                                                                   le_right_tables,
                                                                   left_tables,
                                                                   right_tables);
              }
            }
            if (OB_SUCC(ret)) {
              // check right_tables and left_tables
              // size of left table should be 0, size of right table should be 1.
              for (int64_t i = 0; i < le_right_tables.count(); i++) {
                OZ((common::add_var_to_array_no_dup)(right_tables, le_right_tables.at(i)));
              }
              // if there is conflict between two exprs in IN, the error is ORA-1719
              if (right_tables.count() != 1 || left_tables.count() != 0) {
                ret = OB_ERR_OUTER_JOIN_AMBIGUOUS;
                LOG_WARN("outer join operator (+) not allowed in operand of OR or IN", K(ret));
              }
            }
          }
        }
        break;
      }
      case T_OP_OR: {
        // oracle_outer_join should appear in both side of OR-expr.
        /* the check order is from left to right.
        * select * from t1,t2 where t1.c1(+) = t1.c1 or t1.c1(+) = t2.c1(+); will raise ORA-1416
        * select * from t1,t2 where t1.c1(+) = t2.c1(+) or t1.c1(+) = t1.c1; will raise ORA-1468
        * @OK: select * from t1, t2 where t1.c1(+) = 1 or t1.c2(+) = 2;
        */
        for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); i++) {
          const ObRawExpr *e = expr->get_param_expr(i);
          CK(OB_NOT_NULL(e));
          if (OB_SUCC(ret)) {
            if (e->has_flag(IS_OR)) {
              ret = SMART_CALL(check_oracle_outer_join_in_or_validity(e, right_tables));
            } else {
              /* the tmp_left_tables and tmp_right_tables contain the table_id of the tables appear
              * in e. Conflict between tmp_left_tables and tmp_right_tables will raise ORA-1416 or ORA-1468.
              * e,g,. t1.c1(+) = t2.c1(+) will raise ORA-1468.
              *       t1.c1(+) = t1.c1 will raise ORA-1416
              *       t1.c1(+) + t2.c1 + t1.c1 = 1 or t1.c1(+) + t1.c1 + t2.c1 will raise ORA-1416
              */
              ObArray<uint64_t> tmp_left_tables;
              ObArray<uint64_t> tmp_right_tables;
              OZ(extract_column_with_outer_join_symbol(e, tmp_left_tables, tmp_right_tables));
              if (OB_SUCC(ret)) {
                if (tmp_left_tables.count() != 0 ||
                    tmp_right_tables.count() != 1 ||
                    (right_tables.count() != 0 &&
                     !has_exist_in_array(right_tables, tmp_right_tables.at(0)))) {
                  // should raise error
                  if (tmp_right_tables.count() > 1) {
                    ret = OB_ERR_MULTI_OUTER_JOIN_TABLE;
                    LOG_WARN("a predicate may reference only one outer-joined table", K(ret));
                  } else if (tmp_left_tables.count() != 0 && tmp_right_tables.count() == 1) {
                    bool exist_flag = false;
                    for (int64_t i = 0; !exist_flag && i < tmp_left_tables.count(); i++) {
                      if (has_exist_in_array(tmp_right_tables, tmp_left_tables.at(i))) {
                        exist_flag = true;
                      }
                    }
                    if (exist_flag) {
                      ret = OB_ERR_OUTER_JOIN_NESTED;
                      LOG_WARN("two tables cannot be outer-joined to each other", K(ret));
                    } else {
                      ret = OB_ERR_OUTER_JOIN_AMBIGUOUS;
                      LOG_WARN("outer join operator (+) not allowed in operand of OR or IN", K(ret));
                    }
                  } else {
                    ret = OB_ERR_OUTER_JOIN_AMBIGUOUS;
                    LOG_WARN("outer join operator (+) not allowed in operand of OR or IN", K(ret));
                  }
                } else if (right_tables.count() == 0 ){
                  OZ((common::add_var_to_array_no_dup)(right_tables, tmp_right_tables.at(0)));
                }
              }
            }
          }
        }
        break;
      }
      default: {
        const int64_t cnt = expr->get_param_count();
        for (int64_t i = 0; OB_SUCC(ret) && i < cnt; i++) {
          const ObRawExpr *e = expr->get_param_expr(i);
          if (NULL == e) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("null param expr returned", K(ret), K(i), K(cnt));
          } else if (e->has_flag(CNT_OR) || e->has_flag(CNT_IN)) {
            OZ(SMART_CALL(check_oracle_outer_join_in_or_validity(e, right_tables)));
          }
        }
      }
    }
  }
  return ret;
}

int ObDMLResolver::check_single_oracle_outer_join_expr_validity(const ObRawExpr *right_expr,
                                                                ObIArray<uint64_t> &le_left_tables, //left tables of the left expr
                                                                ObIArray<uint64_t> &le_right_tables, //right tables of the left expr
                                                                ObIArray<uint64_t> &left_tables,
                                                                ObIArray<uint64_t> &right_tables)
{
  UNUSED(left_tables);
  int ret = OB_SUCCESS;;
  CK(OB_NOT_NULL(right_expr));
  if (OB_SUCC(ret)) {
    ObArray<uint64_t> re_left_tables;  // left tables of right expr;
    ObArray<uint64_t> re_right_tables; // right tables of right expr;
    OZ(extract_column_with_outer_join_symbol(right_expr, re_left_tables, re_right_tables));
    if (OB_SUCC(ret)) {
      if (le_left_tables.count() != 0 || re_left_tables.count() != 0) {
        if (le_right_tables.count() == 0 &&
            re_right_tables.count() == 0 &&
            right_tables.count() == 0) {
        } else {
          bool exist_flag = true;
          for (int64_t i = 0; exist_flag && i < le_left_tables.count(); i++) {
            if (!has_exist_in_array(re_right_tables,le_left_tables.at(i))) {
              exist_flag = false;
            }
          }
          for (int64_t i = 0; exist_flag && i < re_left_tables.count(); i++) {
            if (!has_exist_in_array(le_right_tables,re_left_tables.at(i))) {
              exist_flag = false;
            }
          }
          if (exist_flag) {
            ret = OB_ERR_OUTER_JOIN_NESTED;
            LOG_WARN("two tables cannot be outer-joined to each other", K(ret));
          } else {
            ret = OB_ERR_OUTER_JOIN_AMBIGUOUS;
            LOG_WARN("outer join operator (+) not allowed in operand of OR or IN", K(ret));
          }
        }
      } else if (le_right_tables.count() == 0 && re_right_tables.count() == 0) {
        // both left expr and right expr are exprs without table;
        ret = OB_ERR_OUTER_JOIN_AMBIGUOUS;
        LOG_WARN("outer join operator (+) not allowed in operand of OR or IN", K(ret));
      } else {
        bool exist_flag = true;
        // check le_right_tables.count() to avoid left expr being a const.
        // e,g,. select * from t1, t2 where 1 in (t1.c1(+), t2.c1(+)) should raise ORA-1719
        // instead of ORA-1468
        for (int64_t i = 0; le_right_tables.count() > 0 && i<re_right_tables.count(); i++) {
          if (!has_exist_in_array(le_right_tables, re_right_tables.at(i))) {
            exist_flag = false;
          }
        }
        if (!exist_flag) {
          ret = OB_ERR_MULTI_OUTER_JOIN_TABLE;
          LOG_WARN("a predicate may reference only one outer-joined table", K(ret));
        }
      }
      if (OB_SUCC(ret)) {
        //merge the right expr's right table to right_tables
        for (int64_t i = 0; i < re_right_tables.count(); i++) {
          OZ((common::add_var_to_array_no_dup)(right_tables, re_right_tables.at(i)));
        }
        // no need to merge the left tables, since any left table will raise error.
      }
    }
  }
  return ret;
}

/**
 * @bref 消除 expr 中的 T_OP_ORACLE_OUTER_JOIN_SYMBOL.
 * 消除方式为把有 IS_OUTER_JOIN_SYMBOL 的节点移除.
 * 1. 如果 outer join symbol 出现在我们处理不了的 scope 需要消除.
 * 2. 所有处理完的 expr 需要消除.
 */
int ObDMLResolver::remove_outer_join_symbol(ObRawExpr* &expr)
{
  int ret = OB_SUCCESS;
  CK(OB_NOT_NULL(expr));

  if (OB_FAIL(ret)) {
    // do nothing
  } else if (!expr->has_flag(CNT_OUTER_JOIN_SYMBOL)) {
    // do nothing
  } else if (expr->has_flag(IS_OUTER_JOIN_SYMBOL)) {
    CK(expr->get_param_count() == 1,
       OB_NOT_NULL(expr->get_param_expr(0)));
    if (OB_SUCC(ret)) {
      expr = expr->get_param_expr(0);
      OZ(SMART_CALL(remove_outer_join_symbol(expr)));
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); i++) {
      OZ(SMART_CALL(remove_outer_join_symbol(expr->get_param_expr(i))));
    }
  }
  return ret;
}

/**
 * @bref 检查 scope, 如果存在 T_OUTER_JOIN_SYMBOL 在 WHERE 中,
 * 设置 has_oracle_join 标志; 否则消除或者报错(取决于scope).
 */
int ObDMLResolver::resolve_outer_join_symbol(const ObStmtScope scope,
                                             ObRawExpr* &expr)
{
  int ret = OB_SUCCESS;
  CK(OB_NOT_NULL(expr));
  if (OB_SUCC(ret) && (expr->has_flag(CNT_OUTER_JOIN_SYMBOL))) {
    if (OB_UNLIKELY(T_FIELD_LIST_SCOPE == scope
                    || T_CONNECT_BY_SCOPE == scope
                    || T_START_WITH_SCOPE == scope
                    || T_ORDER_SCOPE == scope)) {
      /*
       * ORA-30563: 此处不允许使用外部联接运算符 (+)
       * 30563. 00000 -  "outer join operator (+) is not allowed here"
       * *Cause:    An attempt was made to reference (+) in either the select-list,
       *            CONNECT BY clause, START WITH clause, or ORDER BY clause.
       * *Action:   Do not use the operator in the select-list, CONNECT
       *            BY clause, START WITH clause, or ORDER BY clause.
       */
      ret = OB_ERR_OUTER_JOIN_NOT_ALLOWED;
      LOG_WARN("outer join operator (+) is not allowed here", K(ret));
    } else if (T_WHERE_SCOPE != current_scope_) {
      OZ(remove_outer_join_symbol(expr));
    } else {
      set_has_oracle_join(true);
    }
  }
  return ret;
}

int ObDMLResolver::generate_outer_join_tables()
{
  int ret = OB_SUCCESS;
  if (has_oracle_join()) {
    ObDMLStmt *stmt = get_stmt();
    CK(OB_NOT_NULL(stmt));

    if (OB_SUCC(ret)) {
      ObArray<ObBitSet<> > table_dependencies;
      OZ(generate_outer_join_dependency(stmt->get_table_items(),
                                          stmt->get_condition_exprs(),
                                          table_dependencies));
      OZ(build_outer_join_table_by_dependency(table_dependencies, *stmt));
      // remove predicate
      ObArray<JoinedTable*> joined_tables;
      for (int64_t i = 0; OB_SUCC(ret) && i < stmt->get_from_item_size(); i++) {
        const FromItem &from_item = stmt->get_from_item(i);
        if (from_item.is_joined_) {
          OZ((joined_tables.push_back)(stmt->get_joined_table(from_item.table_id_)));
        }
      }

      OZ(deliver_outer_join_conditions(stmt->get_condition_exprs(), joined_tables));
    }
  }
  return ret;
}

int ObDMLResolver::generate_outer_join_dependency(
    const ObIArray<TableItem*> &table_items,
    const ObIArray<ObRawExpr*> &exprs,
    ObIArray<ObBitSet<> > &table_dependencies)
{
  int ret = OB_SUCCESS;
  table_dependencies.reset();
  ObArray<uint64_t> all_table_ids;
  // init param
  for (int64_t i = 0; OB_SUCC(ret) && i < table_items.count(); i++) {
    OZ((table_dependencies.push_back)(ObBitSet<>()));
    CK(OB_NOT_NULL(table_items.at(i)));
    OZ((all_table_ids.push_back)(table_items.at(i)->table_id_));
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < exprs.count(); i++) {
    const ObRawExpr* expr = exprs.at(i);
    ObArray<uint64_t> left_tables;
    ObArray<uint64_t> right_tables;
    CK(OB_NOT_NULL(expr));
    OZ(check_oracle_outer_join_condition(expr));
    OZ(extract_column_with_outer_join_symbol(expr, left_tables, right_tables));

    if (OB_FAIL(ret)) {
      // do nothing
    } else if (OB_UNLIKELY(right_tables.count() > 1)) {
      /**
       * ORA-01468: 一个谓词只能引用一个外部联接的表
       * 01468. 00000 -  "a predicate may reference only one outer-joined table"
       * *Cause:
       * *Action:
       */
      ret = OB_ERR_MULTI_OUTER_JOIN_TABLE;
      LOG_WARN("a predicate may reference only one outer-joined table", K(ret));
    } else if (1 == right_tables.count() && 0 != left_tables.count()) {
      OZ(add_oracle_outer_join_dependency(all_table_ids,
                                          left_tables,
                                          right_tables.at(0),
                                          table_dependencies));
    }
  }
  return ret;
}

int ObDMLResolver::extract_column_with_outer_join_symbol(
    const ObRawExpr *expr, ObIArray<uint64_t> &left_tables, ObIArray<uint64_t> &right_tables)
{
  int ret = OB_SUCCESS;
  bool is_right = false;
  CK(OB_NOT_NULL(expr));
  if (OB_SUCC(ret)) {
    if (expr->has_flag(IS_OUTER_JOIN_SYMBOL)) {
      CK(1 == expr->get_param_count(), OB_NOT_NULL(expr->get_param_expr(0)));
      if (OB_SUCC(ret)) {
        expr = expr->get_param_expr(0);
        is_right = true;
      }
    }

    if (expr->has_flag(IS_COLUMN)) {
      const ObColumnRefRawExpr *col_expr = static_cast<const ObColumnRefRawExpr*>(expr);
      if (!is_right) {
        OZ((common::add_var_to_array_no_dup)(left_tables, col_expr->get_table_id()));
      } else {
        OZ((common::add_var_to_array_no_dup)(right_tables, col_expr->get_table_id()));
      }
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); i++) {
        CK(OB_NOT_NULL(expr->get_param_expr(i)));
        OZ(SMART_CALL(extract_column_with_outer_join_symbol(expr->get_param_expr(i),
                                                            left_tables,
                                                            right_tables)));
      }
    }
  }
  return ret;
}

int ObDMLResolver::add_oracle_outer_join_dependency(
    const ObIArray<uint64_t> &all_tables,
    const ObIArray<uint64_t> &left_tables,
    uint64_t right_table_id,
    ObIArray<ObBitSet<> > &table_dependencies)
{
  int ret = OB_SUCCESS;
  int64_t right_idx = OB_INVALID_INDEX_INT64;
  CK(table_dependencies.count() == all_tables.count());

  if (OB_SUCC(ret) && OB_UNLIKELY(!has_exist_in_array(all_tables, right_table_id, &right_idx))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Cannot find right table", K(ret), K(all_tables), K(right_table_id));
  }
  CK(0 <= right_idx, right_idx < all_tables.count());

  for (int64_t i = 0; OB_SUCC(ret) && i < left_tables.count(); i++) {
    int64_t left_idx = OB_INVALID_INDEX_INT64;
    // bool 类型返回值
    if (OB_UNLIKELY(!has_exist_in_array(all_tables, left_tables.at(i), &left_idx))) {
      //zhenling.zzg 如果引用的表不是当前stmt的表，则退化成普通的expr
    } else {
      CK(0 <= left_idx, left_idx < all_tables.count());
      table_dependencies.at(right_idx).add_member(left_idx);
    }
  }
  return ret;
}

int ObDMLResolver::build_outer_join_table_by_dependency(
    const ObIArray<ObBitSet<> > &table_dependencies, ObDMLStmt &stmt)
{
  int ret = OB_SUCCESS;
  // TODO(@linsheng): 这里生成的序可能不是最优的, 需要 JO 支持

  ObBitSet<> built_tables;
  TableItem *last_table_item = NULL;
  bool is_found = true;

  CK(table_dependencies.count() == stmt.get_table_items().count());

  // strategy: smallest-numbered available vertex first
  for (int64_t cnt = 0; OB_SUCC(ret) && is_found && cnt < table_dependencies.count(); cnt++) {
    is_found = false;
    for (int64_t i = 0; OB_SUCC(ret) && !is_found && i < table_dependencies.count(); i++) {
      if (!built_tables.has_member(i) && built_tables.is_superset(table_dependencies.at(i))) {
        is_found = true;
        if (NULL == last_table_item) {
          last_table_item = stmt.get_table_item(i);
        } else {
          JoinedTable *new_joined_table = NULL;
          ObJoinType joined_type = table_dependencies.at(i).is_empty()?
              ObJoinType::INNER_JOIN: ObJoinType::LEFT_OUTER_JOIN;

          OZ(create_joined_table_item(joined_type, last_table_item,
                                        stmt.get_table_item(i), new_joined_table));

          last_table_item = static_cast<TableItem*>(new_joined_table);
        }
        // table is built
        OZ((built_tables.add_member)(i));
      }
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(!is_found)) {
      ret = OB_ERR_OUTER_JOIN_NESTED;
      LOG_WARN("two tables cannot be outer-joined to each other", K(ret));
    }
  }

  // clean info
  ARRAY_FOREACH(stmt.get_from_items(), i) {
    OZ((column_namespace_checker_.remove_reference_table)(stmt.get_from_item(i).table_id_));
  }

  OX((stmt.get_joined_tables().reset()));
  OX((stmt.clear_from_item()));

  // add info to stmt
  OZ((stmt.add_from_item)(last_table_item->table_id_,
                          last_table_item->is_joined_table()));

  if (OB_SUCC(ret) && last_table_item->is_joined_table()) {
    OZ((stmt.add_joined_table)(static_cast<JoinedTable*>(last_table_item)));
    OZ(join_infos_.push_back(ResolverJoinInfo(last_table_item->table_id_)));
  }

  OZ((column_namespace_checker_.add_reference_table)(last_table_item));

  return ret;
}

int ObDMLResolver::deliver_outer_join_conditions(ObIArray<ObRawExpr*> &exprs,
                                                  ObIArray<JoinedTable*> &joined_tables)
{
  int ret = OB_SUCCESS;
  for (int64_t i = exprs.count() - 1; OB_SUCC(ret) && i >=0; i--) {
    ObArray<uint64_t> left_tables;
    ObArray<uint64_t> right_tables;
    ObRawExpr *expr = exprs.at(i);

    CK(OB_NOT_NULL(expr));
    OZ(extract_column_with_outer_join_symbol(expr, left_tables, right_tables));
    if (OB_FAIL(ret)) {
      // do nothing
    } else if (OB_UNLIKELY(right_tables.count() > 1)) {
      /**
       * ORA-01468: 一个谓词只能引用一个外部联接的表
       * 01468. 00000 -  "a predicate may reference only one outer-joined table"
       * *Cause:
       * *Action:
       * ----
       * 之前已经检测过了, 防御性代码.
       */
      ret = OB_ERR_MULTI_OUTER_JOIN_TABLE;
      LOG_WARN("a predicate may reference only one outer-joined table", K(ret));
    } else if (1 == right_tables.count()) {
      ObArray<uint64_t> table_ids;
      OZ((append)(table_ids, left_tables));
      OZ((append)(table_ids, right_tables));
      bool is_delivered = false;
      for (int64_t j = 0; OB_SUCC(ret) && !is_delivered && j < joined_tables.count(); j++) {
        // 应该只有一次
        OZ(deliver_expr_to_outer_join_table(expr, table_ids, joined_tables.at(j), is_delivered));
      }

      OZ(remove_outer_join_symbol(expr));
      OZ((expr->extract_info)());
      if (OB_SUCC(ret) && is_delivered) {
        OZ((exprs.remove)(i));
      }
    }
  }
  return ret;
}

int ObDMLResolver::deliver_expr_to_outer_join_table(const ObRawExpr *expr,
                                                    const ObIArray<uint64_t> &table_ids,
                                                    JoinedTable *joined_table,
                                                    bool &is_delivered)
{
  int ret = OB_SUCCESS;

  CK(OB_NOT_NULL(joined_table));
  is_delivered = false;
  if (OB_SUCC(ret)) {
    bool in_left = false;
    bool in_right = false;
    bool force_deliver = false;

    // check left
    CK(OB_NOT_NULL(joined_table->left_table_));
    if (OB_FAIL(ret)) {
      // do nothing
    } else if (joined_table->left_table_->is_joined_table()) {
      JoinedTable *cur_joined = static_cast<JoinedTable*>(joined_table->left_table_);
      for (int64_t i = 0; OB_SUCC(ret) && !in_left && i < cur_joined->single_table_ids_.count(); i++) {
        in_left = has_exist_in_array(table_ids, cur_joined->single_table_ids_.at(i));
      }
    } else {
      in_left = has_exist_in_array(table_ids, joined_table->left_table_->table_id_);
      // 目前不可能, 防御性处理.
      if (in_left && joined_table->joined_type_ == ObJoinType::RIGHT_OUTER_JOIN) {
        force_deliver = true;
      }
    }

    // check right
    CK(OB_NOT_NULL(joined_table->right_table_));
    if (OB_FAIL(ret)) {
      // do nothing
    } else if (joined_table->right_table_->is_joined_table()) {
      JoinedTable *cur_joined = static_cast<JoinedTable*>(joined_table->right_table_);
      for (int64_t i = 0; OB_SUCC(ret) && !in_right && i < cur_joined->single_table_ids_.count(); i++) {
        in_right = has_exist_in_array(table_ids, cur_joined->single_table_ids_.at(i));
      }
    } else {
      in_right = has_exist_in_array(table_ids, joined_table->right_table_->table_id_);
      if (in_right && joined_table->joined_type_ == ObJoinType::LEFT_OUTER_JOIN) {
        force_deliver = true;
      }
    }

    // analyze result, recursive if not found
    if (OB_FAIL(ret)) {
      // do nothing
    } else if (force_deliver || (in_left && in_right)) {
      is_delivered = true;
      OZ((joined_table->join_conditions_.push_back)(const_cast<ObRawExpr*>(expr)));
    } else if (in_left && joined_table->left_table_->is_joined_table()) {
      JoinedTable *cur_joined = static_cast<JoinedTable*>(joined_table->left_table_);
      OZ(SMART_CALL(deliver_expr_to_outer_join_table(expr, table_ids, cur_joined, is_delivered)));
    } else if (in_right && joined_table->right_table_->is_joined_table()) {
      JoinedTable *cur_joined = static_cast<JoinedTable*>(joined_table->right_table_);
      OZ(SMART_CALL(deliver_expr_to_outer_join_table(expr, table_ids, cur_joined, is_delivered)));
    }
  }
  return ret;
}

// 对于 natural join, 把所有一个 joined table 左右相同的列全部放进 using_columns_ 数组.
int ObDMLResolver::fill_same_column_to_using(JoinedTable* &joined_table)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObString, 8> left_column_names;
  ObSEArray<ObString, 8> right_column_names;
  ResolverJoinInfo *join_info = NULL;
  if (OB_ISNULL(joined_table)) {
    LOG_WARN("NULL joined table", K(ret));
  } else if (OB_FAIL(get_columns_from_table_item(joined_table->left_table_, left_column_names))) {
    LOG_WARN("failed to get left column names", K(ret));
  } else if (OB_FAIL(get_columns_from_table_item(joined_table->right_table_, right_column_names))) {
    LOG_WARN("failed to get right column names", K(ret));
  } else if (!get_joininfo_by_id(joined_table->table_id_, join_info)) {
    LOG_WARN("fail to get log info", K(ret));
  } else {
    // No overflow risk because all string from ObColumnSchemaV2.
    /*
     * find all common column and put to using_columns_.
     */
    for (int64_t i = 0; OB_SUCC(ret) && i < left_column_names.count(); i++) {
      for (int64_t j = 0; OB_SUCC(ret) && j < right_column_names.count(); j++) {
        if (ObCharset::case_insensitive_equal(left_column_names.at(i), right_column_names.at(j))) {
          if (OB_FAIL(join_info->using_columns_.push_back(left_column_names.at(i)))) {
            LOG_WARN("failed to push back column name", K(ret));
          }
        }
      }
    }
  }
  // remove redundant using column
  ObIArray<ObString> &using_columns = join_info->using_columns_;
  for (int64_t i = 0; OB_SUCC(ret) && i < using_columns.count(); i++) {
    for (int64_t j = using_columns.count() - 1; OB_SUCC(ret) && j > i; j--) {
      if (ObCharset::case_insensitive_equal(using_columns.at(i), using_columns.at(j))) {
        if (OB_FAIL(using_columns.remove(j))) {
          LOG_WARN("failed to remove redundant column name in using", K(ret));
        }
      }
    }
  }
  return ret;
}

/**
 * 拿一个 TableItem 所有非 hidden 的列,
 * 如果是 JoinedTable, 对子节点递归调用这个函数.
 */
int ObDMLResolver::get_columns_from_table_item(const TableItem *table_item, ObIArray<ObString> &column_names)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table_item) ||
      OB_ISNULL(schema_checker_) ||
      OB_ISNULL(get_stmt())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("NULL param", K(ret), K(table_item), K(schema_checker_));
  } else if (OB_ISNULL(session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session_info_ is null", K(ret));
  } else if (table_item->is_joined_table()) {
    const JoinedTable *joined_table = reinterpret_cast<const JoinedTable*>(table_item);
    if (OB_ISNULL(joined_table->left_table_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("left table of joined table is NULL", K(ret));
    } else if (OB_ISNULL(joined_table->right_table_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("right table of joined table is NULL", K(ret));
    } else if (OB_FAIL(SMART_CALL(get_columns_from_table_item(joined_table->left_table_,
                                                              column_names)))) {
      LOG_WARN("failed to get columns from left table item", K(ret));
    } else if (OB_FAIL(SMART_CALL(get_columns_from_table_item(joined_table->right_table_,
                                                              column_names)))) {
      LOG_WARN("failed to get columns from right table item", K(ret));
    }
  } else if (table_item->is_generated_table() || table_item->is_temp_table()) {
    ObSelectStmt *table_ref = table_item->ref_query_;
    if (OB_ISNULL(table_ref)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("generate table is null");
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < table_ref->get_select_item_size(); ++i) {
      const SelectItem &select_item = table_ref->get_select_item(i);
      if (OB_FAIL(column_names.push_back(select_item.alias_name_))) {
        LOG_WARN("push back column name failed", K(ret));
      }
    }
  } else if (table_item->is_function_table()) {
    OZ (ObResolverUtils::get_all_function_table_column_names(*table_item,
                                                             params_,
                                                             column_names));
  } else if (table_item->is_basic_table() || table_item->is_fake_cte_table()) {
    /**
     * CTE_TABLE is same as BASIC_TABLE or ALIAS_TABLE
     */
    const ObTableSchema *table_schema = NULL;
    if (OB_FAIL(schema_checker_->get_table_schema(session_info_->get_effective_tenant_id(), table_item->ref_id_, table_schema, table_item->is_link_table()))) {
      /**
       * Should not return OB_TABLE_NOT_EXIST.
       * Because tables have been checked in resolve_table already.
       */
      LOG_WARN("get table schema failed", K(ret));
    } else if (OB_ISNULL(table_schema)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get NULL table schema", K(ret));
    }

    for (int64_t i = 0; OB_SUCC(ret) && i < table_schema->get_column_count(); i++) {
      const ObColumnSchemaV2 *column_schema = table_schema->get_column_schema_by_idx(i);
      if (OB_ISNULL(column_schema)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("NULL column schema", K(ret));
      } else if (column_schema->is_hidden()) {
        // do noting
      } else if (OB_FAIL(column_names.push_back(column_schema->get_column_name_str()))) {
        LOG_WARN("failed to push back column name", K(ret));
      }
    }
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("this table item is not supported", K(ret), K(table_item->type_));
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "current table item");
  }
  return ret;
}

int ObDMLResolver::resolve_pseudo_column(
    const ObQualifiedName &q_name,
    ObRawExpr *&real_ref_expr)
{
  int ret = OB_SUCCESS;
  if (0 == q_name.col_name_.case_compare("ORA_ROWSCN")) {
    if (OB_FAIL(resolve_ora_rowscn_pseudo_column(q_name, real_ref_expr))) {
      LOG_WARN("resolve ora_rowscn pseudo column failed", K(ret));
    }
  } else if (lib::is_oracle_mode() &&
             0 == q_name.col_name_.case_compare(OB_HIDDEN_LOGICAL_ROWID_COLUMN_NAME)) {
    if (OB_FAIL(resolve_rowid_pseudo_column(q_name, real_ref_expr))) {
      LOG_WARN("resolve rowid pseudo column failed", K(ret));
    }
  } else {
    ret = OB_ERR_BAD_FIELD_ERROR;
  }
  return ret;
}

int ObDMLResolver::resolve_ora_rowscn_pseudo_column(
    const ObQualifiedName &q_name,
    ObRawExpr *&real_ref_expr)
{
  int ret = OB_SUCCESS;
  ObPseudoColumnRawExpr *pseudo_column_expr = NULL;
  const TableItem *table_item = NULL;
  if (OB_FAIL(column_namespace_checker_.check_rowscn_table_column_namespace(
      q_name, table_item))) {
    LOG_WARN("check rowscn table colum namespace failed", K(ret));
  } else if (OB_ISNULL(table_item)) {
    ret = OB_ERR_BAD_FIELD_ERROR;
    LOG_WARN("ORA_ROWSCN pseudo column only avaliable in basic table", K(ret));
  } else if (OB_FAIL(get_stmt()->get_ora_rowscn_column(table_item->table_id_,
                                                       pseudo_column_expr))) {
      LOG_WARN("failed to get ora_rowscn column", K(ret), K(table_item));
  } else if (pseudo_column_expr != NULL) {
    //this type of pseudo_column_expr has been add
    real_ref_expr = pseudo_column_expr;
  } else if (OB_ISNULL(params_.expr_factory_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("param factory is null", K(ret), K_(params_.expr_factory));
  } else if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_ORA_ROWSCN, pseudo_column_expr))) {
    LOG_WARN("create rowscn pseudo column expr failed", K(ret));
  } else if (OB_ISNULL(pseudo_column_expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pseudo column expr is null", K(ret));
  } else {
    pseudo_column_expr->set_data_type(ObIntType);
    pseudo_column_expr->set_accuracy(ObAccuracy::MAX_ACCURACY[ObIntType]);
    pseudo_column_expr->set_table_id(table_item->table_id_);
    pseudo_column_expr->set_expr_level(current_level_);
    real_ref_expr = pseudo_column_expr;
    OZ(pseudo_column_expr->add_relation_id(get_stmt()->get_table_bit_index(table_item->table_id_)));
    OZ(get_stmt()->get_pseudo_column_like_exprs().push_back(pseudo_column_expr));
    LOG_DEBUG("ora_rowscn_expr build success", K(*pseudo_column_expr));
  }
  return ret;
}

int ObDMLResolver::resolve_rowid_pseudo_column(
    const ObQualifiedName &q_name,
    ObRawExpr *&real_ref_expr)
{
  int ret = OB_SUCCESS;
  const TableItem *table_item = NULL;
  ObDMLStmt *cur_stmt = NULL;
  int32_t cur_level = current_level_;
  ObQueryRefRawExpr *query_ref = NULL;
  if (OB_FAIL(check_rowid_table_column_in_all_namespace(q_name, table_item, cur_stmt, cur_level, query_ref))) {
    LOG_WARN("failed to check rowid table column in all namespace", K(ret));
  } else if (OB_ISNULL(table_item) || OB_ISNULL(cur_stmt)) {
    ret = OB_ERR_BAD_FIELD_ERROR;
    LOG_WARN("could not find table_item", K(ret), K(q_name), K(cur_stmt));
  } else if (table_item->is_generated_table()) {
    if (OB_ISNULL(params_.expr_factory_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("expr factory is null", K(ret));
    } else if (OB_FAIL(ObRawExprUtils::build_empty_rowid_expr(*params_.expr_factory_,
                                                              table_item->table_id_,
                                                              real_ref_expr))) {
      LOG_WARN("build empty rowid expr failed", K(ret));
    }
  } else if (OB_FAIL(resolve_rowid_expr(cur_stmt, *table_item, real_ref_expr))) {
    LOG_WARN("resolve rowid expr failed", K(ret));
  }
  if (OB_SUCC(ret)) {
    if (OB_ISNULL(real_ref_expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("real_ref_expr is null", K(ret));
    } else {
      real_ref_expr->set_expr_level(cur_level);
      ObRawExpr *same_rowid_expr = NULL;
      if (OB_FAIL(cur_stmt->check_and_get_same_rowid_expr(real_ref_expr, same_rowid_expr))) {
        LOG_WARN("failed to check and get same rowid expr", K(ret));
      } else if (same_rowid_expr != NULL) {
        real_ref_expr = same_rowid_expr;
        LOG_DEBUG("rowid_expr build success", K(*real_ref_expr));
      } else {
        LOG_DEBUG("rowid_expr build success", K(*real_ref_expr));
      }
      if (OB_SUCC(ret)) {
        if (cur_level != current_level_) {
          ObRawExpr *exec_param = NULL;
          if (OB_ISNULL(query_ref)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("no subquery is found", K(ret), K(query_ref));
          } else if (OB_FAIL(ObRawExprUtils::get_exec_param_expr(*params_.expr_factory_,
                                                                  query_ref,
                                                                  real_ref_expr,
                                                                  exec_param))) {
            LOG_WARN("failed to get exec param expr", K(ret));
          } else {
            real_ref_expr = exec_param;
          }
        }
      }
    }
  }
  return ret;
}

int ObDMLResolver::check_keystore_status()
{
  int ret = OB_SUCCESS;
  const ObKeystoreSchema *keystore_schema = NULL;
  if (OB_FAIL(schema_checker_->get_keystore_schema(session_info_->get_effective_tenant_id(),
      keystore_schema))) {
    LOG_WARN("fail to get keystore schema", K(ret));
  } else if (OB_ISNULL(keystore_schema)) {
    ret = OB_KEYSTORE_NOT_EXIST;
    LOG_WARN("the keystore is not exist", K(ret));
  } else if (0 == keystore_schema->get_status()) {
    ret = OB_KEYSTORE_NOT_OPEN;
    LOG_WARN("the keystore is not open", K(ret));
  } else if (2 == keystore_schema->get_status()) {
    ret = OB_KEYSTORE_OPEN_NO_MASTER_KEY;
    LOG_WARN("the keystore dont have any master key", K(ret));
  }
  return ret;
}

int ObDMLResolver::resolve_rowid_expr(ObDMLStmt *stmt, const TableItem &table_item, ObRawExpr *&ref_expr)
{
  int ret = OB_SUCCESS;
  ref_expr = NULL;
  if (OB_ISNULL(stmt) || OB_ISNULL(allocator_) ||
      OB_ISNULL(params_.session_info_) || OB_ISNULL(params_.expr_factory_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("stmt or allocator_ or params_ is NULL", K(ret),
        KP(stmt), KP(allocator_), KP(params_.session_info_), KP(params_.expr_factory_));
  } else {
    const ObTableSchema *table_schema = NULL;
    ObSEArray<ObRawExpr*, 4> index_keys;
    ObSysFunRawExpr *rowid_expr = NULL;
    if (OB_FAIL(schema_checker_->get_table_schema(params_.session_info_->get_effective_tenant_id(), table_item.ref_id_, table_schema, table_item.is_link_table()))) {
      LOG_WARN("get table schema failed", K(ret), K(table_item.ref_id_));
    } else if (OB_ISNULL(table_schema)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table_schema is NULL", K(ret));
    } else {
      ObSEArray<uint64_t, 4> col_ids;
      int64_t rowkey_cnt = -1;
      if (OB_FAIL(table_schema->get_column_ids_serialize_to_rowid(col_ids, rowkey_cnt))) {
        LOG_WARN("get col ids failed", K(ret));
      } else if (OB_UNLIKELY(col_ids.count() < rowkey_cnt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("col_ids cnt is invalid", K(ret), K(col_ids), K(rowkey_cnt));
      } else {
        for (int i = 0; OB_SUCC(ret) && i < col_ids.count(); ++i) {
          const ObColumnSchemaV2 *column = NULL;
          ColumnItem *col_item = NULL;
          if (OB_ISNULL(column = table_schema->get_column_schema(col_ids.at(i)))) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("invalid column schema", K(ret), K(col_ids.at(i)));
          } else if (OB_FAIL(resolve_basic_column_item(table_item, column->get_column_name(),
                                                       true, col_item, stmt))) {
            LOG_WARN("failed to resolve basic column item", K(ret));
          } else if (OB_ISNULL(col_item) || OB_ISNULL(col_item->expr_)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("col_item or col_item expr is null", K(ret), KP(col_item));
          } else if (OB_FAIL(index_keys.push_back(col_item->expr_))) {
            LOG_WARN("push back col_item expr failed", K(ret));
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(ObRawExprUtils::build_rowid_expr(stmt,
                                                       *params_.expr_factory_,
                                                       *allocator_,
                                                       *params_.session_info_,
                                                       *table_schema,
                                                       table_item.table_id_,
                                                       index_keys,
                                                       rowid_expr))) {
            LOG_WARN("build rowid expr failed", K(ret));
          } else {
            ref_expr = rowid_expr;
            LOG_TRACE("succeed to resolve rowid expr", K(*ref_expr), K(rowkey_cnt), K(col_ids));
          }
        }
      }
    }
  }
  return ret;
}

// get all column ref expr recursively
int ObDMLResolver::get_all_column_ref(ObRawExpr *expr, ObIArray<ObColumnRefRawExpr*> &arr)
{
  int ret = OB_SUCCESS;
  CK(OB_NOT_NULL(expr));
  if (OB_SUCC(ret)) {
    const ObItemType type = expr->get_expr_type();
    if (T_REF_COLUMN == type) {
      ObColumnRefRawExpr *col_ref = static_cast<ObColumnRefRawExpr*>(expr);
      if (OB_FAIL(add_var_to_array_no_dup(arr, col_ref))) {
        LOG_WARN("push back expr failed", K(ret));
      }
    } else if (IS_EXPR_OP(type) || IS_FUN_SYS_TYPE(type)) {
      ObOpRawExpr *op_expr = static_cast<ObOpRawExpr*>(expr);
      for (int64_t i = 0; OB_SUCC(ret) && i < op_expr->get_param_count(); ++i) {
        if (OB_FAIL(SMART_CALL(get_all_column_ref(op_expr->get_param_expr(i), arr)))) {
          LOG_WARN("get_all_column_ref failed", K(ret));
        }
      }
    } else {
      // do nothing
    }
  }
  return ret;
}

/*@brief, ObDMLResolver::process_part_str 用于将部分特殊关键字添加双引号去除关键字属性，比如：
 * create table t1(SYSTIMESTAMP int) partition by range(SYSTIMESTAMP) (parition "p0" values less than 10000);
 * select SYSTIMESTAMP from dual; ==> select "SYSTIMESTAMP" from dual;
 * 以上才能真正重新解析出来part expr, 否则会误解析为函数，本质上这里表示的为普通列性质,目前已知的有如下关键字：
 * SYSTIMESTAMP、CURRENT_DATE、LOCALTIMESTAMP、CURRENT_TIMESTAMP、SESSIONTIMEZONE、DBTIMEZONE、
 * CONNECT_BY_ISCYCLE、CONNECT_BY_ISLEAF
 * bug:https://work.aone.alibaba-inc.com/issue/32136817
 */

#define ISSPACE(c) ((c) == ' ' || (c) == '\n' || (c) == '\r' || (c) == '\t' || (c) == '\f' || (c) == '\v')

int ObDMLResolver::process_part_str(ObIAllocator &calc_buf,
                                    const ObString &part_str,
                                    ObString &new_part_str)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  char *tmp_buf = NULL;
  const char *part_ptr = part_str.ptr();
  int32_t part_len = part_str.length();
  int64_t buf_len = part_len + part_len / 10 * 2;
  int32_t real_len = 0;
  uint64_t offset = 0;
  if (OB_ISNULL(buf = static_cast<char *>(calc_buf.alloc(buf_len))) ||
      OB_ISNULL(tmp_buf = static_cast<char *>(calc_buf.alloc(part_len)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("alloc memory failed", K(ret), K(buf), K(buf_len), K(tmp_buf), K(part_len));
  } else if (buf_len == part_len) {
    new_part_str.assign_ptr(part_ptr, part_len);
    LOG_TRACE("succeed to process part str", K(new_part_str));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i <= part_len; ++i) {
      if (i < part_len && ISSPACE(part_ptr[i])) {
        /*do nothing*/
      } else if (part_ptr[i] == ',' || i == part_len) {
        if (0 == STRNCASECMP(tmp_buf, "SYSTIMESTAMP", std::min(offset, strlen("SYSTIMESTAMP"))) ||
            0 == STRNCASECMP(tmp_buf, "CURRENT_DATE", std::min(offset, strlen("CURRENT_DATE"))) ||
            0 == STRNCASECMP(tmp_buf, "LOCALTIMESTAMP", std::min(offset, strlen("LOCALTIMESTAMP"))) ||
            0 == STRNCASECMP(tmp_buf, "CURRENT_TIMESTAMP", std::min(offset, strlen("CURRENT_TIMESTAMP"))) ||
            0 == STRNCASECMP(tmp_buf, "SESSIONTIMEZONE", std::min(offset, strlen("SESSIONTIMEZONE"))) ||
            0 == STRNCASECMP(tmp_buf, "DBTIMEZONE", std::min(offset, strlen("DBTIMEZONE"))) ||
            0 == STRNCASECMP(tmp_buf, "CONNECT_BY_ISLEAF", std::min(offset, strlen("CONNECT_BY_ISLEAF"))) ||
            0 == STRNCASECMP(tmp_buf, "CONNECT_BY_ISCYCLE", std::min(offset, strlen("CONNECT_BY_ISCYCLE")))) {
          buf[real_len++] = '\"';
          //由于schema中保存的列名为大写，同时添加双引号的字符串无法保证大小写，因此需要强制转换
          for (int64_t j = 0; j < offset; ++j) {
            tmp_buf[j] = toupper(tmp_buf[j]);
          }
          MEMCPY(buf + real_len, tmp_buf, offset);
          real_len = real_len + offset;
          buf[real_len++] = '\"';
        } else {
          MEMCPY(buf + real_len, tmp_buf, offset);
          real_len = real_len + offset;
        }
        if (part_ptr[i] == ',') {
          buf[real_len++] = ',';
        }
        offset = 0;
        MEMSET(tmp_buf, 0, part_len);
      } else if (OB_UNLIKELY(offset >= part_len)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected error", K(offset), K(part_len), K(ret));
      } else {
        tmp_buf[offset++] = part_ptr[i];
      }
    }
    if (OB_SUCC(ret)) {
      new_part_str.assign_ptr(buf, real_len);
      LOG_TRACE("succeed to process part str", K(new_part_str));
    }
  }
  return ret;
}

int ObDMLResolver::convert_udf_to_agg_expr(ObRawExpr *&expr,
                                           ObRawExpr *parent_expr,
                                           ObExprResolveContext &ctx,
                                           bool need_check_nested /*default false*/)
{
  int ret = OB_SUCCESS;
  ObUDFRawExpr *udf_expr = NULL;
  bool parent_is_win_expr = parent_expr != NULL && parent_expr->is_win_func_expr() ? true : false;
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(expr));
    //only pl agg udf allow have distinct/unique/all as common aggr.
  } else if (expr->is_udf_expr() && !static_cast<ObUDFRawExpr *>(expr)->get_is_aggregate_udf() &&
             static_cast<ObUDFRawExpr *>(expr)->get_is_aggr_udf_distinct()) {
    ret = OB_DISTINCT_NOT_ALLOWED;
    LOG_WARN("distinct/all/unique not allowed here", K(ret));
  } else if (!expr->is_udf_expr() || !static_cast<ObUDFRawExpr *>(expr)->get_is_aggregate_udf()) {
    if (need_check_nested) {
      if (expr->is_aggr_expr()) {
        ObAggFunRawExpr *agg_expr = static_cast<ObAggFunRawExpr *>(expr);
        if (agg_expr->contain_nested_aggr() || parent_is_win_expr) {
          ret = OB_ERR_INVALID_GROUP_FUNC_USE;
          LOG_WARN("Invalid use of group function", K(ret));
        } else if (OB_ISNULL(ctx.aggr_exprs_)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get unexpected null", K(ret), K(ctx.aggr_exprs_));
        } else {
          ObRawExprResolverImpl::AggNestedCheckerGuard agg_guard(ctx);
          bool is_in_nested_aggr = false;
          if (OB_FAIL(agg_guard.check_agg_nested(is_in_nested_aggr))) {
            LOG_WARN("failed to check agg nested.", K(ret));
          } else if (is_in_nested_aggr) {
            int j = 0;
            for (int64_t i = 0; i < ctx.aggr_exprs_->count(); ++i) {
              if (expr != ctx.aggr_exprs_->at(i)) {
                ctx.aggr_exprs_->at(j++) = ctx.aggr_exprs_->at(i);
              } else {/*do nothing*/}
            }
            if (OB_UNLIKELY(j != ctx.aggr_exprs_->count() - 1)) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("get unexpected error", K(ret), K(j), K(ctx.aggr_exprs_->count() - 1));
            } else {
              ctx.aggr_exprs_->pop_back();
              if (OB_FAIL(ctx.aggr_exprs_->push_back(agg_expr))) {
                LOG_WARN("failed to push back", K(ret));
              } else {
                agg_expr->set_in_nested_aggr(true);
              }
            }
          }
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); ++i) {
          if (OB_FAIL(SMART_CALL(convert_udf_to_agg_expr(expr->get_param_expr(i),
                                                         expr,
                                                         ctx,
                                                         need_check_nested)))) {
            LOG_WARN("failed to convert udf to agg expr", K(ret));
          }
        }
      }
    } else {
       for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); ++i) {
        if (OB_FAIL(SMART_CALL(convert_udf_to_agg_expr(expr->get_param_expr(i),
                                                       expr,
                                                       ctx,
                                                       need_check_nested)))) {
          LOG_WARN("failed to convert udf to agg expr", K(ret));
        }
      }
    }
  } else {
    udf_expr = static_cast<ObUDFRawExpr *>(expr);
    ObAggFunRawExpr *agg_expr = NULL;
    ObRawExprResolverImpl::AggNestedCheckerGuard agg_guard(ctx);
    bool is_in_nested_aggr = false;
    if (OB_FAIL(ctx.parents_expr_info_.add_member(IS_AGG))) {
      LOG_WARN("failed to add parents expr info", K(ret));
    } else if (OB_FAIL(agg_guard.check_agg_nested(is_in_nested_aggr))) {
      LOG_WARN("failed to check agg nested.", K(ret));
    } else if (is_in_nested_aggr && parent_is_win_expr) {
      ret = OB_ERR_INVALID_GROUP_FUNC_USE;
      LOG_WARN("Invalid use of group function", K(ret));
    } else if (OB_ISNULL(ctx.aggr_exprs_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret), K(ctx.aggr_exprs_));
    } else if (OB_FAIL(ctx.expr_factory_.create_raw_expr(T_FUN_PL_AGG_UDF, agg_expr))) {
      LOG_WARN("fail to create raw expr", K(ret));
    } else if (!parent_is_win_expr && OB_FAIL(ctx.aggr_exprs_->push_back(agg_expr))) {
      LOG_WARN("store aggr expr failed", K(ret));
    } else {
      agg_expr->set_pl_agg_udf_expr(udf_expr);
      agg_expr->set_param_distinct(udf_expr->get_is_aggr_udf_distinct());
      ObSEArray<ObRawExpr*, 16> param_exprs;
      for (int64_t i = 0; i < udf_expr->get_param_count(); ++i) {
        if (OB_FAIL(SMART_CALL(convert_udf_to_agg_expr(udf_expr->get_param_expr(i), udf_expr,
                                                       ctx, true)))) {
          LOG_WARN("failed to convert udf to agg expr", K(ret));
        } else if (OB_FAIL(agg_expr->get_real_param_exprs_for_update().push_back(
                                                                    udf_expr->get_param_expr(i)))) {
          LOG_WARN("failed to push back expr", K(ret));
        } else {
          /*do nothing */
        }
      }
      if (OB_SUCC(ret)) {
        if (parent_is_win_expr) {
          static_cast<ObWinFunRawExpr *>(parent_expr)->set_agg_expr(agg_expr);
          expr = NULL;//reset pl_agg_udf_expr_ in ObWinFunRawExpr.
        } else {
          expr = agg_expr;
        }
        LOG_TRACE("Succeed to convert udf to agg expr", K(*expr));
      }
    }

    ctx.parents_expr_info_.del_member(IS_AGG);

    if (OB_SUCC(ret) && is_in_nested_aggr) {
      static_cast<ObAggFunRawExpr *>(expr)->set_in_nested_aggr(true);
    } else { /*do nothing.*/ }
  }
  return ret;
}

int ObDMLResolver::get_view_id_for_trigger(const TableItem &view_item, uint64_t &view_id)
{
  int ret = OB_SUCCESS;
  view_id = view_item.ref_id_;
  if (OB_SUCC(ret) && !view_item.alias_name_.empty()) {
    uint64_t tenant_id = session_info_->get_effective_tenant_id();
    ObSchemaGetterGuard *schema_guard = NULL;
    CK (OB_NOT_NULL(schema_checker_));
    CK (OB_NOT_NULL(schema_guard = schema_checker_->get_schema_guard()))
    OZ (schema_guard->get_table_id(tenant_id, view_item.database_name_,
                                   view_item.table_name_, false /*is_index*/,
                                   ObSchemaGetterGuard::ALL_NON_HIDDEN_TYPES, view_id));
  }
  return ret;
}

int ObDMLResolver::add_parent_gen_col_exprs(const ObArray<GenColumnExprInfo> &gen_col_exprs)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(append(gen_col_exprs_, gen_col_exprs))) {
    LOG_WARN("failed to append gen col exprs", K(ret));
  }
  return ret;
}

int ObDMLResolver::check_index_table_has_partition_keys(const ObTableSchema *index_schema,
                                                        const ObPartitionKeyInfo &partition_keys,
                                                        bool &has_part_key)
{
  int ret = OB_SUCCESS;
  has_part_key = true;
  if (OB_ISNULL(index_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("index schema should not be null", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && has_part_key && i < partition_keys.get_size(); ++i) {
      const ObColumnSchemaV2 *column_schema = nullptr;
      uint64_t col_id = OB_INVALID_ID;
      if (OB_FAIL(partition_keys.get_column_id(i, col_id))) {
        LOG_WARN("get_column_id failed", "index", i, K(ret));
      } else if (OB_ISNULL(column_schema = index_schema->get_column_schema(col_id))
                 || column_schema->is_virtual_generated_column()) {
        has_part_key = false;
      }
    }
  }
  return ret;
}

int ObDMLResolver::check_rowid_table_column_in_all_namespace(const ObQualifiedName &q_name,
                                                             const TableItem *&table_item,
                                                             ObDMLStmt *&dml_stmt,
                                                             int32_t &cur_level,
                                                             ObQueryRefRawExpr *&query_ref)
{
  int ret = OB_SUCCESS;
  //first check current resolver
  if (OB_FAIL(column_namespace_checker_.check_rowid_table_column_namespace(q_name, table_item))) {
    LOG_WARN("check rowid table colum namespace failed", K(ret));
  } else if (table_item != NULL) {
    dml_stmt = get_stmt();
    cur_level = current_level_;
    query_ref = get_subquery();
  } else if (get_stmt()->is_select_stmt()) {
    //if don't find table item and current resolver is select resolver then try to find table from
    // parent namespace resolver.
    if (OB_ISNULL(get_parent_namespace_resolver())) {
      /*do nothing*/
    } else if (OB_FAIL(SMART_CALL(get_parent_namespace_resolver()->
             check_rowid_table_column_in_all_namespace(q_name, table_item, dml_stmt, cur_level, query_ref)))) {
      LOG_WARN("failed to check rowid table column in all namespace", K(ret), K(q_name));
    } else {/*do nothing*/}
  }
  LOG_TRACE("Succeed to check rowid table column in all namespace", K(q_name), KPC(table_item),
                                                       KPC(dml_stmt), K(cur_level), KPC(query_ref));
  return ret;
}


int ObDMLResolver::resolve_hints(const ParseNode *node)
{
  int ret = OB_SUCCESS;
  LOG_DEBUG("start to resolve query hints", K(node));
  ObDMLStmt *stmt = NULL;
  ObQueryCtx *query_ctx = NULL;
  ObString qb_name;
  if (OB_ISNULL(stmt = get_stmt()) || OB_ISNULL(query_ctx = stmt->get_query_ctx())) {
    ret = OB_NOT_INIT;
    LOG_WARN("Stmt and query ctx should not be NULL. ", K(ret), K(stmt), K(query_ctx));
  } else if (NULL == node) {
    /* do nothing */
  } else {
    ObQueryHint &query_hint = query_ctx->get_query_hint_for_update();
    ObGlobalHint global_hint;
    ObSEArray<ObHint*, 8> hints;
    bool get_outline_data = false;
    bool filter_embedded_hint = query_hint.has_outline_data() || query_hint.has_user_def_outline();
    if (OB_FAIL(inner_resolve_hints(*node, filter_embedded_hint,
                                    get_outline_data,
                                    global_hint,
                                    hints,
                                    qb_name))) {
      LOG_WARN("failed to call inner resolve hints", K(ret));
    } else if (filter_embedded_hint) {
      /* do nothing */
    } else if (get_outline_data) {
      if (OB_FAIL(query_hint.set_outline_data_hints(global_hint,
                                                    stmt->get_stmt_id(),
                                                    hints))) {
        LOG_WARN("failed to classify outline hints", K(ret));
      }
    } else if (OB_FAIL(query_hint.get_global_hint().merge_global_hint(global_hint))) {
      LOG_WARN("failed to merge global hints", K(ret));
    } else if (OB_FAIL(query_hint.append_hints(stmt->get_stmt_id(), hints))) {
      LOG_WARN("failed to append embedded hints", K(ret));
    }
  }

  // record origin stmt qb name
  if (OB_SUCC(ret) && OB_FAIL(query_ctx->get_query_hint_for_update().set_stmt_id_map_info(*stmt, qb_name))) {
    LOG_WARN("failed to add id name pair", K(ret));
  }
  return ret;
}

int ObDMLResolver::resolve_outline_data_hints()
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = NULL;
  ObQueryCtx *query_ctx = NULL;
  const ParseNode *outline_hint_node = NULL;
  if (OB_ISNULL(stmt = get_stmt()) || OB_ISNULL(query_ctx = stmt->get_query_ctx())) {
    ret = OB_NOT_INIT;
    LOG_WARN("Stmt and query ctx should not be NULL. ", K(ret), K(stmt), K(query_ctx));
  } else if (!stmt->is_root_stmt()) {
    /* do noting */
  } else if (NULL == (outline_hint_node = get_outline_data_hint_node())) {
    /* do noting */
  } else {
    ObQueryHint &query_hint = query_ctx->get_query_hint_for_update();
    ObGlobalHint global_hint;
    ObSEArray<ObHint*, 8> hints;
    bool get_outline_data = false;
    ObString qb_name;
    if (OB_FAIL(inner_resolve_hints(*outline_hint_node, false,
                                    get_outline_data,
                                    global_hint,
                                    hints,
                                    qb_name))) {
      LOG_WARN("failed to resolve outline data hints", K(ret));
    } else if (hints.empty() && !global_hint.has_hint_exclude_concurrent()
               && ObGlobalHint::UNSET_MAX_CONCURRENT != global_hint.max_concurrent_) {
      /* max concurrent outline, do not ignore other hint */
      if (OB_FAIL(query_hint.get_global_hint().assign(global_hint))) {
        LOG_WARN("failed to assign global hint.", K(ret));
      }
    } else if (OB_FAIL(query_hint.set_outline_data_hints(global_hint, stmt->get_stmt_id(),
                                                         hints))) {
      LOG_WARN("failed to classify outline hints", K(ret));
    }
  }
  return ret;
}

const ParseNode *ObDMLResolver::get_outline_data_hint_node()
{
  const ParseNode *node = NULL;
  const ParseNode *select_node = NULL;
  if (NULL == params_.outline_parse_result_
      || NULL == params_.outline_parse_result_->result_tree_
      || NULL == (select_node = params_.outline_parse_result_->result_tree_->children_[0])) {
    /* do nothing */
  } else if (OB_UNLIKELY(T_SELECT != select_node->type_)) {
    LOG_WARN("unexpected node type", "type", get_type_name(select_node->type_));
  } else {
    node = select_node->children_[PARSE_SELECT_HINTS];
  }
  return node;
}

int ObDMLResolver::inner_resolve_hints(const ParseNode &node,
                                       const bool filter_embedded_hint,
                                       bool &get_outline_data,
                                       ObGlobalHint &global_hint,
                                       ObIArray<ObHint*> &hints,
                                       ObString &qb_name)
{
  int ret = OB_SUCCESS;
  get_outline_data = false;
  global_hint.reset();
  hints.reset();
  qb_name.reset();
  if (OB_UNLIKELY(T_HINT_OPTION_LIST != node.type_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected node type", K(ret), K(node.type_));
  } else {
    ObGlobalHint embeded_global_hint;
    ObSEArray<ObHint*, 8> embedded_hints;
    ParseNode *hint_node = NULL;
    bool in_outline_data = false;
    bool reset_outline_hints = false;
    bool resolved_hint = false;
    ObSEArray<ObHint*, 2> cur_hints;
    bool qb_name_conflict = false;
    for (int32_t i = 0; OB_SUCC(ret) && i < node.num_child_; i++) {
      resolved_hint = false;
      cur_hints.reuse();
      if (OB_ISNULL(hint_node = node.children_[i])) {
        /* do nothing */
      } else if (T_QB_NAME == hint_node->type_ && !qb_name_conflict) {
        ObString tmp_qb_name;
        if (OB_FAIL(resolve_qb_name_node(hint_node, tmp_qb_name))) {
          LOG_WARN("failed to resolve qb name node", K(ret));
        } else if (OB_UNLIKELY(!qb_name.empty() && !tmp_qb_name.empty())) {
          qb_name_conflict = true;
          qb_name.reset();
        } else if (!tmp_qb_name.empty()) {
          qb_name = tmp_qb_name;
        }
      } else if (filter_embedded_hint || get_outline_data) {
        /* has valid outline data already, do not resolve hint except qb_name hint */
      } else if (T_BEGIN_OUTLINE_DATA == hint_node->type_) {
        if (OB_LIKELY(!in_outline_data)) {
          in_outline_data = true;
        } else {
          reset_outline_hints = true;
          LOG_TRACE("Unpaired BEGIN_OUTLINE_DATA in outline data", K(ret));
        }
      } else if (T_END_OUTLINE_DATA == hint_node->type_) {
        if (OB_UNLIKELY(!in_outline_data)) {
          LOG_TRACE("Unpaired END_OUTLINE_DATA in outline data");
        } else if (!global_hint.has_valid_opt_features_version()) {
          reset_outline_hints = true;
          LOG_TRACE("get outline data without opt features version");
        } else {
          get_outline_data = true;
          in_outline_data = false;
        }
      } else if (OB_FAIL(resolve_global_hint(*hint_node,
                                             in_outline_data ? global_hint : embeded_global_hint,
                                             resolved_hint))) {
        LOG_WARN("failed to resolve global hint", K(ret));
      } else if (resolved_hint) {
        LOG_DEBUG("resolve global hint node", "type", get_type_name(hint_node->type_));
      } else if (OB_FAIL(resolve_transform_hint(*hint_node, resolved_hint, cur_hints))) {
        LOG_WARN("failed to resolve transform hint", K(ret));
      } else if (!resolved_hint && OB_FAIL(resolve_optimize_hint(*hint_node, resolved_hint,
                                                                 cur_hints))) {
        LOG_WARN("failed to resolve optimize hint", K(ret));
      } else if (OB_UNLIKELY(!resolved_hint)) {
        ret = OB_ERR_HINT_UNKNOWN;
        LOG_WARN("Unknown hint", "hint_name", get_type_name(hint_node->type_));
      } else if (OB_FAIL(append(in_outline_data ? hints : embedded_hints, cur_hints))) {
        LOG_WARN("failed to append hints", K(ret));
      } else {
        LOG_DEBUG("resolved a tranform/optimize hint.", "type", get_type_name(hint_node->type_),
                                                        K(cur_hints));
      }

      if (OB_SUCC(ret) && reset_outline_hints) {
        if (OB_FAIL(append(embedded_hints, hints))) {
          LOG_WARN("failed to append hints", K(ret));
        } else if (OB_FAIL(embeded_global_hint.merge_global_hint(global_hint))) {
          LOG_WARN("failed to merge global hints", K(ret));
        } else {
          reset_outline_hints = false;
          global_hint.reset();
          hints.reset();
        }
      }
    } // end of for

    if (OB_SUCC(ret) && !get_outline_data) {
      if (OB_FAIL(global_hint.merge_global_hint(embeded_global_hint))) {
        LOG_WARN("failed to merge global hints", K(ret));
      } else if (OB_FAIL(append(hints, embedded_hints))) {
        LOG_WARN("failed to append hints", K(ret));
      }
    }
  }
  return ret;
}

// resolve and deal conflict global hint,
// if hint_node is not global hint, set is_global_hint to false.
int ObDMLResolver::resolve_global_hint(const ParseNode &hint_node,
                                       ObGlobalHint &global_hint,
                                       bool &resolved_hint)
{
  int ret = OB_SUCCESS;
  resolved_hint = true;
  ParseNode *child0 = NULL;
  ParseNode *child1 = NULL;

#define CHECK_HINT_PARAM(node, N)                             \
  if (OB_UNLIKELY(N != node.num_child_)                       \
      || (1 <= N && OB_ISNULL(child0 = node.children_[0]))    \
      || (2 <= N && OB_ISNULL(child1 = node.children_[1]))) { \
    ret = OB_ERR_UNEXPECTED;                                  \
    LOG_WARN("unexpected hint node", K(ret),                  \
                  K(node.num_child_), K(child0), K(child1));  \
  } else                                                      \


  switch (hint_node.type_) {
    case T_TOPK: {
      CHECK_HINT_PARAM(hint_node, 2) {
        global_hint.merge_topk_hint(child0->value_, child1->value_);
      }
      break;
    }
    case T_QUERY_TIMEOUT: {
      CHECK_HINT_PARAM(hint_node, 1) {
        global_hint.merge_query_timeout_hint(child0->value_);
      }
      break;
    }
    case T_FROZEN_VERSION: {
      CHECK_HINT_PARAM(hint_node, 1) {
        global_hint.merge_read_consistency_hint(FROZEN, child0->value_);
      }
      break;
    }
    case T_READ_CONSISTENCY: {
      if (hint_node.value_ == 2) {
        global_hint.merge_read_consistency_hint(FROZEN, -1);
      } else if (hint_node.value_ == 3) {
        global_hint.merge_read_consistency_hint(WEAK, -1);
      } else if (hint_node.value_ == 4) {
        global_hint.merge_read_consistency_hint(STRONG, -1);
      } else {
        ret = OB_ERR_HINT_UNKNOWN;
        LOG_WARN("Unknown hint value", "hint_name", get_type_name(hint_node.type_));
      }
      break;
    }
    case T_LOG_LEVEL: {
      CHECK_HINT_PARAM(hint_node, 1) {
        if (NULL != child0->str_value_) {
          ObString log_level;
          log_level.assign_ptr(child0->str_value_, static_cast<int32_t>(child0->str_len_));
          global_hint.merge_log_level_hint(log_level);
        }
      }
      break;
    }
    case T_TRANS_PARAM: {
      CHECK_HINT_PARAM(hint_node, 2) {
        if (child1->type_ == T_VARCHAR) {
          ObString trans_param_str;
          trans_param_str.assign_ptr(child0->str_value_, static_cast<int32_t>(child0->str_len_));
          if (!trans_param_str.case_compare("ENABLE_EARLY_LOCK_RELEASE")) {
            trans_param_str.assign_ptr(child1->str_value_, static_cast<int32_t>(child1->str_len_));
            if (!trans_param_str.case_compare("true")) {
              global_hint.enable_lock_early_release_ = true;
            }
          }
        }
      }
      break;
    }
    case T_OPT_PARAM_HINT: {
      CHECK_HINT_PARAM(hint_node, 2) {
        ObString param_name(child0->str_len_, child0->str_value_);
        ObObj val;
        if (T_VARCHAR == child1->type_) {
          val.set_varchar(child1->str_value_, static_cast<int32_t>(child1->str_len_));
        } else if (T_INT == child1->type_) {
          val.set_int(child1->value_);
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected value type in opt param hint", "type", get_type_name(child1->type_));
        }
        if (OB_SUCC(ret) && OB_FAIL(global_hint.opt_params_.add_opt_param_hint(param_name, val))) {
          LOG_WARN("failed to add opt param hint", K(param_name), K(val));
        }
      }
      break;
    }
    case T_OB_DDL_SCHEMA_VERSION: {
      CHECK_HINT_PARAM(hint_node, 2) {
        ObDDLSchemaVersionHint ddlSchemaVersionHit;
        if (OB_FAIL(resolve_table_relation_in_hint(*child0,
                                                    ddlSchemaVersionHit.table_))) {
          LOG_WARN("failed to resovle simple table list in hint", K(ret));
        } else if (T_INT != child1->type_) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected value type in ddl schema version", "type", get_type_name(child1->type_));
        } else if (OB_FALSE_IT(ddlSchemaVersionHit.schema_version_ = child1->value_))  {
        } else if (OB_FAIL(global_hint.ob_ddl_schema_versions_.push_back(ddlSchemaVersionHit))) {
          LOG_WARN("failed to add ddl schema version hint", K(ret));
        }
      }
      break;
    }
    case T_FORCE_REFRESH_LOCATION_CACHE: {
      global_hint.force_refresh_lc_ = true;
      break;
    }
    case T_USE_PLAN_CACHE: {
      if (1 == hint_node.value_) {
        global_hint.merge_plan_cache_hint(OB_USE_PLAN_CACHE_NONE);
      } else if (2 == hint_node.value_) {
        global_hint.merge_plan_cache_hint(OB_USE_PLAN_CACHE_DEFAULT);
      } else {
        ret = OB_ERR_HINT_UNKNOWN;
        LOG_WARN("Unknown hint.", K(ret));
      }
      break;
    }
    case T_ENABLE_PARALLEL_DML: {
      global_hint.merge_parallel_dml_hint(ObPDMLOption::ENABLE);
      break;
    }
    case T_DISABLE_PARALLEL_DML: {
      global_hint.merge_parallel_dml_hint(ObPDMLOption::DISABLE);
      break;
    }
    case T_TRACE_LOG: {
      // make sure that the trace log starts
      // NOTE: no need to call SET_SAMPLE_FORCE_TRACE_LOG since we just need to make sure
      //       the logging starts. Printing will be forced as long as 'force_trace_log_'
      //       is true, which will be set in the ObSqlCtx.
      LOG_DEBUG("user set trace_log hint");
      global_hint.force_trace_log_ = true; // not used at the moment
      break;
    }
    case T_MAX_CONCURRENT: {
      CHECK_HINT_PARAM(hint_node, 1) {
        global_hint.merge_max_concurrent_hint(child0->value_);
      }
      break;
    }
    case T_NO_PARALLEL: {
      global_hint.merge_parallel_hint(ObGlobalHint::DEFAULT_PARALLEL);
      break;
    }
    case T_PARALLEL: {
      // global parallel hint is processed here. table level hint is processed elsewhere
      CHECK_HINT_PARAM(hint_node, 1) {
        global_hint.merge_parallel_hint(child0->value_);
      }
      break;
    }
    case T_MONITOR: {
      global_hint.monitor_ = true;
      break;
    }
    case T_TRACING:
    case T_STAT: {
      ObSEArray<ObMonitorHint, 8> monitoring_ids;
      if (OB_FAIL(resolve_monitor_ids(hint_node, monitoring_ids))) {
        LOG_WARN("Failed to resolve monitor ids", K(ret));
      } else if (OB_FAIL(global_hint.merge_monitor_hints(monitoring_ids))) {
        LOG_WARN("Failed to add tracing hint", K(ret));
      }
      break;
    }
    case T_DOP: {
      CHECK_HINT_PARAM(hint_node, 2) {
        if (OB_FAIL(global_hint.merge_dop_hint(static_cast<uint64_t>(child0->value_),
                                               static_cast<uint64_t>(child1->value_)))) {
          LOG_WARN("Failed to add dop hint");
        }
      }
      break;
    }
    case T_CURSOR_SHARING_EXACT: {
      global_hint.merge_param_option_hint(ObParamOption::EXACT);
      break;
    }
    case T_OPTIMIZER_FEATURES_ENABLE: {
      CHECK_HINT_PARAM(hint_node, 1) {
        uint64_t version = 0;
        if (OB_FAIL(ObClusterVersion::get_version(child0->str_value_, version))) {
          ret = OB_SUCCESS; // just ignore this invalid hint
          LOG_WARN("failed to get version in hint");
        } else {
          global_hint.merge_opt_features_version_hint(version);
        }
      }
      break;
    }
    case T_NO_QUERY_TRANSFORMATION: {
      global_hint.disable_transform_ = true;
      break;
    }
    case T_NO_COST_BASED_QUERY_TRANSFORMATION: {
      global_hint.disable_cost_based_transform_ = true;
      break;
    }
    default: {
      resolved_hint = false;
      break;
    }
  }
  return ret;
}

int ObDMLResolver::resolve_transform_hint(const ParseNode &hint_node,
                                          bool &resolved_hint,
                                          ObIArray<ObHint*> &trans_hints)
{
  int ret = OB_SUCCESS;
  resolved_hint = true;
  ObTransHint *trans_hint = NULL;
  switch (hint_node.type_) {
    case T_MERGE_HINT:
    case T_NO_MERGE_HINT: {
      if (OB_FAIL(resolve_view_merge_hint(hint_node, trans_hint))) {
        LOG_WARN("failed to resolve hint with qb name param.", K(ret));
      }
      break;
    }
    case T_NO_EXPAND:
    case T_USE_CONCAT: {
      if (OB_FAIL(resolve_or_expand_hint(hint_node, trans_hint))) {
        LOG_WARN("failed to resolve hint with qb name param.", K(ret));
      }
      break;
    }
    case T_INLINE:
    case T_MATERIALIZE: {
      if (OB_FAIL(resolve_materialize_hint(hint_node, trans_hint))) {
        LOG_WARN("failed to resolve materialize hint", K(ret));
      }
      break;
    }
    case T_SEMI_TO_INNER:
    case T_NO_SEMI_TO_INNER: {
      if (OB_FAIL(resolve_semi_to_inner_hint(hint_node, trans_hint))) {
        LOG_WARN("failed to resolve semi to inner hint", K(ret));
      }
      break;
    }
    case T_COALESCE_SQ:
    case T_NO_COALESCE_SQ: {
      if (OB_FAIL(resolve_coalesce_sq_hint(hint_node, trans_hint))) {
        LOG_WARN("failed to resolve coalesce sq hint", K(ret));
      }
      break;
    }
    case T_COUNT_TO_EXISTS:
    case T_NO_COUNT_TO_EXISTS: {
      if (OB_FAIL(resolve_count_to_exists_hint(hint_node, trans_hint))) {
        LOG_WARN("failed to resolve count to exists hint", K(ret));
      }
      break;
    }
    case T_LEFT_TO_ANTI:
    case T_NO_LEFT_TO_ANTI: {
      if (OB_FAIL(resolve_left_to_anti_hint(hint_node, trans_hint))) {
        LOG_WARN("failed to resolve left to anti hint", K(ret));
      }
      break;
    }
    case T_ELIMINATE_JOIN:
    case T_NO_ELIMINATE_JOIN: {
      if (OB_FAIL(resolve_eliminate_join_hint(hint_node, trans_hint))) {
        LOG_WARN("failed to resolve eliminate join hint", K(ret));
      }
      break;
    }
    case T_WIN_MAGIC:
    case T_NO_WIN_MAGIC: {
      if (OB_FAIL(resolve_win_magic_hint(hint_node, trans_hint))) {
        LOG_WARN("failed to resolve win magic hint", K(ret));
      }
      break;
    }
    case T_PLACE_GROUP_BY:
    case T_NO_PLACE_GROUP_BY: {
      if (OB_FAIL(resolve_place_group_by_hint(hint_node, trans_hint))) {
        LOG_WARN("failed to resolve win magic hint", K(ret));
      }
      break;
    }
    case T_UNNEST:
    case T_NO_UNNEST:
    case T_PRED_DEDUCE:
    case T_NO_PRED_DEDUCE:
    case T_PUSH_PRED_CTE:
    case T_NO_PUSH_PRED_CTE:
    case T_REPLACE_CONST:
    case T_NO_REPLACE_CONST:
    case T_SIMPLIFY_ORDER_BY:
    case T_NO_SIMPLIFY_ORDER_BY:
    case T_SIMPLIFY_GROUP_BY:
    case T_NO_SIMPLIFY_GROUP_BY:
    case T_SIMPLIFY_DISTINCT:
    case T_NO_SIMPLIFY_DISTINCT:
    case T_SIMPLIFY_WINFUNC:
    case T_NO_SIMPLIFY_WINFUNC:
    case T_SIMPLIFY_EXPR:
    case T_NO_SIMPLIFY_EXPR:
    case T_SIMPLIFY_LIMIT:
    case T_NO_SIMPLIFY_LIMIT:
    case T_SIMPLIFY_SUBQUERY:
    case T_NO_SIMPLIFY_SUBQUERY:
    case T_FAST_MINMAX:
    case T_NO_FAST_MINMAX:
    case T_PROJECT_PRUNE:
    case T_NO_PROJECT_PRUNE:
    case T_SIMPLIFY_SET:
    case T_NO_SIMPLIFY_SET:
    case T_OUTER_TO_INNER:
    case T_NO_OUTER_TO_INNER:
    case T_PUSH_LIMIT:
    case T_NO_PUSH_LIMIT:
    case T_NO_REWRITE:
    case T_PULLUP_EXPR:
    case T_NO_PULLUP_EXPR: {
      if (OB_FAIL(resolve_normal_transform_hint(hint_node, trans_hint))) {
        LOG_WARN("failed to resolve hint with qb name param.", K(ret));
      }
      break;
    }
    default: {
      resolved_hint = false;
      break;
    }
  }
  if (OB_SUCC(ret) && NULL != trans_hint && OB_FAIL(trans_hints.push_back(trans_hint))) {
    LOG_WARN("failed to push back hint.", K(ret));
  }
  return ret;
}

int ObDMLResolver::resolve_optimize_hint(const ParseNode &hint_node,
                                         bool &resolved_hint,
                                         ObIArray<ObHint*> &opt_hints)
{
  int ret = OB_SUCCESS;
  resolved_hint = true;
  ObOptHint *opt_hint = NULL;
  switch (hint_node.type_) {
    case T_INDEX_HINT:
    case T_NO_INDEX_HINT:
    case T_FULL_HINT:
    case T_USE_DAS_HINT:
    case T_NO_USE_DAS_HINT:
    case T_INDEX_SS_HINT:
    case T_INDEX_SS_ASC_HINT:
    case T_INDEX_SS_DESC_HINT:  {
      if (OB_FAIL(resolve_index_hint(hint_node, opt_hint))) {
        LOG_WARN("failed to resolve index hint", K(ret));
      }
      break;
    }
    case T_ORDERED:
    case T_LEADING: {
      if (OB_FAIL(resolve_join_order_hint(hint_node, opt_hint))) {
        LOG_WARN("failed to resolve leading hint", K(ret));
      }
      break;
    }
    case T_USE_MERGE:
    case T_NO_USE_MERGE:
    case T_USE_HASH:
    case T_NO_USE_HASH:
    case T_USE_NL:
    case T_NO_USE_NL:
    case T_USE_NL_MATERIALIZATION:
    case T_NO_USE_NL_MATERIALIZATION: {
      if (OB_FAIL(resolve_join_hint(hint_node, opt_hints))) {
        LOG_WARN("failed to resolve join hint", K(ret));
      }
      break;
    }
    case T_PX_JOIN_FILTER:
    case T_NO_PX_JOIN_FILTER:
    case T_PX_PART_JOIN_FILTER:
    case T_NO_PX_PART_JOIN_FILTER:  {
      if (OB_FAIL(resolve_join_filter_hint(hint_node, opt_hint))) {
        LOG_WARN("failed to resolve join hint", K(ret));
      }
      break;
    }
    case T_PQ_MAP: {
      if (OB_FAIL(resolve_pq_map_hint(hint_node, opt_hint))) {
        LOG_WARN("failed to resolve pq map hint", K(ret));
      }
      break;
    }
    case T_PQ_DISTRIBUTE:  {
      if (OB_FAIL(resolve_pq_distribute_hint(hint_node, opt_hint))) {
        LOG_WARN("failed to resolve pq distribute hint", K(ret));
      }
      break;
    }
    case T_PQ_SET:  {
      if (OB_FAIL(resolve_pq_set_hint(hint_node, opt_hint))) {
        LOG_WARN("failed to resolve pq set hint", K(ret));
      }
      break;
    }
    case T_USE_LATE_MATERIALIZATION:
    case T_NO_USE_LATE_MATERIALIZATION:
    case T_USE_HASH_AGGREGATE:
    case T_NO_USE_HASH_AGGREGATE:
    case T_GBY_PUSHDOWN:
    case T_NO_GBY_PUSHDOWN:
    case T_USE_HASH_DISTINCT:
    case T_NO_USE_HASH_DISTINCT:
    case T_DISTINCT_PUSHDOWN:
    case T_NO_DISTINCT_PUSHDOWN:
    case T_USE_HASH_SET:
    case T_NO_USE_HASH_SET:
    case T_USE_DISTRIBUTED_DML:
    case T_NO_USE_DISTRIBUTED_DML: {
      if (OB_FAIL(resolve_normal_optimize_hint(hint_node, opt_hint))) {
        LOG_WARN("failed to resolve normal optimize hint.", K(ret));
      }
      break;
    }
    case T_TABLE_PARALLEL: {  // PARALLEL(qb_name tablespec 4)
      if (OB_FAIL(resolve_table_parallel_hint(hint_node, opt_hint))) {
        LOG_WARN("fail to resolve parallel in hint", K(ret));
      }
      break;
    }
    case T_PQ_DISTRIBUTE_WINDOW: {
      CK(2 == hint_node.num_child_);
      OZ(resolve_pq_distribute_window_hint(hint_node, opt_hint));
      break;
    }
    default: {
      resolved_hint = false;
      break;
    }
  }
  if (OB_SUCC(ret) && NULL != opt_hint && OB_FAIL(opt_hints.push_back(opt_hint))) {
    LOG_WARN("failed to push back hint.", K(ret));
  }
  return ret;
}

int ObDMLResolver::resolve_table_parallel_hint(const ParseNode &hint_node,
                                               ObOptHint *&opt_hint)
{
  int ret = OB_SUCCESS;
  opt_hint = NULL;
  ObTableParallelHint *parallel_hint = NULL;
  ObString qb_name;
  int64_t parallel = ObGlobalHint::UNSET_PARALLEL;
  if (OB_UNLIKELY(3 != hint_node.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table parallel hint should have 3 child", K(ret));
  } else if (OB_ISNULL(hint_node.children_[1]) || OB_ISNULL(hint_node.children_[2])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("parallel_node is NULL", K(ret));
  } else if ((parallel = hint_node.children_[2]->value_) < 1) {
    // do nothing
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, parallel_hint))) {
    LOG_WARN("failed to create hint", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
    LOG_WARN("failed to resolve qb name node.", K(ret));
  } else if (OB_FAIL(resolve_table_relation_in_hint(*hint_node.children_[1],
                                                    parallel_hint->get_table()))) {
    LOG_WARN("failed to resovle simple table list in hint", K(ret));
  } else {
    parallel_hint->set_parallel(parallel);
    parallel_hint->set_qb_name(qb_name);
    opt_hint = parallel_hint;
  }
  return ret;
}

int ObDMLResolver::resolve_index_hint(const ParseNode &index_node,
                                      ObOptHint *&opt_hint)
{
  int ret = OB_SUCCESS;
  opt_hint = NULL;
  ObIndexHint *index_hint = NULL;
  ParseNode *table_node = NULL;
  ParseNode *index_name_node = NULL;
  ObString qb_name;
  if (OB_UNLIKELY(2 > index_node.num_child_)
      || OB_ISNULL(table_node = index_node.children_[1])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected index hint", K(ret), K(index_node.type_), K(index_node.num_child_),
                                      K(table_node));
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, index_node.type_, index_hint))) {
    LOG_WARN("failed to create hint", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(index_node.children_[0], qb_name))) {
    LOG_WARN("Failed to resolve qb name node", K(ret));
  } else if (OB_FAIL(resolve_table_relation_in_hint(*table_node, index_hint->get_table()))) {
    LOG_WARN("Resolve table relation fail", K(ret));
  } else if (T_FULL_HINT == index_hint->get_hint_type() ||
             T_USE_DAS_HINT == index_hint->get_hint_type()) {
    index_hint->set_qb_name(qb_name);
    opt_hint = index_hint;
  } else if (OB_UNLIKELY(3 != index_node.num_child_) ||
             OB_ISNULL(index_name_node = index_node.children_[2])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected index hint", K(ret), K(index_node.type_), K(index_node.num_child_),
                                      K(index_name_node));
  } else {
    index_hint->set_qb_name(qb_name);
    index_hint->get_index_name().assign_ptr(index_name_node->str_value_,
                                            static_cast<int32_t>(index_name_node->str_len_));
    opt_hint = index_hint;
  }
  return ret;
}

// for mysql mode, resolve index hint afer from table
int ObDMLResolver::resolve_index_hint(const TableItem &table, const ParseNode &index_hint_node)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = NULL;
  ObQueryCtx *query_ctx = NULL;
  const ParseNode *index_hint_first = NULL;
  const ParseNode *index_list = NULL;
  const ParseNode *index_hint_type = NULL;
  if (OB_ISNULL(stmt = get_stmt()) || OB_ISNULL(query_ctx = stmt->get_query_ctx())) {
    ret = OB_NOT_INIT;
    LOG_WARN("Stmt and query ctx should not be NULL. ", K(ret), K(stmt), K(query_ctx), K(index_hint_first));
  } else if (query_ctx->get_query_hint().has_outline_data() || 0 >= index_hint_node.num_child_) {
    /* do nothing */
  } else if (OB_ISNULL(index_hint_first = index_hint_node.children_[0])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("index_hint_list num child more than 0, but first is NULL", K(ret));
  } else if (OB_UNLIKELY(2 != index_hint_first->num_child_) ||
             OB_ISNULL(index_hint_type = index_hint_first->children_[0]) ||
             OB_UNLIKELY(T_USE != index_hint_type->type_ &&
                         T_FORCE != index_hint_type->type_ &&
                         T_IGNORE != index_hint_type->type_)) {
    ret = OB_ERR_PARSE_SQL;
    LOG_WARN("Parse SQL error, index hint should have 2 children", K(ret), K(index_hint_type));
  } else if (NULL == (index_list = index_hint_first->children_[1])) {
    /* do nothing */
  } else {
    ObItemType type = T_IGNORE == index_hint_type->type_ ? T_NO_INDEX_HINT : T_INDEX_HINT;
    ObSEArray<ObHint*, 2> index_hints;
    ObIndexHint *index_hint = NULL;
    const ParseNode *index_node = NULL;
    for (int i = 0; OB_SUCC(ret) && i < index_list->num_child_; i++) {
      if (OB_ISNULL(index_node = index_list->children_[i])) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Index name node should not be NULL", K(ret), K(index_node));
      } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, type, index_hint))) {
        LOG_WARN("failed to create hint", K(ret));
      } else if (OB_FAIL(index_hints.push_back(index_hint))) {
        LOG_WARN("failed to push back hint.", K(ret));
      } else {
        index_hint->get_index_name().assign_ptr(index_node->str_value_,
                                           static_cast<int32_t>(index_node->str_len_));
        index_hint->get_table().table_name_ = table.get_object_name();
        index_hint->get_table().db_name_ = table.database_name_;
      }
    }
    if (OB_SUCC(ret)) {
      ObQueryHint &query_hint = query_ctx->get_query_hint_for_update();
      if (OB_FAIL(query_hint.append_hints(stmt->get_stmt_id(), index_hints))) {
        LOG_WARN("failed to append embedded hints", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_join_order_hint(const ParseNode &hint_node,
                                           ObOptHint *&opt_hint)
{
  int ret = OB_SUCCESS;
  opt_hint = NULL;
  ObJoinOrderHint *join_order_hint = NULL;
  const ParseNode *table_node = NULL;
  ObString qb_name;
  if (OB_UNLIKELY(1 > hint_node.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("hint with qb name param has no one children.", K(ret));
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, join_order_hint))) {
    LOG_WARN("failed to create hint", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
    LOG_WARN("failed to resolve qb name node", K(ret));
  } else if (T_ORDERED == hint_node.type_) {
    join_order_hint->set_qb_name(qb_name);
    opt_hint = join_order_hint;
  } else if (OB_UNLIKELY(2 != hint_node.num_child_) ||
             OB_ISNULL(table_node = hint_node.children_[1])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected join hint", K(ret), K(hint_node.num_child_));
  } else if (OB_FAIL(resolve_tables_in_leading_hint(table_node, join_order_hint->get_table()))) {
    LOG_WARN("failed to resolve tables in leading hint", K(ret));
  } else {
    join_order_hint->set_qb_name(qb_name);
    opt_hint = join_order_hint;
  }
  return ret;
}

// for use_nl(t1 t2), create two hint use_nl(t1) use_nl(t2)
int ObDMLResolver::resolve_join_hint(const ParseNode &join_node,
                                     ObIArray<ObHint*> &join_hints)
{
  int ret = OB_SUCCESS;
  ObString qb_name;
  const ParseNode *join_tables = NULL;
  if (OB_UNLIKELY(2 != join_node.num_child_) || OB_ISNULL(join_tables = join_node.children_[1])
      || OB_UNLIKELY(T_RELATION_FACTOR_IN_USE_JOIN_HINT_LIST != join_tables->type_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected join hint", K(ret), K(join_node.num_child_));
  } else if (OB_FAIL(resolve_qb_name_node(join_node.children_[0], qb_name))) {
    LOG_WARN("Failed to resolve qb name node", K(ret));
  } else {
    ObJoinHint *join_hint = NULL;
    const ParseNode *cur_table_node = NULL;
    ObSEArray<ObTableInHint, 4> hint_tables;
    for (int64_t i = 0; OB_SUCC(ret) && i < join_tables->num_child_; i++) {
      hint_tables.reuse();
      if (OB_ISNULL(cur_table_node = join_tables->children_[i])) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected null", K(ret), K(cur_table_node));
      } else if (OB_FAIL(resolve_simple_table_list_in_hint(cur_table_node, hint_tables))) {
        LOG_WARN("failed to resovle simple table list in hint", K(ret));
      } else if (hint_tables.empty()) {
        /* do nothing */
      } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, join_node.type_, join_hint))) {
        LOG_WARN("failed to create hint", K(ret));
      } else if (OB_FAIL(join_hint->get_tables().assign(hint_tables))) {
        LOG_WARN("failed to assign hint tables", K(ret));
      } else if (OB_FAIL(join_hints.push_back(join_hint))) {
        LOG_WARN("failed to push back hint.", K(ret));
      } else {
        join_hint->set_qb_name(qb_name);
        LOG_DEBUG("Succ to add join hint", K(*join_hint));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_pq_map_hint(const ParseNode &hint_node,
                                       ObOptHint *&opt_hint)
{
  int ret = OB_SUCCESS;
  opt_hint = NULL;
  ObJoinHint *pq_map_hint = NULL;
  ObTableInHint table_in_hint;
  ObString qb_name;
  if (OB_UNLIKELY(2 != hint_node.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("PQ Map hint should have 2 child", K(ret));
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, pq_map_hint))) {
    LOG_WARN("failed to create hint", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
    LOG_WARN("failed to resolve qb name node.", K(ret));
  } else if (OB_FAIL(resolve_simple_table_list_in_hint(hint_node.children_[1],
                                                       pq_map_hint->get_tables()))) {
    LOG_WARN("failed to resovle simple table list in hint", K(ret));
  } else {
    pq_map_hint->set_qb_name(qb_name);
    opt_hint = pq_map_hint;
  }
  return ret;
}

int ObDMLResolver::resolve_pq_distribute_hint(const ParseNode &hint_node,
                                              ObOptHint *&opt_hint)
{
  int ret = OB_SUCCESS;
  opt_hint = NULL;
  if (OB_UNLIKELY(4 != hint_node.num_child_)
      || OB_ISNULL(hint_node.children_[1])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected PQ Distribute hint node", K(ret), K(hint_node.num_child_));
  } else {
    DistAlgo dist_algo = DistAlgo::DIST_INVALID_METHOD;
    if (OB_ISNULL(hint_node.children_[2]) && OB_ISNULL(hint_node.children_[3])) {
      dist_algo = DistAlgo::DIST_BASIC_METHOD;
    } else if (OB_ISNULL(hint_node.children_[2]) || OB_ISNULL(hint_node.children_[3])) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected PQ Distribute null child", K(ret), K(hint_node.children_[2]), K(hint_node.children_[2]));
    } else {
      ObItemType outer = hint_node.children_[2]->type_;
      ObItemType inner = hint_node.children_[3]->type_;
      if (T_DISTRIBUTE_HASH == outer && T_DISTRIBUTE_HASH == inner) {
        dist_algo = DistAlgo::DIST_HASH_HASH;
      } else if (T_DISTRIBUTE_BROADCAST == outer && T_DISTRIBUTE_NONE == inner) {
        dist_algo = DistAlgo::DIST_BROADCAST_NONE;
      } else if (T_DISTRIBUTE_NONE == outer && T_DISTRIBUTE_BROADCAST == inner) {
        dist_algo = DistAlgo::DIST_NONE_BROADCAST;
      } else if (T_DISTRIBUTE_PARTITION == outer && T_DISTRIBUTE_NONE == inner) {
        dist_algo = DistAlgo::DIST_PARTITION_NONE;
      } else if (T_DISTRIBUTE_NONE == outer && T_DISTRIBUTE_PARTITION == inner) {
        dist_algo = DistAlgo::DIST_NONE_PARTITION;
      } else if (T_DISTRIBUTE_NONE == outer && T_DISTRIBUTE_NONE == inner) {
        dist_algo = DistAlgo::DIST_PARTITION_WISE;
      } else if (T_DISTRIBUTE_LOCAL == outer && T_DISTRIBUTE_LOCAL == inner) {
        dist_algo = DistAlgo::DIST_PULL_TO_LOCAL;
      } else if (T_DISTRIBUTE_BC2HOST == outer && T_DISTRIBUTE_NONE == inner) {
        dist_algo = DistAlgo::DIST_BC2HOST_NONE;
      } else if (T_DISTRIBUTE_NONE == outer && T_DISTRIBUTE_ALL == inner) {
        dist_algo = DistAlgo::DIST_NONE_ALL;
      } else if (T_DISTRIBUTE_ALL == outer && T_DISTRIBUTE_NONE == inner) {
        dist_algo = DistAlgo::DIST_ALL_NONE;
      }
    }

    if (DistAlgo::DIST_INVALID_METHOD != dist_algo) {
      ObJoinHint *pq_dis_hint = NULL;
      ObString qb_name;
      if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, pq_dis_hint))) {
        LOG_WARN("failed to create hint", K(ret));
      } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
        LOG_WARN("failed to resolve query block name", K(ret));
      } else if (OB_FAIL(resolve_simple_table_list_in_hint(hint_node.children_[1],
                                                           pq_dis_hint->get_tables()))) {
        LOG_WARN("failed to resovle simple table list in hint", K(ret));
      } else {
        pq_dis_hint->set_qb_name(qb_name);
        pq_dis_hint->set_dist_algo(dist_algo);
        opt_hint = pq_dis_hint;
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_pq_set_hint(const ParseNode &hint_node,
                                       ObOptHint *&opt_hint)
{
  int ret = OB_SUCCESS;
  opt_hint = NULL;
  ObSEArray<ObItemType, 2> dist_methods;
  ObString qb_name;
  ObString left_branch;
  ObPQSetHint *pq_dis_hint = NULL;
  int64_t random_none_idx = OB_INVALID_INDEX;
  bool is_valid = false;
  if (OB_UNLIKELY(3 != hint_node.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected pq_set hint node", K(ret), K(hint_node.num_child_));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
    LOG_WARN("failed to resolve query block name", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[1], left_branch))) {
    LOG_WARN("failed to resolve query block name", K(ret));
  } else if (OB_FAIL(get_valid_dist_methods(hint_node.children_[2], dist_methods, is_valid))) {
    LOG_WARN("failed to get valid dist methods", K(ret));
  } else if (!is_valid) {
    /* do nothing */
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, pq_dis_hint))) {
    LOG_WARN("failed to create hint", K(ret));
  } else if (OB_FAIL(pq_dis_hint->get_dist_methods().assign(dist_methods))) {
    LOG_WARN("failed to assign dist methods", K(ret));
  } else {
    pq_dis_hint->set_qb_name(qb_name);
    pq_dis_hint->set_left_branch(left_branch);
    opt_hint = pq_dis_hint;
  }
  return ret;
}

int ObDMLResolver::get_valid_dist_methods(const ParseNode *dist_methods_node,
                                          ObIArray<ObItemType> &dist_methods,
                                          bool &is_valid)
{
  int ret = OB_SUCCESS;
  dist_methods.reuse();
  is_valid = false;
  if (OB_ISNULL(dist_methods_node)) {
    is_valid = true;
  } else if (OB_UNLIKELY(T_DISTRIBUTE_METHOD_LIST != dist_methods_node->type_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected pq_set hint node", K(ret), K(get_type_name(dist_methods_node->type_)));
  } else if (OB_UNLIKELY(2 > dist_methods_node->num_child_)) {
    /* do nothing */
  } else {
    is_valid = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < dist_methods_node->num_child_; ++i) {
      if (OB_ISNULL(dist_methods_node->children_[i])) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected null", K(ret), K(i));
      } else if (OB_FAIL(dist_methods.push_back(dist_methods_node->children_[i]->type_))) {
        LOG_WARN("failed to push back", K(ret));
      }
    }
    if (OB_SUCC(ret) && !ObPQSetHint::is_valid_dist_methods(dist_methods)) {
      dist_methods.reuse();
      is_valid = false;
    }
  }
  return ret;
}

int ObDMLResolver::resolve_join_filter_hint(const ParseNode &hint_node,
                                            ObOptHint *&opt_hint)
{
  int ret = OB_SUCCESS;
  opt_hint = NULL;
  ObString qb_name;
  ObJoinFilterHint *join_filter_hint = NULL;
  const ParseNode *join_tables = NULL;
  if (OB_UNLIKELY(3 != hint_node.num_child_) || OB_ISNULL(hint_node.children_[1])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected join filter hint", K(ret), K(hint_node.num_child_));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
    LOG_WARN("Failed to resolve qb name node", K(ret));
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, join_filter_hint))) {
    LOG_WARN("failed to create hint", K(ret));
  } else if (OB_FAIL(resolve_table_relation_in_hint(*hint_node.children_[1],
                                                    join_filter_hint->get_filter_table()))) {
    LOG_WARN("failed to resovle simple table list in hint", K(ret));
  } else if (NULL != hint_node.children_[2] &&
             OB_FAIL(resolve_simple_table_list_in_hint(hint_node.children_[2],
                                                       join_filter_hint->get_left_tables()))) {
    LOG_WARN("failed to resovle simple table list in hint", K(ret));
  } else {
    join_filter_hint->set_qb_name(qb_name);
    opt_hint = join_filter_hint;
  }
  return ret;
}

int ObDMLResolver::resolve_view_merge_hint(const ParseNode &hint_node,
                                           ObTransHint *&hint)
{
  int ret = OB_SUCCESS;
  hint = NULL;
  ObViewMergeHint *view_merge_hint = NULL;
  ObString qb_name;
  ObString parent_qb_name;
  if (OB_UNLIKELY(2 != hint_node.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected child num of view merge hint node", K(ret), K(hint_node.num_child_));
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, view_merge_hint))) {
    LOG_WARN("failed to create hint", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
    LOG_WARN("failed to resolve qb name node.", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[1], parent_qb_name))) {
    LOG_WARN("failed to resolve qb name node.", K(ret));
  } else {
    view_merge_hint->set_parent_qb_name(parent_qb_name);
    view_merge_hint->set_qb_name(qb_name);
    view_merge_hint->set_is_used_query_push_down(hint_node.value_ == 1);
    hint = view_merge_hint;
  }
  return ret;
}

int ObDMLResolver::resolve_or_expand_hint(const ParseNode &hint_node,
                                          ObTransHint *&hint)
{
  int ret = OB_SUCCESS;
  hint = NULL;
  ObOrExpandHint *or_expand_hint = NULL;
  ObString qb_name;
  if (OB_UNLIKELY(2 != hint_node.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected child num of or expand hint node", K(ret), K(hint_node.num_child_));
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, or_expand_hint))) {
    LOG_WARN("failed to create hint", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
    LOG_WARN("failed to resolve qb name node.", K(ret));
  } else {
    const ParseNode *cond_node = hint_node.children_[1];
    if (NULL != cond_node) {
      int32_t length = static_cast<int32_t>(cond_node->str_len_);
      length = length >= ObHint::MAX_EXPR_STR_LENGTH_IN_HINT
               ? ObHint::MAX_EXPR_STR_LENGTH_IN_HINT - 1 : length;
      or_expand_hint->set_expand_condition(cond_node->str_value_, length);
    }
    or_expand_hint->set_qb_name(qb_name);
    hint = or_expand_hint;
  }
  return ret;
}

int ObDMLResolver::resolve_materialize_hint(const ParseNode &hint_node,
                                            ObTransHint *&hint)
{
  int ret = OB_SUCCESS;
  hint = NULL;
  ObMaterializeHint *materialize_hint = NULL;
  ObString qb_name;
  if (OB_UNLIKELY(1 > hint_node.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected child num of materialize hint node", K(ret), K(hint_node.num_child_));
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, materialize_hint))) {
    LOG_WARN("failed to create hint", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
    LOG_WARN("failed to resolve qb name node.", K(ret));
  } else if (2 == hint_node.num_child_) {
    const ParseNode *qb_name_list_node = hint_node.children_[1];
    if (OB_FAIL(resolve_multi_qb_name_list(qb_name_list_node, materialize_hint->get_qb_name_list()))) {
      LOG_WARN("failed to resolve qb name list", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    materialize_hint->set_qb_name(qb_name);
    hint = materialize_hint;
  }
  return ret;
}

int ObDMLResolver::resolve_semi_to_inner_hint(const ParseNode &hint_node,
                                          ObTransHint *&hint)
{
  int ret = OB_SUCCESS;
  hint = NULL;
  ObSemiToInnerHint *semi_to_inner_hint = NULL;
  ObString qb_name;
  if (OB_UNLIKELY(1 > hint_node.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected child num of semi to inner hint node", K(ret), K(hint_node.num_child_));
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, semi_to_inner_hint))) {
    LOG_WARN("failed to create hint", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
    LOG_WARN("failed to resolve qb name node.", K(ret));
  } else if (2 == hint_node.num_child_) {
    const ParseNode *table_node = hint_node.children_[1];
    if (OB_FAIL(resolve_simple_table_list_in_hint(table_node,
                                semi_to_inner_hint->get_tables()))) {
      LOG_WARN("failed to resolve table relatopm in hint", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    semi_to_inner_hint->set_qb_name(qb_name);
    hint = semi_to_inner_hint;
  }
  return ret;
}

int ObDMLResolver::resolve_coalesce_sq_hint(const ParseNode &hint_node,
                                          ObTransHint *&hint)
{
  int ret = OB_SUCCESS;
  hint = NULL;
  ObCoalesceSqHint *coalesce_sq_hint = NULL;
  ObString qb_name;
  if (OB_UNLIKELY(1 > hint_node.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected child num of coalesce_sq hint node", K(ret), K(hint_node.num_child_));
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, coalesce_sq_hint))) {
    LOG_WARN("failed to create hint", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
    LOG_WARN("failed to resolve qb name node.", K(ret));
  } else if (2 == hint_node.num_child_) {
    const ParseNode *qb_name_list_node = hint_node.children_[1];
    if (OB_FAIL(resolve_multi_qb_name_list(qb_name_list_node, coalesce_sq_hint->get_qb_name_list()))) {
      LOG_WARN("failed to resolve qb name list", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    coalesce_sq_hint->set_qb_name(qb_name);
    hint = coalesce_sq_hint;
  }
  return ret;
}

int ObDMLResolver::resolve_count_to_exists_hint(const ParseNode &hint_node,
                                                ObTransHint *&hint)
{
  int ret = OB_SUCCESS;
  hint = NULL;
  ObCountToExistsHint *count_to_exists_hint = NULL;
  ObString qb_name;
  if (OB_UNLIKELY(1 != hint_node.num_child_ && 2 != hint_node.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected child num of count to exists hint node", K(ret), K(hint_node.num_child_));
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, count_to_exists_hint))) {
    LOG_WARN("failed to create hint", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
    LOG_WARN("failed to resolve qb name node.", K(ret));
  } else {
    const ParseNode *qb_name_list_node = hint_node.children_[1];
    if (qb_name_list_node != NULL &&
        OB_FAIL(resolve_qb_name_list(qb_name_list_node, count_to_exists_hint->get_qb_name_list()))) {
      LOG_WARN("failed to resolve qb name list", K(ret));
    } else {
      count_to_exists_hint->set_qb_name(qb_name);
      hint = count_to_exists_hint;
    }
  }
  return ret;
}

int ObDMLResolver::resolve_left_to_anti_hint(const ParseNode &hint_node,
                                             ObTransHint *&hint)
{
  int ret = OB_SUCCESS;
  hint = NULL;
  ObLeftToAntiHint *left_to_anti_hint = NULL;
  ObString qb_name;
  if (OB_UNLIKELY(1 != hint_node.num_child_) && OB_UNLIKELY(2 != hint_node.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected child num of left to anti hint node", K(ret), K(hint_node.num_child_));
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, left_to_anti_hint))) {
    LOG_WARN("failed to create hint", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
    LOG_WARN("failed to resolve qb name node.", K(ret));
  } else {
    const ParseNode *tb_name_list_node = hint_node.children_[1];
    if (NULL != tb_name_list_node &&
        OB_FAIL(resolve_tb_name_list(tb_name_list_node,
                                     left_to_anti_hint->get_tb_name_list()))) {
      LOG_WARN("failed to resolve table name list", K(ret));
    }
    left_to_anti_hint->set_qb_name(qb_name);
    hint = left_to_anti_hint;
  }
  return ret;
}

int ObDMLResolver::resolve_eliminate_join_hint(const ParseNode &hint_node,
                                               ObTransHint *&hint)
{
  int ret = OB_SUCCESS;
  hint = NULL;
  ObEliminateJoinHint *eliminate_join_hint = NULL;
  ObString qb_name;
  if (OB_UNLIKELY(1 != hint_node.num_child_) && OB_UNLIKELY(2 != hint_node.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected num child", K(ret), K(hint_node.num_child_));
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, eliminate_join_hint))) {
    LOG_WARN("failed to create eliminate join hint", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
    LOG_WARN("failed to resolve qb name", K(ret));
  } else {
    const ParseNode *tb_name_list_node = hint_node.children_[1];
    if (NULL != tb_name_list_node &&
        OB_FAIL(resolve_tb_name_list(tb_name_list_node,
                                     eliminate_join_hint->get_tb_name_list()))) {
      LOG_WARN("failed to resolve table name list", K(ret));
    }
    eliminate_join_hint->set_qb_name(qb_name);
    hint = eliminate_join_hint;
  }
  return ret;
}

int ObDMLResolver::resolve_win_magic_hint(const ParseNode &hint_node,
                                               ObTransHint *&hint)
{
  int ret = OB_SUCCESS;
  hint = NULL;
  ObWinMagicHint *win_magic_hint = NULL;
  ObString qb_name;
  if (OB_UNLIKELY(1 != hint_node.num_child_) && OB_UNLIKELY(2 != hint_node.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected num child", K(ret), K(hint_node.num_child_));
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, win_magic_hint))) {
    LOG_WARN("failed to create eliminate join hint", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
    LOG_WARN("failed to resolve qb name", K(ret));
  } else {
    ObSEArray<ObSEArray<ObTableInHint, 4>, 4> tb_name_list;
    const ParseNode *tb_name_list_node = hint_node.children_[1];
    if (NULL != tb_name_list_node &&
        OB_FAIL(resolve_tb_name_list(tb_name_list_node, tb_name_list))) {
      LOG_WARN("failed to resolve table name list", K(ret));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < tb_name_list.count(); i++) {
      for (int64_t j = 0; OB_SUCC(ret) && j < tb_name_list.at(i).count(); j++) {
        if (OB_FAIL(win_magic_hint->get_tb_name_list().push_back(tb_name_list.at(i).at(j)))) {
          LOG_WARN("failed to push table name list", K(ret));
        }
      }
    }
    win_magic_hint->set_qb_name(qb_name);
    hint = win_magic_hint;
    LOG_DEBUG("show win magic hint", K(*win_magic_hint));
  }
  return ret;
}

int ObDMLResolver::resolve_place_group_by_hint(const ParseNode &hint_node,
                                               ObTransHint *&hint)
{
  int ret = OB_SUCCESS;
  hint = NULL;
  ObGroupByPlacementHint *group_by_hint = NULL;
  ObString qb_name;
  if (OB_UNLIKELY(1 != hint_node.num_child_) && OB_UNLIKELY(2 != hint_node.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected num child", K(ret), K(hint_node.num_child_));
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, group_by_hint))) {
    LOG_WARN("failed to create eliminate join hint", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
    LOG_WARN("failed to resolve qb name", K(ret));
  } else {
    const ParseNode *tb_name_list_node = hint_node.children_[1];
    if (NULL != tb_name_list_node &&
        OB_FAIL(resolve_tb_name_list(tb_name_list_node,
                                     group_by_hint->get_tb_name_list()))) {
      LOG_WARN("failed to resolve table name list", K(ret));
    }
    group_by_hint->set_qb_name(qb_name);
    hint = group_by_hint;
    LOG_DEBUG("show group_by_hint hint", K(*group_by_hint));
  }
  return ret;
}

int ObDMLResolver::resolve_tb_name_list(const ParseNode *tb_name_list_node,
                                        ObIArray<ObSEArray<ObTableInHint, 4>> &tb_name_list)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(tb_name_list_node)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else if (tb_name_list_node->type_ == T_RELATION_FACTOR_IN_USE_JOIN_HINT_LIST) {
    const ParseNode *cur_table_node = NULL;
    ObSEArray<ObTableInHint, 4> hint_tables;
    for (int64_t i = 0; OB_SUCC(ret) && i < tb_name_list_node->num_child_; ++i) {
      hint_tables.reuse();
      if (OB_ISNULL(cur_table_node = tb_name_list_node->children_[i])) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else if (OB_FAIL(resolve_simple_table_list_in_hint(cur_table_node, hint_tables))) {
        LOG_WARN("failed to resolve simple table list", K(ret));
      } else if (OB_FAIL(tb_name_list.push_back(hint_tables))) {
        LOG_WARN("failed to push back hint tables", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_normal_transform_hint(const ParseNode &hint_node,
                                                 ObTransHint *&hint)
{
  int ret = OB_SUCCESS;
  hint = NULL;
  ObString qb_name;
  if (OB_UNLIKELY(1 != hint_node.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("hint with qb name param has no one children.", K(ret));
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, hint))) {
    LOG_WARN("failed to create hint", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
    LOG_WARN("failed to resolve qb name node.", K(ret));
  } else {
    hint->set_qb_name(qb_name);
  }
  return ret;
}

int ObDMLResolver::resolve_normal_optimize_hint(const ParseNode &hint_node,
                                                ObOptHint *&hint)
{
  int ret = OB_SUCCESS;
  hint = NULL;
  ObString qb_name;
  if (OB_UNLIKELY(1 != hint_node.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("hint with qb name param has no one children.", K(ret));
  } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, hint_node.type_, hint))) {
    LOG_WARN("failed to create hint", K(ret));
  } else if (OB_FAIL(resolve_qb_name_node(hint_node.children_[0], qb_name))) {
    LOG_WARN("failed to resolve qb name node.", K(ret));
  } else {
    hint->set_qb_name(qb_name);
  }
  return ret;
}

int ObDMLResolver::resolve_monitor_ids(const ParseNode &tracing_node,
                                       ObIArray<ObMonitorHint> &monitoring_ids)
{
  int ret = OB_SUCCESS;
  monitoring_ids.reuse();
  if (OB_UNLIKELY(T_TRACING == tracing_node.type_ && T_STAT == tracing_node.type_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected parse node.", K(ret), K(tracing_node.type_));
  } else {
    ObMonitorHint monitor_hint;
    ObSEArray<uint64_t, 8> ids;
    monitor_hint.flags_ = T_TRACING == tracing_node.type_
                          ? ObMonitorHint::OB_MONITOR_TRACING
                          : ObMonitorHint::OB_MONITOR_STAT;
    ParseNode *node = NULL;
    for (int64_t i = 0; OB_SUCC(ret) && i < tracing_node.num_child_; ++i) {
      if (OB_ISNULL(node = tracing_node.children_[i])) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("Invalid tracing node", K(ret));
      } else if (node->value_ < 0) {
        // Invalid operator id, do nothing.
      } else if (has_exist_in_array(ids, (uint64_t)node->value_)) {
        //do nothing
      } else if (OB_FALSE_IT(monitor_hint.id_ = (uint64_t)node->value_)) {
      } else if (OB_FAIL(monitoring_ids.push_back(monitor_hint))) {
        LOG_WARN("failed to push back", K(ret));
      } else if (OB_FAIL(ids.push_back(monitor_hint.id_))) {
        LOG_WARN("failed to push back", K(ret));
      } else {
        LOG_DEBUG("Resolve tracing hint", K(monitor_hint));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_tables_in_leading_hint(const ParseNode *tables_node,
                                                  ObLeadingTable &leading_table)
{
  int ret = OB_SUCCESS;
  leading_table.reset();
  if (OB_ISNULL(tables_node)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected NULL", K(ret), K(tables_node));
  } else if (T_RELATION_FACTOR_IN_HINT == tables_node->type_) {
    if (OB_FAIL(ObQueryHint::create_hint_table(allocator_, leading_table.table_))) {
      LOG_WARN("fail to create hint table", K(ret));
    } else if (OB_FAIL(resolve_table_relation_in_hint(*tables_node, *leading_table.table_))) {
      LOG_WARN("resolve table relation failed", K(ret));
    }
  } else if (OB_UNLIKELY(T_LINK_NODE != tables_node->type_ || 2 != tables_node->num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected join tables node", K(ret), K(tables_node));
  } else if (OB_FAIL(ObQueryHint::create_leading_table(allocator_, leading_table.left_table_)) ||
             OB_FAIL(ObQueryHint::create_leading_table(allocator_, leading_table.right_table_))) {
    LOG_WARN("fail to create leading table", K(ret));
  } else if (OB_FAIL(SMART_CALL(resolve_tables_in_leading_hint(tables_node->children_[0],
                                                               *leading_table.left_table_)))) {
    LOG_WARN("failed to resolve tables in leading hint", K(ret));
  } else if (OB_FAIL(SMART_CALL(resolve_tables_in_leading_hint(tables_node->children_[1],
                                                               *leading_table.right_table_)))) {
    LOG_WARN("failed to resolve tables in leading hint", K(ret));
  }

  if (OB_SUCC(ret) && OB_UNLIKELY(!leading_table.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected invalid leading table", K(ret), K(leading_table));
  }
  return ret;
}

int ObDMLResolver::resolve_simple_table_list_in_hint(const ParseNode *table_list,
                                                     common::ObIArray<ObTableInHint> &hint_tables)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table_list)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table list is null.", K(ret));
  } else if (T_RELATION_FACTOR_IN_HINT == table_list->type_) {
    ObTableInHint table_in_hint;
    if (OB_FAIL(resolve_table_relation_in_hint(*table_list, table_in_hint))) {
      LOG_WARN("resolve table relation failed", K(ret));
    } else if (OB_FAIL(hint_tables.push_back(table_in_hint))) {
      LOG_WARN("failed to push back", K(ret));
    }
  } else {
    const ParseNode *cur_table = NULL;
    for (int32_t i = 0; OB_SUCC(ret) && i < table_list->num_child_; i++) {
      if (OB_ISNULL(cur_table = table_list->children_[i])) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("table node is null.", K(ret));
      } else if (OB_FAIL(resolve_simple_table_list_in_hint(cur_table, hint_tables))) {
        LOG_WARN("resolve table relation failed", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_table_relation_in_hint(const ParseNode &table_node,
                                                  ObTableInHint &table_in_hint)
{
  int ret = OB_SUCCESS;
  bool is_db_explicit = false;
  table_in_hint.reset();
  if (OB_UNLIKELY(T_RELATION_FACTOR_IN_HINT != table_node.type_ || 2 != table_node.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected table relation node.", K(ret), K(get_type_name(table_node.type_)), K(table_node.num_child_));
  } else if (OB_FAIL(resolve_qb_name_node(table_node.children_[1], table_in_hint.qb_name_))) {
    LOG_WARN("failed to resolve qb name node.", K(ret));
  } else if (OB_FAIL(resolve_table_relation_node_v2(table_node.children_[0],
                                                    table_in_hint.table_name_,
                                                    table_in_hint.db_name_,
                                                    is_db_explicit,
                                                    true,
                                                    false))) {
    LOG_WARN("fail to resolve table relation node", K(ret));
  }
  return ret;
}

int ObDMLResolver::resolve_pq_distribute_window_hint(const ParseNode &node,
                                                     ObOptHint *&hint)
{
  int ret = OB_SUCCESS;
  hint = NULL;
  ObWindowDistHint *win_dist = NULL;
  ObString qb_name;
  const ParseNode *dist_methods_node = NULL;
  CK(T_PQ_DISTRIBUTE_WINDOW == node.type_ && 2 == node.num_child_);
  if (OB_ISNULL(dist_methods_node = node.children_[1])
      || OB_UNLIKELY(T_DISTRIBUTE_METHOD_LIST != dist_methods_node->type_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected pq_distribute_window hint node", K(ret), K(dist_methods_node));
  } else if (OB_FAIL(resolve_qb_name_node(node.children_[0], qb_name))) {
    LOG_WARN("failed to resolve query block name", K(ret));
  } else {
    bool is_valid = true;
    ObSEArray<WinDistAlgo, 2> dist_methods;
    WinDistAlgo method = WinDistAlgo::NONE;
    for (int64_t i = 0; is_valid && OB_SUCC(ret) && i < dist_methods_node->num_child_; ++i) {
      if (OB_ISNULL(dist_methods_node->children_[i])) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected null", K(ret), K(i));
      } else {
        switch (dist_methods_node->children_[i]->type_) {
          case T_DISTRIBUTE_NONE:   method = WinDistAlgo::NONE;   break;
          case T_DISTRIBUTE_HASH:   method = WinDistAlgo::HASH;   break;
          case T_DISTRIBUTE_RANGE:  method = WinDistAlgo::RANGE;  break;
          case T_DISTRIBUTE_LIST:   method = WinDistAlgo::LIST;   break;
          default: is_valid = false;  break;
        }
        if (is_valid && OB_FAIL(dist_methods.push_back(method))) {
          LOG_WARN("failed to push back", K(ret));
        }
      }
    }
    if (OB_FAIL(ret) || !is_valid) {
    } else if (OB_FAIL(ObQueryHint::create_hint(allocator_, node.type_, win_dist))) {
      LOG_WARN("failed to create hint", K(ret));
    } else if (OB_FAIL(win_dist->get_algos().assign(dist_methods))) {
      LOG_WARN("failed to assign dist methods", K(ret));
    } else {
      win_dist->set_qb_name(qb_name);
      hint = win_dist;
    }
  }
  return ret;
}

int ObDMLResolver::resolve_qb_name_node(const ParseNode *qb_name_node, common::ObString &qb_name)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(qb_name_node)) {
    qb_name = ObString::make_empty_string();
  } else if (T_QB_NAME == qb_name_node->type_
             && (OB_UNLIKELY(1 != qb_name_node->num_child_)
                 || OB_ISNULL(qb_name_node = qb_name_node->children_[0]))) {
    ret = OB_ERR_PARSE_SQL;
    LOG_WARN("Parse sql failed", K(ret));
  } else if (T_VARCHAR != qb_name_node->type_ && T_IDENT != qb_name_node->type_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("qb name node type should be T_VARCHAR or T_IDENT", K(ret), K(qb_name_node->type_));
  } else if (static_cast<int32_t>(qb_name_node->str_len_) > OB_MAX_QB_NAME_LENGTH) {
    qb_name = ObString::make_empty_string();
  } else {
    qb_name.assign_ptr(qb_name_node->str_value_, static_cast<int32_t>(qb_name_node->str_len_));
  }
  return ret;
}

int ObDMLResolver::resolve_multi_qb_name_list(const ParseNode *multi_qb_name_list_node, ObIArray<QbNameList> &multi_qb_name_list)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(multi_qb_name_list_node)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected NULL", K(ret), K(multi_qb_name_list_node));
  } else if (T_LINK_NODE != multi_qb_name_list_node->type_) {
    QbNameList qb_name_list;
    if (OB_FAIL(resolve_qb_name_list(multi_qb_name_list_node, qb_name_list.qb_names_))) {
      LOG_WARN("failed to resolve qb name node", K(ret));
    } else if (OB_FAIL(multi_qb_name_list.push_back(qb_name_list))) {
      LOG_WARN("failed to push back qb_name", K(ret));
    }
  } else if (OB_UNLIKELY(2 != multi_qb_name_list_node->num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected join tables node", K(ret), K(multi_qb_name_list_node));
  } else if (OB_FAIL(SMART_CALL(resolve_multi_qb_name_list(multi_qb_name_list_node->children_[0],
                                                           multi_qb_name_list)))) {
    LOG_WARN("failed to resolve qb name list", K(ret));
  } else if (OB_FAIL(SMART_CALL(resolve_multi_qb_name_list(multi_qb_name_list_node->children_[1],
                                                           multi_qb_name_list)))) {
    LOG_WARN("failed to resolve qb name list", K(ret));
  }
  return ret;
}

int ObDMLResolver::resolve_qb_name_list(const ParseNode *qb_name_list_node, 
                                        ObIArray<ObString> &qb_name_list)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(qb_name_list_node)
      || OB_UNLIKELY(T_QB_NAME_LIST != qb_name_list_node->type_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected qb name list node", K(ret), K(qb_name_list_node));
  } else {
    const ParseNode *qb_name_node = NULL;
    ObString qb_name;
    for (int32_t i = 0; OB_SUCC(ret) && i < qb_name_list_node->num_child_; ++i) {
      if (OB_ISNULL(qb_name_node = qb_name_list_node->children_[i])) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected null", K(ret), K(i));
      } else if (OB_FAIL(resolve_qb_name_node(qb_name_node, qb_name))) {
        LOG_WARN("failed to resolve qb name node", K(ret));
      } else if (OB_FAIL(qb_name_list.push_back(qb_name))) {
        LOG_WARN("failed to push back qb_name", K(ret));
      }
    }
  }
  return ret;
}

class CopySchemaExpr : public ObRawExprCopier
{
public:
  CopySchemaExpr(ObRawExprFactory &expr_factory) :
    ObRawExprCopier(expr_factory)
  {}

  int check_need_copy(const ObRawExpr *old_expr, ObRawExpr *&new_expr) override
  {
    int ret = OB_SUCCESS;
    if (OB_ISNULL(old_expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("old expr is null", K(ret), K(old_expr));
    } else if (old_expr->is_column_ref_expr()) {
      new_expr = const_cast<ObRawExpr *>(old_expr);
    }
    return ret;
  }
};

int ObDMLResolver::copy_schema_expr(ObRawExprFactory &factory,
                                    ObRawExpr *expr,
                                    ObRawExpr *&new_expr)
{
  CopySchemaExpr copier(factory);
  return copier.copy(expr, new_expr);
}

int ObDMLResolver::find_table_index_infos(const ObString &dst_index_name,
                                          const TableItem *table_item,
                                          bool &find_it,
                                          int64_t &table_id,
                                          int64_t &ref_id)
{
  int ret = OB_SUCCESS;
  find_it = false;
  const ObTableSchema *data_table_schema = NULL;
  if (OB_ISNULL(table_item) || OB_ISNULL(schema_checker_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(table_item), K(schema_checker_));
  } else if (OB_ISNULL(params_.session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("params_.session_info_ is null", K(ret));
  } else if (OB_FAIL(schema_checker_->get_table_schema(params_.session_info_->get_effective_tenant_id(), table_item->ref_id_, data_table_schema))) {
    LOG_WARN("failed to get table schema", K(ret), K(*table_item));
  } else if (OB_ISNULL(data_table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("table not exists", K(*table_item));
  } else {
    ObSEArray<ObAuxTableMetaInfo, 16> index_infos;
    if (OB_FAIL(data_table_schema->get_simple_index_infos(index_infos, false))) {
      LOG_WARN("get simple index infos failed", K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && !find_it && i < index_infos.count(); ++i) {
        const ObTableSchema *index_schema = NULL;
        ObString src_index_name;
        if (OB_FAIL(schema_checker_->get_table_schema(params_.session_info_->get_effective_tenant_id(), index_infos.at(i).table_id_, index_schema))) {
          LOG_WARN("get index schema from schema checker failed", K(ret));
        } else if (OB_ISNULL(index_schema)) {
          ret = OB_TABLE_NOT_EXIST;
          LOG_WARN("index table not exists", K(index_infos.at(i).table_id_));
        } else if (OB_FAIL(index_schema->get_index_name(src_index_name))) {
          LOG_WARN("fail to get index name", K(ret));
        } else if (0 == src_index_name.case_compare(dst_index_name)) {
          find_it = true;
          table_id = table_item->table_id_;
          ref_id = index_schema->get_table_id();
        } else {
          LOG_TRACE("fine index name", K(src_index_name), K(dst_index_name),
                                       K(index_schema->get_table_name_str()));
        }
      }
    }
  }
  LOG_TRACE("find table index infos", K(dst_index_name), KPC(table_item), K(find_it),
                                      K(table_id), K(ref_id));
  return ret;
}

bool ObDMLResolver::get_joininfo_by_id(int64_t table_id, ResolverJoinInfo *&join_info) {
  bool found = false;
  for (int64_t i = 0; !found && i < join_infos_.count(); i++) {
    if (table_id == join_infos_.at(i).table_id_) {
      found = true;
      join_info = &join_infos_.at(i);
    }
  }
  return found;
}

int ObDMLResolver::get_table_schema(const uint64_t table_id,
                                    const uint64_t ref_table_id,
                                    ObDMLStmt *stmt,
                                    const ObTableSchema *&table_schema)
{
  int ret = OB_SUCCESS;
  const TableItem *table_item = NULL;
  if (NULL == schema_checker_) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(table_id), K(schema_checker_), K(stmt), K(ret));
  } else if (OB_ISNULL(session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session_info_ is null", K(ret));
  } else {
    bool is_link = false;
    is_link = ObSqlSchemaGuard::is_link_table(stmt, table_id);
    OZ(schema_checker_->get_table_schema(session_info_->get_effective_tenant_id(), ref_table_id, table_schema, is_link), table_id, ref_table_id);
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
