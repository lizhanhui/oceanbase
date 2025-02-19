/**
 * Copyright 2014-2016 Alibaba Inc. All Rights Reserved.
 *
 * Date: 2022年3月19日
 *
 * ob_das_spatial_index_lookup_op.cpp is for …
 *
 * Authors:
 *     shengle<xiaoyi.xy@alibaba-inc.com>
 */
#define USING_LOG_PREFIX SQL_DAS
#include "sql/das/ob_das_spatial_index_lookup_op.h"
#include "sql/engine/ob_exec_context.h"
#include "storage/access/ob_dml_param.h"
namespace oceanbase
{
using namespace common;
using namespace storage;
using namespace transaction;
namespace sql
{

bool ObRowKeyCompare::operator()(const ObRowkey *left, const ObRowkey *right)
{
  bool bool_ret = false;
  if (OB_UNLIKELY(common::OB_SUCCESS != result_code_)) {
    //do nothing
  } else if (OB_UNLIKELY(NULL == left)
             || OB_UNLIKELY(NULL == right)) {
    result_code_ = common::OB_INVALID_ARGUMENT;
    LOG_WARN("Invaid argument, ", KP(left), KP(right), K_(result_code));
  } else {
    bool_ret = (*left) < (*right);
  }
  return bool_ret;
}

int ObSpatialIndexLookupOp::init(const ObDASScanCtDef *lookup_ctdef,
                                 ObDASScanRtDef *lookup_rtdef,
                                 const ObDASScanCtDef *index_ctdef,
                                 ObDASScanRtDef *index_rtdef,
                                 ObTxDesc *tx_desc,
                                 ObTxReadSnapshot *snapshot,
                                 const ObMbrFilterArray *mbr_filters)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObLocalIndexLookupOp::init(lookup_ctdef, lookup_rtdef, index_ctdef,
                                         index_rtdef, tx_desc, snapshot))) {
    LOG_WARN("ObLocalIndexLookupOp init failed", K(ret));
  } else if (OB_ISNULL(mbr_filters)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("mbr filter is null", K(ret));
  } else {
    mbr_filters_ = mbr_filters;
    is_inited_ = false;
  }
  return ret;
}

int ObSpatialIndexLookupOp::reset_lookup_state(bool need_switch_param)
{
  is_inited_ = false;
  return ObLocalIndexLookupOp::reset_lookup_state(need_switch_param);
}

int ObSpatialIndexLookupOp::filter_by_mbr(const ObObj &mbr_obj, bool &pass_through)
{
  int ret = OB_SUCCESS;
  pass_through = true;
  ObString mbr_str = mbr_obj.get_varchar();
  ObSpatialMBR idx_spa_mbr;
  bool is_point = (WKB_POINT_DATA_SIZE == mbr_str.length());

  if (OB_FAIL(ObSpatialMBR::from_string(mbr_str, ObGeoRelationType::T_INVALID, idx_spa_mbr, is_point))) {
    LOG_WARN("fail to create index spatial mbr", K(ret), K(mbr_obj));
  } else {
    idx_spa_mbr.is_point_ = is_point;
    for (int64_t i = 0; OB_SUCC(ret) && i < mbr_filters_->count() && pass_through; i++) {
      const ObSpatialMBR &spa_mbr = mbr_filters_->at(i);
      idx_spa_mbr.is_geog_ = spa_mbr.is_geog();
      if (OB_FAIL(idx_spa_mbr.filter(spa_mbr, spa_mbr.get_type(), pass_through))) {
        LOG_WARN("fail to filter by s2", K(ret), K(spa_mbr), K(idx_spa_mbr));
      }
    }
  }
  return ret;
}

int ObSpatialIndexLookupOp::save_rowkeys()
{
  int ret = OB_SUCCESS;
  int64_t simulate_batch_row_cnt = - EVENT_CALL(EventTable::EN_TABLE_LOOKUP_BATCH_ROW_COUNT);
  int64_t default_row_batch_cnt  = simulate_batch_row_cnt > 0 ? simulate_batch_row_cnt : MAX_NUM_PER_BATCH;
  LOG_DEBUG("simulate lookup row batch count", K(simulate_batch_row_cnt), K(default_row_batch_cnt));
  ObStoreRowkey src_key;
  ObExtStoreRowkey dest_key; // original and collation_free rowkeys
  const ObRowkey *idx_row = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && i < default_row_batch_cnt; ++i) {
    if (OB_FAIL(sorter_.get_next_item(idx_row))) {
      if (OB_ITER_END != ret) {
        LOG_WARN("fail to get next sorted item", K(ret), K(i));
      }
    } else if (last_rowkey_ != *idx_row) {
      ObNewRange lookup_range;
      uint64_t ref_table_id = lookup_ctdef_->ref_table_id_;
      if (OB_FAIL(lookup_range.build_range(ref_table_id, *idx_row))) {
        LOG_WARN("build lookup range failed", K(ret), K(ref_table_id), K(*idx_row));
      } else if (FALSE_IT(lookup_range.group_idx_ = get_index_group_cnt() - 1)) {
      } else if (OB_FAIL(scan_param_.key_ranges_.push_back(lookup_range))) {
       LOG_WARN("store lookup key range failed", K(ret), K(scan_param_));
      }
      last_rowkey_ = *idx_row;
      LOG_DEBUG("build data table range", K(ret), K(*idx_row), K(lookup_range), K(scan_param_.key_ranges_.count()));
    }
  }
  return ret;
}

int ObSpatialIndexLookupOp::get_next_row()
{
  int ret = OB_SUCCESS;
  if (is_inited_ == false) {
    /* init external sorter and mbr whole range*/
    cmp_ret_ = OB_SUCCESS;
    new (&comparer_) ObRowKeyCompare(cmp_ret_);
    const int64_t file_buf_size = ObExternalSortConstant::DEFAULT_FILE_READ_WRITE_BUFFER;
    const int64_t expire_timestamp = 0;
    const int64_t buf_limit = SORT_MEMORY_LIMIT;
    const uint64_t tenant_id = MTL_ID();
    sorter_.clean_up();
    if (OB_FAIL(sorter_.init(buf_limit, file_buf_size, expire_timestamp, tenant_id, &comparer_))) {
      STORAGE_LOG(WARN, "fail to init external sorter", K(ret));
    } else {
      is_whole_range_ = (mbr_filters_->count() == 0);
      is_inited_ = true;
      while (OB_SUCC(ret)) {
        index_rtdef_->p_pd_expr_op_->clear_evaluated_flag();
        if (OB_FAIL(rowkey_iter_->get_next_row())) {
          if (OB_ITER_END != ret) {
            LOG_WARN("get next row from index scan failed", K(ret));
          } else if (is_group_scan_) {
            // switch to next index group
            if (OB_FAIL(switch_rowkey_scan_group())) {
              if (OB_ITER_END != ret) {
                LOG_WARN("rescan index operator failed", K(ret));
              } else {
                LOG_DEBUG("switch group end",
                         K(get_index_group_cnt()), K(lookup_rowkey_cnt_), KP(this));
              }
            } else {
              inc_index_group_cnt();
              LOG_DEBUG("switch to next index batch to fetch rowkey",
                       K(get_index_group_cnt()), K(lookup_rowkey_cnt_), KP(this));
            }
          }
        } else if (OB_FAIL(process_data_table_rowkey())) {
          LOG_WARN("process data table rowkey with das failed", K(ret));
        } else {
          ++lookup_rowkey_cnt_;
        }
      }

      if (OB_ITER_END == ret && lookup_rowkey_cnt_ > 0) {
        if (OB_FAIL(sorter_.do_sort(true))) {
          LOG_WARN("sort candidates failed", K(ret));
        }
      }
    }
  }

  bool got_next_row = false;
  do {
    switch (state_) {
      case INDEX_SCAN: {
        if (OB_FAIL(save_rowkeys())) {
          if (ret != OB_ITER_END) {
            LOG_WARN("failed get rowkey from sorter", K(ret));
          }
        }
        if (OB_SUCC(ret) || OB_ITER_END == ret) {
          state_ = DO_LOOKUP;
          index_end_ = (OB_ITER_END == ret);
          ret = OB_SUCCESS;
        }
        break;
      }
      case DO_LOOKUP: {
        lookup_row_cnt_ = 0;
        if (OB_FAIL(do_index_lookup())) {
          LOG_WARN("do index lookup failed", K(ret));
        } else {
          state_ = OUTPUT_ROWS;
        }
        break;
      }
      case OUTPUT_ROWS: {
        lookup_rtdef_->p_pd_expr_op_->clear_evaluated_flag();
        if (scan_param_.key_ranges_.empty()) {
          ret= OB_ITER_END;
          state_ = FINISHED;
        } else if (OB_FAIL(lookup_iter_->get_next_row())) {
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
            if (need_next_index_batch()) {
              // reuse lookup_iter_ only
              ObLocalIndexLookupOp::reset_lookup_state(false);
              index_end_ = false;
              state_ = INDEX_SCAN;
            } else {
              state_ = FINISHED;
            }
          } else {
            LOG_WARN("look up get next row failed", K(ret));
          }
        } else {
          got_next_row = true;
          ++lookup_row_cnt_;
        }
        break;
      }
      case FINISHED: {
        if (OB_SUCC(ret) || OB_ITER_END == ret) {
          ret = OB_ITER_END;
        }
        break;
      }
      default: {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected state", K(state_));
      }
    }
  } while (!got_next_row && OB_SUCC(ret));
  return ret;
}

int ObSpatialIndexLookupOp::process_data_table_rowkey()
{
  int ret = OB_SUCCESS;
  // last output expr is mbr expr
  int64_t rowkey_cnt = index_ctdef_->result_output_.count() - 1;
  ObObj *obj_ptr = nullptr;
  void *buf = nullptr;
  ObNewRange lookup_range;
  if (OB_ISNULL(buf = lookup_rtdef_->scan_allocator_.alloc(sizeof(ObObj) * rowkey_cnt))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("allocate buffer failed", K(ret), K(rowkey_cnt));
  } else {
    obj_ptr = new(buf) ObObj[rowkey_cnt];
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_cnt; ++i) {
    ObObj tmp_obj;
    ObExpr *expr = index_ctdef_->result_output_.at(i);
    if (T_PSEUDO_GROUP_ID == expr->type_) {
      // do nothing
    } else {
      ObDatum &col_datum = expr->locate_expr_datum(*lookup_rtdef_->eval_ctx_);
      if (OB_FAIL(col_datum.to_obj(tmp_obj, expr->obj_meta_, expr->obj_datum_map_))) {
        LOG_WARN("convert datum to obj failed", K(ret));
      } else if (OB_FAIL(ob_write_obj(lookup_rtdef_->scan_allocator_, tmp_obj, obj_ptr[i]))) {
        LOG_WARN("deep copy rowkey value failed", K(ret), K(tmp_obj));
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObRowkey table_rowkey(obj_ptr, rowkey_cnt);
    ObObj mbr_obj;
    bool pass_through = true;
    ObExpr *mbr_expr = index_ctdef_->result_output_.at(rowkey_cnt);
    ObDatum &mbr_datum = mbr_expr->locate_expr_datum(*lookup_rtdef_->eval_ctx_);
    if (OB_FAIL(mbr_datum.to_obj(mbr_obj, mbr_expr->obj_meta_, mbr_expr->obj_datum_map_))) {
      LOG_WARN("convert datum to obj failed", K(ret));
    } else if (!is_whole_range_ && OB_FAIL(filter_by_mbr(mbr_obj, pass_through))) {
      LOG_WARN("filter mbr failed", K(ret));
    } else if (!is_whole_range_ && pass_through) {
      // not target
    } else if (OB_FAIL(sorter_.add_item(table_rowkey))) {
      LOG_WARN("filter mbr failed", K(ret));
    } else {
      LOG_TRACE("add rowkey success", K(table_rowkey), K(obj_ptr), K(obj_ptr[0]), K(rowkey_cnt));
    }
  }
  return ret;
}


}  // namespace sql
}  // namespace oceanbase