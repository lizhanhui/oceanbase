// Copyright (c) 2019-2021 Alibaba Inc. All Rights Reserved.
// Author:
//     lixia.yq@antfin.com
//

#include <gtest/gtest.h>

#define private public
#define protected public

#include "storage/compaction/ob_partition_merge_policy.h"
#include "storage/ob_storage_struct.h"
#include "storage/blocksstable/ob_sstable.h"
#include "share/rc/ob_tenant_base.h"
#include "storage/meta_mem/ob_tenant_meta_mem_mgr.h"

namespace oceanbase
{
using namespace common;
using namespace storage;
using namespace blocksstable;
using namespace compaction;
using namespace omt;
using namespace share;

namespace unittest
{

class TestParallelMinorDag : public ::testing::Test
{
public:
  TestParallelMinorDag() : allocator_(ObModIds::TEST), tenant_base_(500) {}
  virtual ~TestParallelMinorDag() {}
  int prepare_merge_result(const int64_t sstable_cnt, ObGetMergeTablesResult &result);

  void SetUp()
  {
    ObTenantMetaMemMgr *t3m = OB_NEW(ObTenantMetaMemMgr, ObModIds::TEST, 500);
    tenant_base_.set(t3m);

    ObTenantEnv::set_tenant(&tenant_base_);
    ASSERT_EQ(OB_SUCCESS, tenant_base_.init());

    ASSERT_EQ(OB_SUCCESS, t3m->init());
  }

  share::SCN get_start_log_ts(const int64_t idx);
  share::SCN get_end_log_ts(const int64_t idx);
  void check_result(const int64_t sstable_cnt, const int64_t result_cnt);

  static const int64_t TENANT_ID = 1;
  static const int64_t TABLE_ID = 7777;
  static const int64_t TEST_ROWKEY_COLUMN_CNT = 3;
  static const int64_t TEST_COLUMN_CNT = 6;
  static const int64_t MAX_SSTABLE_CNT = 60;

  common::ObArenaAllocator allocator_;
  ObTenantBase tenant_base_;
  ObSSTable fake_sstables_[MAX_SSTABLE_CNT];
};

int TestParallelMinorDag::prepare_merge_result(
    const int64_t sstable_cnt,
    ObGetMergeTablesResult &result)
{
  int ret = OB_SUCCESS;
  result.reset();

  result.version_range_.base_version_ = 50;
  result.version_range_.snapshot_version_ = 100;
  result.version_range_.multi_version_start_ = 100;
  result.merge_version_ = 0;
  result.base_schema_version_ = 0;
  result.schema_version_ = 0;
  result.create_snapshot_version_ = 0;
  result.suggest_merge_type_ = MINOR_MERGE;

  int64_t log_ts = 1;
  for (int i = 0; OB_SUCC(ret) && i < sstable_cnt; ++i) {
    fake_sstables_[i].key_.scn_range_.start_scn_.convert_for_tx(log_ts++);
    fake_sstables_[i].key_.scn_range_.end_scn_.convert_for_tx(log_ts);
    if (OB_FAIL(result.handle_.add_table(&fake_sstables_[i]))) {
      COMMON_LOG(WARN, "failed to push table", K(ret), K(i), K(fake_sstables_[i]));
    }
  }
  result.scn_range_.start_scn_ = fake_sstables_[0].key_.scn_range_.start_scn_;
  result.scn_range_.end_scn_ = fake_sstables_[sstable_cnt - 1].key_.scn_range_.end_scn_;
  return ret;
}

share::SCN TestParallelMinorDag::get_start_log_ts(const int64_t idx)
{
  return fake_sstables_[idx].key_.scn_range_.start_scn_;
}

share::SCN TestParallelMinorDag::get_end_log_ts(const int64_t idx)
{
  return fake_sstables_[idx].key_.scn_range_.end_scn_;
}

void check_result_valid(const ObGetMergeTablesResult &result)
{
  ASSERT_EQ(result.handle_.get_table(0)->get_start_scn(), result.scn_range_.start_scn_);
  ASSERT_EQ(result.handle_.get_table(result.handle_.get_count() - 1)->get_end_scn(), result.scn_range_.end_scn_);
}

void TestParallelMinorDag::check_result(const int64_t sstable_cnt, const int64_t result_cnt)
{
  ObGetMergeTablesResult result;
  ObArray<ObGetMergeTablesResult> result_array;
  ObMinorExecuteRangeMgr minor_range_mgr;

  ASSERT_EQ(OB_SUCCESS, prepare_merge_result(sstable_cnt, result));
  ASSERT_EQ(OB_SUCCESS, ObPartitionMergePolicy::generate_parallel_minor_interval(result, minor_range_mgr, result_array));

  COMMON_LOG(INFO, "generate_parallel_minor_interval", K(sstable_cnt), K(result_array));
  ASSERT_EQ(result_array.count(), result_cnt);
  int idx = 0;
  int rest_cnt = sstable_cnt;
  const int64_t minor_trigger = ObPartitionMergePolicy::OB_MINOR_PARALLEL_SSTABLE_CNT_IN_DAG / 2;
  if (sstable_cnt < ObPartitionMergePolicy::OB_MINOR_PARALLEL_SSTABLE_CNT_TRIGGER) {
    ASSERT_EQ(result_array.count(), 1);
    ASSERT_EQ(result_array.at(0).handle_.get_count(), sstable_cnt);
  } else {
    for (int i = 0; i < result_array.count(); ++i) {
      check_result_valid(result_array.at(i));

      ASSERT_EQ(result_array.at(i).scn_range_.start_scn_, get_start_log_ts(idx));
      if (rest_cnt > ObPartitionMergePolicy::OB_MINOR_PARALLEL_SSTABLE_CNT_IN_DAG + minor_trigger
          && sstable_cnt >= minor_trigger) {
        ASSERT_EQ(result_array.at(i).handle_.get_count(), ObPartitionMergePolicy::OB_MINOR_PARALLEL_SSTABLE_CNT_IN_DAG);
        idx += ObPartitionMergePolicy::OB_MINOR_PARALLEL_SSTABLE_CNT_IN_DAG;
        rest_cnt -= ObPartitionMergePolicy::OB_MINOR_PARALLEL_SSTABLE_CNT_IN_DAG;
      } else {
        ASSERT_EQ(result_array.at(i).handle_.get_count(), rest_cnt);
        idx = sstable_cnt;
      }
      ASSERT_EQ(result_array.at(i).scn_range_.end_scn_, get_end_log_ts(idx - 1));
    }
  }
}

TEST_F(TestParallelMinorDag, test_parallel_interval)
{
  check_result(20, 2);
  check_result(19, 1);
  check_result(36, 4);
  check_result(35, 3);
  check_result(32, 3);
  check_result(12, 1);
  check_result(18, 1);
  check_result(22, 2);
  check_result(3, 1);
  check_result(9, 1);
  check_result(40, 4);
}

#define CHECK_IN_RANGE(start_log_ts, end_log_ts, flag) \
    fake_sstables_[0].key_.scn_range_.start_scn_.convert_for_tx(start_log_ts); \
    fake_sstables_[0].key_.scn_range_.end_scn_.convert_for_tx(end_log_ts); \
    ASSERT_EQ(flag, range_mgr.in_execute_range(&fake_sstables_[0]));

ObScnRange construct_scn_range(const int64_t start_scn, const int64_t end_scn)
{
  ObScnRange ret_range;
  ret_range.start_scn_.convert_for_tx(start_scn);
  ret_range.end_scn_.convert_for_tx(end_scn);
  return ret_range;
}

TEST_F(TestParallelMinorDag, test_range_mgr)
{
  ObMinorExecuteRangeMgr range_mgr;

  range_mgr.exe_range_array_.push_back(construct_scn_range(60, 80));
  range_mgr.exe_range_array_.push_back(construct_scn_range(50, 70));
  ASSERT_EQ(OB_ERR_UNEXPECTED, range_mgr.sort_ranges());

  range_mgr.reset();
  range_mgr.exe_range_array_.push_back(construct_scn_range(60, 80));
  range_mgr.exe_range_array_.push_back(construct_scn_range(10, 20));
  range_mgr.exe_range_array_.push_back(construct_scn_range(30, 50));
  ASSERT_EQ(OB_SUCCESS, range_mgr.sort_ranges());
  COMMON_LOG(INFO, "success to sort ranges", K(range_mgr.exe_range_array_));

  CHECK_IN_RANGE(18, 19, true);
  CHECK_IN_RANGE(60, 70, true);
  CHECK_IN_RANGE(22, 30, false);
  CHECK_IN_RANGE(30, 50, true);

  range_mgr.reset();
  range_mgr.exe_range_array_.push_back(construct_scn_range(10, 20));
  range_mgr.exe_range_array_.push_back(construct_scn_range(40, 80));
  range_mgr.exe_range_array_.push_back(construct_scn_range(20, 40));
  ASSERT_EQ(OB_SUCCESS, range_mgr.sort_ranges());
  COMMON_LOG(INFO, "success to sort ranges", K(range_mgr.exe_range_array_));

  CHECK_IN_RANGE(18, 19, true);
  CHECK_IN_RANGE(60, 70, true);
  CHECK_IN_RANGE(22, 30, true);
  CHECK_IN_RANGE(30, 50, true);
  CHECK_IN_RANGE(80, 85, false);
  CHECK_IN_RANGE(30, 65, true);

  range_mgr.reset();
  range_mgr.exe_range_array_.push_back(construct_scn_range(0, 200));
  range_mgr.exe_range_array_.push_back(construct_scn_range(10, 20));
  range_mgr.exe_range_array_.push_back(construct_scn_range(40, 80));
  range_mgr.exe_range_array_.push_back(construct_scn_range(20, 40));
  ASSERT_EQ(OB_SUCCESS, range_mgr.sort_ranges());

  CHECK_IN_RANGE(100, 165, true);
}

TEST_F(TestParallelMinorDag, test_parallel_with_range_mgr)
{
  int64_t sstable_cnt = 40;
  ObGetMergeTablesResult result;
  ObArray<ObGetMergeTablesResult> result_array;
  ObMinorExecuteRangeMgr minor_range_mgr;

  minor_range_mgr.exe_range_array_.push_back(construct_scn_range(11, 21));
  minor_range_mgr.exe_range_array_.push_back(construct_scn_range(31, 41));

  ASSERT_EQ(OB_SUCCESS, prepare_merge_result(sstable_cnt, result));
  ASSERT_EQ(OB_SUCCESS, ObPartitionMergePolicy::generate_parallel_minor_interval(result, minor_range_mgr, result_array));
  ASSERT_EQ(result_array.count(), 2);

  ASSERT_EQ(result_array.at(0).scn_range_.start_scn_.get_val_for_tx(), 1);
  ASSERT_EQ(result_array.at(0).scn_range_.end_scn_.get_val_for_tx(), 11);

  ASSERT_EQ(result_array.at(1).scn_range_.start_scn_.get_val_for_tx(), 21);
  ASSERT_EQ(result_array.at(1).scn_range_.end_scn_.get_val_for_tx(), 31);


  result_array.reset();
  minor_range_mgr.reset();
  minor_range_mgr.exe_range_array_.push_back(construct_scn_range(15, 19));
  minor_range_mgr.exe_range_array_.push_back(construct_scn_range(37, 39));

  ASSERT_EQ(OB_SUCCESS, ObPartitionMergePolicy::generate_parallel_minor_interval(result, minor_range_mgr, result_array));
  COMMON_LOG(INFO, "generate_parallel_minor_interval", K(result_array));
  ASSERT_EQ(result_array.count(), 2);

  ASSERT_EQ(result_array.at(0).scn_range_.start_scn_.get_val_for_tx(), 1);
  ASSERT_EQ(result_array.at(0).scn_range_.end_scn_.get_val_for_tx(), 15);

  ASSERT_EQ(result_array.at(1).scn_range_.start_scn_.get_val_for_tx(), 19);
  ASSERT_EQ(result_array.at(1).scn_range_.end_scn_.get_val_for_tx(), 37);

  // two runing ranges, candidates need > 8
  result_array.reset();
  minor_range_mgr.reset();
  minor_range_mgr.exe_range_array_.push_back(construct_scn_range(1, 17));
  minor_range_mgr.exe_range_array_.push_back(construct_scn_range(18, 34));
  ASSERT_EQ(OB_SUCCESS, ObPartitionMergePolicy::generate_parallel_minor_interval(result, minor_range_mgr, result_array));
  COMMON_LOG(INFO, "generate_parallel_minor_interval", K(result_array));
  ASSERT_EQ(result_array.count(), 0);

  result_array.reset();
  minor_range_mgr.reset();
  minor_range_mgr.exe_range_array_.push_back(construct_scn_range(1, 17));
  minor_range_mgr.exe_range_array_.push_back(construct_scn_range(17, 31));
  ASSERT_EQ(OB_SUCCESS, ObPartitionMergePolicy::generate_parallel_minor_interval(result, minor_range_mgr, result_array));
  COMMON_LOG(INFO, "generate_parallel_minor_interval", K(result_array));
  ASSERT_EQ(result_array.count(), 1);
  ASSERT_EQ(result_array.at(0).scn_range_.start_scn_.get_val_for_tx(), 31);
  ASSERT_EQ(result_array.at(0).scn_range_.end_scn_.get_val_for_tx(), 41);

  // one runing ranges, candidates need > 4
  result_array.reset();
  minor_range_mgr.reset();
  minor_range_mgr.exe_range_array_.push_back(construct_scn_range(1, 34));
  ASSERT_EQ(OB_SUCCESS, ObPartitionMergePolicy::generate_parallel_minor_interval(result, minor_range_mgr, result_array));
  COMMON_LOG(INFO, "generate_parallel_minor_interval", K(result_array));
  ASSERT_EQ(result_array.count(), 1);
  ASSERT_EQ(result_array.at(0).scn_range_.start_scn_.get_val_for_tx(), 34);
  ASSERT_EQ(result_array.at(0).scn_range_.end_scn_.get_val_for_tx(), 41);
}

} // namespace unittest
} // namespace oceanbase

int main(int argc, char **argv)
{
  system("rm -rf test_parallel_minor_dag.log*");
  OB_LOGGER.set_file_name("test_parallel_minor_dag.log");
  OB_LOGGER.set_log_level("INFO");
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
