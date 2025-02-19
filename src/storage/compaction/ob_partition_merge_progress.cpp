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

#define USING_LOG_PREFIX STORAGE_COMPACTION
#include "ob_partition_merge_progress.h"
#include "storage/compaction/ob_tablet_merge_ctx.h"
#include "storage/compaction/ob_tablet_merge_task.h"
#include "share/scheduler/ob_sys_task_stat.h"
#include "storage/access/ob_table_estimator.h"
#include "storage/access/ob_index_sstable_estimator.h"
#include "ob_tenant_compaction_progress.h"

namespace oceanbase
{
using namespace share;
using namespace common;
using namespace memtable;
using namespace storage;
using namespace blocksstable;

namespace compaction
{

ObPartitionMergeProgress::ObPartitionMergeProgress(common::ObIAllocator &allocator)
  : allocator_(allocator),
    read_info_(nullptr),
    merge_dag_(nullptr),
    scanned_row_cnt_arr_(nullptr),
    output_block_cnt_arr_(nullptr),
    concurrent_cnt_(0),
    estimate_row_cnt_(0),
    estimate_occupy_size_(0),
    avg_row_length_(0),
    latest_update_ts_(ObTimeUtility::fast_current_time()),
    estimated_finish_time_(0),
    pre_scanned_row_cnt_(0),
    pre_output_block_cnt_(0),
    is_updating_(false),
    is_inited_(false)
{
}

ObPartitionMergeProgress::~ObPartitionMergeProgress()
{
  reset();
}

void ObPartitionMergeProgress::reset()
{
  is_inited_ = false;
  merge_dag_ = nullptr;
  if (OB_NOT_NULL(scanned_row_cnt_arr_)) {
    allocator_.free(scanned_row_cnt_arr_);
    scanned_row_cnt_arr_ = nullptr;
  }
  output_block_cnt_arr_ = nullptr;
  estimate_row_cnt_ = 0;
  estimate_occupy_size_ = 0;
  avg_row_length_ = 0;
  latest_update_ts_ = 0;
  estimated_finish_time_ = 0;
  pre_scanned_row_cnt_ = 0;
  pre_output_block_cnt_ = 0;
  concurrent_cnt_ = 0;
  is_updating_ = false;
}


int64_t ObPartitionMergeProgress::to_string(char *buf, const int64_t buf_len) const
{
  int64_t pos = 0;
  if (OB_ISNULL(buf) || buf_len <= 0) {
  } else {
    J_OBJ_START();
    J_KV(K_(is_inited), KPC_(merge_dag), KP_(scanned_row_cnt_arr), KP_(output_block_cnt_arr),
        K_(concurrent_cnt), K_(estimate_row_cnt), K_(estimate_occupy_size),
        K_(latest_update_ts), K_(estimated_finish_time));
    J_OBJ_END();
  }
  return pos;
}

int ObPartitionMergeProgress::init(ObTabletMergeCtx *ctx, const ObTableReadInfo &read_info)
{
  int ret = OB_SUCCESS;
  int64_t *buf = NULL;
  ObTabletMergeDag *merge_dag = nullptr;
  int64_t concurrent_cnt = 0;

  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObPartitionMergeProgress inited twice", K(ret));
  } else if (OB_UNLIKELY(NULL == ctx
      || NULL == (merge_dag = static_cast<ObTabletMergeDag *>(ctx->merge_dag_))
      || 0 == (concurrent_cnt = ctx->get_concurrent_cnt()))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid arguments", K(ret), K(ctx), K(merge_dag), K(concurrent_cnt));
  } else if (OB_ISNULL(buf = static_cast<int64_t *>(allocator_.alloc(sizeof(int64_t) * concurrent_cnt * 2)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("Failed to alloc memory for unit_cnt_arr_", K(ret), K(concurrent_cnt));
  } else {
    // for parallel merge, [0, concurrent_cnt) stores row count, [concurrent_cnt, concurrent_cnt * 2) stores block count
    MEMSET(buf, 0, sizeof(int64_t) * concurrent_cnt * 2);
    scanned_row_cnt_arr_ = buf;
    output_block_cnt_arr_ = buf + concurrent_cnt;

    concurrent_cnt_ = concurrent_cnt;
    merge_dag_ = merge_dag;
    read_info_ = &read_info;

    if (OB_FAIL(estimate(ctx))) {
      LOG_WARN("failed to estimate unit count", K(ret), K(ctx));
    } else {
      is_inited_ = true;
    }
  }
  return ret;
}

int ObPartitionMergeProgress::estimate(ObTabletMergeCtx *ctx)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ctx)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid arguments", K(ret), K(ctx));
  } else {
    const ObIArray<ObITable*> &tables = ctx->tables_handle_.get_tables();
    ObITable *table = nullptr;
    int64_t old_major_data_size = 0;
    if (OB_UNLIKELY(0 == tables.count() || NULL == tables.at(0))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected tables", K(ret), K(tables));
    } else if (is_mini_merge(ctx->param_.merge_type_)) { // only mini merge use estimate row interface
      ObQueryFlag query_flag(ObQueryFlag::Forward,
                             true,   /*is daily merge scan*/
                             true,   /*is read multiple macro block*/
                             true,   /*sys task scan, read one macro block in single io*/
                             false,  /*full row scan flag, obsoleted*/
                             false,  /*index back*/
                             false); /*query_stat*/
      ObTableEstimateBaseInput base_input(query_flag, tables.at(0)->get_key().tablet_id_.id(), tables, ctx->tablet_handle_);

      ObDatumRange whole_range;
      whole_range.set_whole_range();
      ObSEArray<ObDatumRange, 1> ranges;
      if (OB_FAIL(ranges.push_back(whole_range))) {
        LOG_WARN("failed to add ranges", K(ret), K(ranges), K(whole_range));
      } else {
        ObPartitionEst part_estimate;
        ObSEArray<ObEstRowCountRecord, MAX_SSTABLE_CNT_IN_STORAGE> records;
        if (OB_FAIL(ObTableEstimator::estimate_row_count_for_scan(base_input, ranges, part_estimate, records))) {
          LOG_WARN("failed to estimate row counts", K(ret), K(part_estimate), K(records));
        } else {
          estimate_row_cnt_ = MAX(1, part_estimate.physical_row_count_);
          for (int64_t i = tables.count() - 1; i >= 0; --i) {
            if (OB_ISNULL(table = tables.at(i))) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("get unexpected null table", K(ret), K(i), K(table));
            } else if (table->is_memtable()) {
              estimate_occupy_size_ = static_cast<ObMemtable *>(table)->get_occupied_size();
            }
          }
        }
      }
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < tables.count(); ++i) {
        if (OB_ISNULL(table = tables.at(i))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get unexpected null table", K(ret), K(i), K(table));
        } else if (table->is_sstable()) {
          const ObSSTableBasicMeta &meta = static_cast<ObSSTable *>(table)->get_meta().get_basic_meta();
          if (meta.get_total_macro_block_count() <= 0) {
            LOG_DEBUG("table is empty, skip it", K(i), KPC(static_cast<ObSSTable *>(table)));
            continue;
          } else {
            estimate_row_cnt_ += meta.row_count_;
            estimate_occupy_size_ += meta.occupy_size_;
            if (table->is_major_sstable()) {
              old_major_data_size = meta.occupy_size_;
            }
          }
        }
      }
    }

    if (OB_SUCC(ret)) {
      if (estimate_occupy_size_ > 0 && estimate_row_cnt_ > 0) {
        avg_row_length_ = estimate_occupy_size_ * 1.0 / estimate_row_cnt_;
      }
      update_estimated_finish_time_();
      if (ctx->param_.is_tenant_major_merge_) {
          if (OB_FAIL(MTL(ObTenantCompactionProgressMgr*)->update_progress(
                    merge_dag_->get_ctx().param_.merge_version_,
                    estimate_occupy_size_ - old_major_data_size, // estimate_occupy_size_delta
                    0, // scanned_data_size_delta
                    0, // output_block_cnt_delta
                    estimated_finish_time_,
                    false))) {
          LOG_WARN("failed to update progress", K(ret), K(old_major_data_size));
        } else {
          LOG_DEBUG("init() success to update progress", K(ret),
              "param", merge_dag_->get_ctx().param_, K_(estimate_row_cnt), K_(estimate_occupy_size),
              K(old_major_data_size));
        }
      }
    }
  }
  return ret;
}

int ObPartitionMergeProgress::update_row_count(const int64_t idx, const int64_t incre_row_cnt)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObPartitionMergeProgress not inited", K(ret));
  } else if (incre_row_cnt > 0) {
    scanned_row_cnt_arr_[idx] += incre_row_cnt;
    if (REACH_TENANT_TIME_INTERVAL(UPDATE_INTERVAL)) {
      latest_update_ts_ = ObTimeUtility::fast_current_time();
    }
  }
  return ret;
}

int ObPartitionMergeProgress::update_merge_progress(
    const int64_t idx,
    const int64_t scanned_row_cnt,
    const int64_t output_block_cnt)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObPartitionMergeProgress not inited", K(ret));
  } else if (OB_UNLIKELY(0 == estimate_row_cnt_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected zero estimate_total_units_", K(ret));
  } else if (OB_UNLIKELY(idx < 0 || idx >= concurrent_cnt_ || scanned_row_cnt < 0 || output_block_cnt < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid arguments", K(ret), K(idx), K(concurrent_cnt_), K(scanned_row_cnt), K(output_block_cnt));
  } else if (scanned_row_cnt > scanned_row_cnt_arr_[idx] || output_block_cnt > output_block_cnt_arr_[idx]) {
    scanned_row_cnt_arr_[idx] = MAX(scanned_row_cnt_arr_[idx], scanned_row_cnt);
    output_block_cnt_arr_[idx] = MAX(output_block_cnt_arr_[idx], output_block_cnt);

    if (REACH_TENANT_TIME_INTERVAL(UPDATE_INTERVAL)) {
      if (!ATOMIC_CAS(&is_updating_, false, true)) {
        latest_update_ts_ = ObTimeUtility::fast_current_time();

        int64_t scanned_row_cnt = 0;
        int64_t output_block_cnt = 0;

        for (int64_t i = 0; i < concurrent_cnt_; ++i) {
          scanned_row_cnt += scanned_row_cnt_arr_[i];
          output_block_cnt += output_block_cnt_arr_[i];
        }

        if (scanned_row_cnt >= estimate_row_cnt_) {
          estimate_row_cnt_ += scanned_row_cnt - pre_scanned_row_cnt_;
          avg_row_length_ = estimate_occupy_size_ * 1.0 / estimate_row_cnt_;
        }

        // record old value
        pre_scanned_row_cnt_ = scanned_row_cnt;
        pre_output_block_cnt_= output_block_cnt;
        update_estimated_finish_time_();

        ATOMIC_STORE(&is_updating_, false);
      }
    }
  }
  return ret;
}

int ObPartitionMergeProgress::update_merge_info(ObSSTableMergeInfo &merge_info)
{
  int ret = OB_SUCCESS;
  if (concurrent_cnt_ > 1) {
    for (int i = 0; i < concurrent_cnt_; ++i) {
      merge_info.parallel_merge_info_.info_[ObParalleMergeInfo::SCAN_UNITS].add(scanned_row_cnt_arr_[i]);
    }
  }
  return ret;
}


void ObPartitionMergeProgress::update_estimated_finish_time_()
{
  int64_t current_time = ObTimeUtility::fast_current_time();
  if (0 == pre_scanned_row_cnt_) {
    int64_t spend_time = estimate_occupy_size_ / common::OB_DEFAULT_MACRO_BLOCK_SIZE * ObCompactionProgress::MERGE_SPEED
        + ObCompactionProgress::EXTRA_TIME;
    estimated_finish_time_ = spend_time + current_time + UPDATE_INTERVAL;
  } else {
    int64_t rest_time = (estimate_row_cnt_ - pre_scanned_row_cnt_)
        * (current_time - merge_dag_->get_start_time()) / pre_scanned_row_cnt_;
    estimated_finish_time_ = current_time + rest_time + UPDATE_INTERVAL;
  }
}

// called by ObTabletMergeFinishTask::process()
int ObPartitionMergeProgress::finish_merge_progress(const int64_t output_cnt)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObPartitionMergeProgress not inited", K(ret));
  } else {
    pre_output_block_cnt_ = output_cnt;
  }
  return ret;
}

int ObPartitionMergeProgress::get_progress_info(ObCompactionProgress &input_progress)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    input_progress.data_size_ = estimate_occupy_size_;
    input_progress.unfinished_data_size_ =
        0 == estimate_occupy_size_ ? 0 : estimate_occupy_size_ - pre_scanned_row_cnt_ * avg_row_length_;
    input_progress.estimated_finish_time_ = estimated_finish_time_;
  }
  return ret;
}

int ObPartitionMergeProgress::diagnose_progress(ObDiagnoseTabletCompProgress &input_progress)
{
  int ret = OB_SUCCESS;
  if (ObTimeUtility::fast_current_time() - latest_update_ts_ > UPDATE_INTERVAL * NORMAL_UPDATE_PARAM) {
    input_progress.is_suspect_abormal_ = true;
  }
  return ret;
}

/*
 * ObPartitionMajorMergeProgress implement
 * */

int ObPartitionMajorMergeProgress::update_merge_progress(
    const int64_t idx,
    const int64_t scanned_row_cnt,
    const int64_t output_block_cnt)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObPartitionMergeProgress not inited", K(ret));
  } else if (OB_UNLIKELY(0 == estimate_row_cnt_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected zero estimate_total_units_", K(ret));
  } else if (OB_UNLIKELY(idx < 0 || idx >= concurrent_cnt_ || scanned_row_cnt < 0 || output_block_cnt < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid arguments", K(ret), K(idx), K(concurrent_cnt_), K(scanned_row_cnt), K(output_block_cnt));
  } else if (scanned_row_cnt > scanned_row_cnt_arr_[idx] || output_block_cnt > output_block_cnt_arr_[idx]) {
    scanned_row_cnt_arr_[idx] = MAX(scanned_row_cnt_arr_[idx], scanned_row_cnt);
    output_block_cnt_arr_[idx] = MAX(output_block_cnt_arr_[idx], output_block_cnt);

    if (REACH_TENANT_TIME_INTERVAL(UPDATE_INTERVAL)) {
      if (!ATOMIC_CAS(&is_updating_, false, true)) {
        latest_update_ts_ = ObTimeUtility::fast_current_time();

        int64_t scanned_row_cnt = 0;
        int64_t output_block_cnt = 0;

        int64_t scan_data_size_delta = 0;
        int64_t output_block_cnt_delta = 0;
        for (int64_t i = 0; i < concurrent_cnt_; ++i) {
          scanned_row_cnt += scanned_row_cnt_arr_[i];
          output_block_cnt += output_block_cnt_arr_[i];
        }

        if (scanned_row_cnt >= estimate_row_cnt_) {
          estimate_row_cnt_ += scanned_row_cnt - pre_scanned_row_cnt_;
          avg_row_length_ = estimate_occupy_size_ * 1.0 / estimate_row_cnt_;
        }

        // calculate delta value
        scan_data_size_delta = (scanned_row_cnt - pre_scanned_row_cnt_) * avg_row_length_;
        output_block_cnt_delta = output_block_cnt - pre_output_block_cnt_;
        LOG_DEBUG("success to calculate current merge progress", K(ret), K(idx), K(scan_data_size_delta), K(output_block_cnt_delta));
        // record old value
        pre_scanned_row_cnt_ = scanned_row_cnt;
        pre_output_block_cnt_= output_block_cnt;
        update_estimated_finish_time_();

        if (OB_FAIL(MTL(ObTenantCompactionProgressMgr*)->update_progress(
                merge_dag_->get_ctx().param_.merge_version_,
                0, // estimate_occupy_size_delta
                scan_data_size_delta,
                output_block_cnt_delta,
                estimated_finish_time_,
                false/*finish_flag*/))) {
          LOG_WARN("failed to update progress", K(ret), K(idx), K(scan_data_size_delta), K(output_block_cnt_delta));
        } else {
          LOG_DEBUG("update() success to update progress", K(ret),
              "param", merge_dag_->get_ctx().param_, K(scan_data_size_delta), K(output_block_cnt_delta));
        }
        ATOMIC_STORE(&is_updating_, false);
      }
    }
  }
  return ret;
}

int ObPartitionMajorMergeProgress::finish_merge_progress(const int64_t output_cnt)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObPartitionMajorMergeProgress not inited", K(ret));
  } else if (OB_FAIL(MTL(ObTenantCompactionProgressMgr*)->update_progress(
      merge_dag_->get_ctx().param_.merge_version_,
      0, // estimate_occupy_size_delta
      estimate_occupy_size_ - pre_scanned_row_cnt_ * avg_row_length_,// scanned_data_size_delta
      output_cnt - pre_output_block_cnt_,// output_block_cnt_delta
      estimated_finish_time_,
      true/*finish_flag*/,
      &merge_dag_->get_ctx().time_guard_))) {
    LOG_WARN("failed to update progress", K(ret), K(output_cnt), K(estimate_occupy_size_),
        K(pre_scanned_row_cnt_), K(avg_row_length_));
  } else if (OB_FAIL(MTL(ObTenantCompactionProgressMgr*)->update_compression_ratio(
      merge_dag_->get_ctx().param_.merge_version_,
      merge_dag_->get_ctx().merge_info_.get_sstable_merge_info()))) {
    LOG_WARN("failed to update progress", K(ret), K(output_cnt));
  } else {
    LOG_DEBUG("finish() success to update progress", K(ret),
        "param", merge_dag_->get_ctx().param_, K(output_cnt),
        K(pre_scanned_row_cnt_), K(avg_row_length_));
  }
  return ret;
}

} //compaction
} //oceanbase
