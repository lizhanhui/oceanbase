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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_dbms_stats_utils.h"
#include "share/stat/ob_opt_column_stat.h"
#include "share/object/ob_obj_cast.h"
#include "share/stat/ob_opt_stat_manager.h"
#include "share/stat/ob_opt_table_stat.h"
#include "share/schema/ob_schema_struct.h"
#include "sql/engine/ob_exec_context.h"

namespace oceanbase
{
namespace common
{
int ObDbmsStatsUtils::get_part_info(const ObTableStatParam &param,
                                    const ObExtraParam &extra,
                                    PartInfo &part_info)
{
  int ret = OB_SUCCESS;
  if (extra.type_ == TABLE_LEVEL) {
    part_info.part_name_ = param.tab_name_;
    part_info.part_stattype_ = param.stattype_;
    part_info.part_id_ = param.global_part_id_;
    part_info.tablet_id_ = param.global_part_id_;
  } else if (extra.type_ == PARTITION_LEVEL) {
    if (OB_UNLIKELY(extra.nth_part_ >= param.part_infos_.count())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid partition index",
               K(ret), K(extra.nth_part_), K(param.part_infos_.count()));
    } else {
      part_info = param.part_infos_.at(extra.nth_part_);
    }
  } else if (extra.type_ == SUBPARTITION_LEVEL) {
    if (OB_UNLIKELY(extra.nth_part_ >= param.subpart_infos_.count())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid subpartition index",
               K(ret), K(extra.nth_part_), K(param.subpart_infos_.count()));
    } else {
      part_info = param.subpart_infos_.at(extra.nth_part_);
    }
  }
  return ret;
}

int ObDbmsStatsUtils::init_col_stats(ObIAllocator &allocator,
                                     int64_t col_cnt,
                                     ObIArray<ObOptColumnStat*> &col_stats)
{
  int ret = OB_SUCCESS;
  void *ptr = NULL;
  if (OB_UNLIKELY(col_cnt <= 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected error, expected specify column cnt is great 0", K(ret), K(col_cnt));
  } else if (OB_FAIL(col_stats.prepare_allocate(col_cnt))) {
    LOG_WARN("failed to prepare allocate column stat", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
      ObOptColumnStat *&col_stat = col_stats.at(i);
      if (OB_ISNULL(ptr = allocator.alloc(sizeof(ObOptColumnStat)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("memory is not enough", K(ret), K(ptr));
      } else {
        col_stat = new (ptr) ObOptColumnStat(allocator);
      }
    }
  }
  return ret;
}

/* @brief ObDbmsStatsUtils::check_range_skew, check data is skewed or not
 * Based on Oracle 12c:
 * 1.for frequency histogram:
 *  if the number of value in all buckets is the same, then it's even distributed, Otherwise, it's
 *  skewed.
 * 2.for hybrid histogram: ==> refine it, TODO@jiangxiu.wt
 *  if the repeat count of value in all buckets is less than total_not_null_row_count / bucket_num,
 *  then it's even distributed, Otherwise, it's skewed.
 */
int ObDbmsStatsUtils::check_range_skew(ObHistType hist_type,
                                       const ObIArray<ObHistBucket> &bkts,
                                       int64_t standard_cnt,
                                       bool &is_even_distributed)
{
  int ret = OB_SUCCESS;
  is_even_distributed = false;
  if (hist_type == ObHistType::FREQUENCY) {
    is_even_distributed = true;
    for (int64_t i = 0; is_even_distributed && i < bkts.count(); ++i) {
      if (i == 0) {
        is_even_distributed = standard_cnt == bkts.at(i).endpoint_num_;
      } else {
        is_even_distributed = standard_cnt == bkts.at(i).endpoint_num_ -
                                                                       bkts.at(i - 1).endpoint_num_;
      }
    }
  } else if (hist_type == ObHistType::HYBIRD) {
    is_even_distributed = true;
    for (int64_t i = 0; is_even_distributed && i < bkts.count(); ++i) {
      is_even_distributed = bkts.at(i).endpoint_repeat_count_ <= standard_cnt;
    }
  } else {/*do nothing*/}
  return ret;
}

int ObDbmsStatsUtils::batch_write(share::schema::ObSchemaGetterGuard *schema_guard,
                                  const uint64_t tenant_id,
                                  ObIArray<ObOptTableStat *> &table_stats,
                                  ObIArray<ObOptColumnStat*> &column_stats,
                                  const int64_t current_time,
                                  const bool is_index_stat,
                                  const bool is_history_stat)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOptStatManager::get_instance().batch_write(schema_guard,
                                                           tenant_id,
                                                           table_stats,
                                                           column_stats,
                                                           current_time,
                                                           is_index_stat,
                                                           is_history_stat))) {
    LOG_WARN("failed to batch write stats", K(ret));
  //histroy stat is from cache no need free.
  } else if (!is_history_stat) {
    for (int64_t i = 0; OB_SUCC(ret) && i < table_stats.count(); ++i) {
      if (NULL != table_stats.at(i)) {
        table_stats.at(i)->~ObOptTableStat();
        table_stats.at(i) = NULL;
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < column_stats.count(); ++i) {
      if (NULL != column_stats.at(i)) {
        column_stats.at(i)->~ObOptColumnStat();
        column_stats.at(i) = NULL;
      }
    }
  }
  return ret;
}

int ObDbmsStatsUtils::cast_number_to_double(const number::ObNumber &src_val, double &dst_val)
{
  int ret = OB_SUCCESS;
  ObObj src_obj;
  ObObj dest_obj;
  src_obj.set_number(src_val);
  ObArenaAllocator calc_buf(ObModIds::OB_SQL_PARSER);
  ObCastCtx cast_ctx(&calc_buf, NULL, CM_NONE, ObCharset::get_system_collation());
  if (OB_FAIL(ObObjCaster::to_type(ObDoubleType, cast_ctx, src_obj, dest_obj))) {
    LOG_WARN("failed to cast number to double type", K(ret));
  } else if (OB_FAIL(dest_obj.get_double(dst_val))) {
    LOG_WARN("failed to get double", K(ret));
  } else {
    LOG_TRACE("succeed to cast number to double", K(src_val), K(dst_val));
  }
  return ret;
}

// gather statistic related inner table should not read or write during tenant restore or on 
// standby cluster.
int ObDbmsStatsUtils::check_table_read_write_valid(const uint64_t tenant_id, bool &is_valid)
{
  int ret = OB_SUCCESS;
  is_valid = true;
  bool in_restore = false;
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else if (OB_FAIL(GCTX.schema_service_->check_tenant_is_restore(NULL, tenant_id, in_restore))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_UNLIKELY(in_restore) || GCTX.is_standby_cluster()) {
    is_valid = false;
  }
  return ret;
}

bool ObDbmsStatsUtils::is_stat_sys_table(const int64_t table_id)
{
  const uint64_t id = table_id;
  return (is_sys_table(id) || share::is_oracle_mapping_real_virtual_table(id)) &&
         !(is_core_table(table_id) ||
           ObSysTableChecker::is_sys_table_index_tid(table_id) ||
           id == share::OB_ALL_TABLE_STAT_TID ||
           id == share::OB_ALL_COLUMN_STAT_TID ||
           id == share::OB_ALL_HISTOGRAM_STAT_TID ||
           id == share::OB_ALL_DUMMY_TID ||
           id == share::OB_ALL_VIRTUAL_AUTO_INCREMENT_REAL_AGENT_ORA_TID ||
           id == share::OB_ALL_TABLE_STAT_HISTORY_TID ||
           id == share::OB_ALL_COLUMN_STAT_HISTORY_TID ||
           id == share::OB_ALL_HISTOGRAM_STAT_HISTORY_TID ||
           is_sys_lob_table(id));
}

/**
 * @brief ObDbmsStats::parse_granularity
 * @param ctx
 * @param granularity
 * possible values are:
 *  ALL: Gather all (subpartition, partition, and global)
 *  AUTO: Oracle recommends setting granularity to the default value of AUTO to gather subpartition,
 *        partition, or global statistics, depending on partition type. oracle 12c auto is same as
 *        ALL, compatible it.
 *  DEFAULT: Gathers global and partition-level
 *  GLOBAL: Gather global only
 *  GLOBAL AND PARTITION: Gather global and partition-level
 *  APPROX_GLOBAL AND PARTITION: similar to 'GLOBAL AND PARTITION' but in this case the global
                                 statistics are aggregated from partition level statistics.
 *  PARTITION: Gather partition-level
 *  SUBPARTITION: Gather subpartition-level
 *  Oracle granularity actual behavior survey:
 *    https://yuque.antfin-inc.com/docs/share/3eeffde1-7182-4b2a-8f01-e7a3045d4d1e?#
 * @return
 */
int ObDbmsStatsUtils::parse_granularity(const ObString &granularity,
                                        bool &need_global,
                                        bool &need_approx_global,
                                        bool &need_part,
                                        bool &need_subpart)
{
  int ret = OB_SUCCESS;
  // first check the table is partitioned;
  if (0 == granularity.case_compare("all")) {
    need_global = true;
    need_part = true;
    need_subpart = true;
    need_approx_global = false;
  } else if (0 == granularity.case_compare("auto") ||
             0 == granularity.case_compare("Z")) {
    /*do nothing, use default value*/
  } else if (0 == granularity.case_compare("default") ||
             0 == granularity.case_compare("global and partition")) {
    need_global = true;
    need_part = true;
    need_subpart = false;
    need_approx_global = false;
  } else if (0 == granularity.case_compare("approx_global and partition")) {
    need_global = false;
    need_approx_global = true;
    need_part = true;
    need_subpart = false;
  } else if (0 == granularity.case_compare("global")) {
    need_global = true;
    need_part = false;
    need_subpart = false;
    need_approx_global = false;
  } else if (0 == granularity.case_compare("partition")) {
    need_global = false;
    need_part = true;
    need_subpart = false;
    need_approx_global = false;
  } else if (0 == granularity.case_compare("subpartition")) {
    need_global = false;
    need_part = false;
    need_subpart = true;
    need_approx_global = false;
  } else {
    ret = OB_ERR_DBMS_STATS_PL;
    LOG_WARN("Illegal granularity : must be AUTO | ALL | GLOBAL | PARTITION | SUBPARTITION" \
             "| GLOBAL AND PARTITION | APPROX_GLOBAL AND PARTITION", K(ret));
    LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "Illegal granularity : must be AUTO | ALL | GLOBAL |" \
             " PARTITION | SUBPARTITION | GLOBAL AND PARTITION | APPROX_GLOBAL AND PARTITION");
  }
  LOG_TRACE("succeed to parse granularity", K(need_global), K(need_part), K(need_subpart));
  return ret;
}

int ObDbmsStatsUtils::split_batch_write(sql::ObExecContext &ctx,
                                        ObIArray<ObOptTableStat*> &table_stats,
                                        ObIArray<ObOptColumnStat*> &column_stats,
                                        const bool is_index_stat /*default false*/,
                                        const bool is_history_stat /*default false*/)
{
  int ret = OB_SUCCESS;
  int64_t idx_tab_stat = 0;
  int64_t idx_col_stat = 0;
  //avoid the write stat sql is too long, we split write table stats and column stats:
  //  write 2000 tables and 2000 columns every time.
  const int64_t MAX_NUM_OF_WRITE_STATS = 2000;
  int64_t current_time = ObTimeUtility::current_time();
  if (OB_ISNULL(ctx.get_my_session())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(ctx.get_my_session()));
  }
  while (OB_SUCC(ret) &&
        (idx_tab_stat < table_stats.count() || idx_col_stat < column_stats.count())) {
    ObSEArray<ObOptTableStat*, 4> write_table_stats;
    ObSEArray<ObOptColumnStat*, 4> write_column_stats;
    if (OB_UNLIKELY(idx_tab_stat > table_stats.count() || idx_col_stat > column_stats.count())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpectd error", K(ret), K(idx_tab_stat), K(table_stats.count()),
                                      K(idx_col_stat), K(column_stats.count()));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < MAX_NUM_OF_WRITE_STATS && idx_tab_stat < table_stats.count(); ++i) {
        if (OB_FAIL(write_table_stats.push_back(table_stats.at(idx_tab_stat++)))) {
          LOG_WARN("failed to push back", K(ret));
        } else {/*do nothing*/}
      }
      int64_t col_stat_cnt = 0;
      int64_t hist_stat_cnt = 0;
      while (OB_SUCC(ret) &&
             col_stat_cnt < MAX_NUM_OF_WRITE_STATS &&
             hist_stat_cnt < MAX_NUM_OF_WRITE_STATS &&
             idx_col_stat < column_stats.count()) {
        ObOptColumnStat *cur_opt_col_stat = column_stats.at(idx_col_stat);
        if (OB_ISNULL(cur_opt_col_stat)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get unexpected null", K(ret), K(cur_opt_col_stat));
        } else if (OB_FAIL(write_column_stats.push_back(cur_opt_col_stat))) {
          LOG_WARN("failed to push back", K(ret));
        } else {
          ++ col_stat_cnt;
          ++ idx_col_stat;
          if (cur_opt_col_stat->get_histogram().is_valid()) {
            hist_stat_cnt += cur_opt_col_stat->get_histogram().get_bucket_size();
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(ObDbmsStatsUtils::batch_write(ctx.get_virtual_table_ctx().schema_guard_,
                                                ctx.get_my_session()->get_effective_tenant_id(),
                                                write_table_stats,
                                                write_column_stats,
                                                current_time,
                                                is_index_stat,
                                                is_history_stat))) {
        LOG_WARN("failed to batch write stats", K(ret), K(idx_tab_stat), K(idx_col_stat));
      } else {/*do nothing*/}
    }
  }
  return ret;
}

int ObDbmsStatsUtils::batch_write_history_stats(sql::ObExecContext &ctx,
                                                ObIArray<ObOptTableStatHandle> &history_tab_handles,
                                                ObIArray<ObOptColumnStatHandle> &history_col_handles)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObOptTableStat*, 4> table_stats;
  ObSEArray<ObOptColumnStat*, 4> column_stats;
  for (int64_t i = 0; OB_SUCC(ret) && i < history_tab_handles.count(); ++i) {
    if (OB_FAIL(table_stats.push_back(const_cast<ObOptTableStat*>(history_tab_handles.at(i).stat_)))) {
      LOG_WARN("failed to push back", K(ret));
    } else {/*do nothing*/}
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < history_col_handles.count(); ++i) {
    if (OB_FAIL(column_stats.push_back(const_cast<ObOptColumnStat*>(history_col_handles.at(i).stat_)))) {
      LOG_WARN("failed to push back", K(ret));
    } else {/*do nothing*/}
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(split_batch_write(ctx, table_stats, column_stats, false, true))) {
      LOG_WARN("failed to split batch write", K(ret));
    } else {/*do nothing*/}
  }
  return ret;
}

bool ObDbmsStatsUtils::is_subpart_id(const ObIArray<PartInfo> &partition_infos,
                                     const int64_t partition_id,
                                     int64_t &part_id)
{
  bool is_true = false;
  part_id = OB_INVALID_ID;
  for (int64_t i = 0; !is_true && i < partition_infos.count(); ++i) {
    is_true = (partition_infos.at(i).first_part_id_ != OB_INVALID_ID &&
               partition_id == partition_infos.at(i).part_id_);
    if (is_true) {
      part_id = partition_infos.at(i).first_part_id_;
    }
  }
  return is_true;
}

int ObDbmsStatsUtils::get_valid_duration_time(const int64_t start_time,
                                              const int64_t max_duration_time,
                                              int64_t &valid_duration_time)
{
  int ret = OB_SUCCESS;
  const int64_t current_time = ObTimeUtility::current_time();
  if (OB_UNLIKELY(start_time <= 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected error", K(start_time), K(ret));
  } else if (max_duration_time == -1) {
    //do nothing
  } else if (OB_UNLIKELY(current_time - start_time >= max_duration_time)) {
    ret = OB_TIMEOUT;
    LOG_WARN("reach the duration time", K(ret), K(current_time), K(start_time), K(max_duration_time));
  } else {
    valid_duration_time = max_duration_time - (current_time - start_time);
  }
  return ret;
}

int ObDbmsStatsUtils::get_dst_partition_by_tablet_id(sql::ObExecContext &ctx,
                                                     const uint64_t tablet_id,
                                                     const ObIArray<PartInfo> &partition_infos,
                                                     int64_t &partition_id)
{
  int ret = OB_SUCCESS;
  partition_id = -1;
  ObTabletID tmp_tablet_id(tablet_id);
  for (int64_t i = 0; partition_id == -1 && i < partition_infos.count(); ++i) {
    if (partition_infos.at(i).tablet_id_ == tmp_tablet_id) {
      partition_id = partition_infos.at(i).part_id_;
    }
  }
  if (OB_UNLIKELY(partition_id == -1)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(tablet_id), K(partition_infos));
  } else {
    LOG_TRACE("succeed to get dst partition by tablet id", K(tablet_id), K(partition_infos), K(partition_id));
  }
  return ret;
}

}
}
