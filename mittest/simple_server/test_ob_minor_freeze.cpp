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

#include <gtest/gtest.h>
#define USING_LOG_PREFIX STORAGE
#define protected public
#define private public

#include "env/ob_simple_cluster_test_base.h"
#include "lib/mysqlclient/ob_mysql_result.h"
#include "storage/ls/ob_freezer.h"
#include "storage/ls/ob_ls.h"
#include "storage/ls/ob_ls_tablet_service.h"
#include "storage/ls/ob_ls_tx_service.h"
#include "storage/tx_storage/ob_ls_map.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "storage/checkpoint/ob_data_checkpoint.h"
#include "storage/meta_mem/ob_tenant_meta_mem_mgr.h"
#include "storage/meta_mem/ob_tablet_handle.h"
#include "storage/compaction/ob_schedule_dag_func.h"
#include "storage/compaction/ob_tablet_merge_task.h"
#include "storage/ob_storage_table_guard.h"
#include "storage/ob_relative_table.h"
#include "storage/access/ob_rows_info.h"
#include "storage/tx_storage/ob_ls_handle.h"
#include "storage/tx_storage/ob_ls_service.h"

namespace oceanbase
{
namespace storage
{
namespace checkpoint
{
  int64_t TRAVERSAL_FLUSH_INTERVAL = 100 * 1000L;
}
} // namespace storage
namespace unittest
{

using namespace oceanbase::transaction;
using namespace oceanbase::storage;

class TestRunCtx
{
public:
  uint64_t tenant_id_ = 0;
};

TestRunCtx RunCtx;

class ObMinorFreezeTest : public ObSimpleClusterTestBase
{
public:
  ObMinorFreezeTest() : ObSimpleClusterTestBase("test_ob_minor_freeze_") {}
  void get_tablet_id_and_ls_id();
  void get_ls();
  void insert_data(int start);
  void tenant_freeze();
  void logstream_freeze();
  void tablet_freeze();
  void tablet_freeze_for_replace_tablet_meta();
  void insert_and_freeze();
  void empty_memtable_flush();
private:
  int insert_thread_num_ = 3;
  int insert_num_ = 200000;
  int freeze_num_ = 1000;
  int freeze_duration_ = 50 * 1000 * 1000;
  int64_t table_id_ = 0;
  ObTabletID tablet_id_;
  share::ObLSID ls_id_;
  ObLSHandle ls_handle_;
};

void ObMinorFreezeTest::get_tablet_id_and_ls_id()
{
  int ret = OB_SUCCESS;
  common::ObMySQLProxy &sql_proxy = get_curr_simple_server().get_sql_proxy();

  int64_t tmp_table_id = 0;
  int64_t tmp_tablet_id = 0;
  int64_t tmp_ls_id = 0;

  OB_LOG(INFO, "get table_id");
  {
    ObSqlString sql;
    ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("select table_id from oceanbase.__all_virtual_table where table_name = 't1'"));
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ASSERT_EQ(OB_SUCCESS, sql_proxy.read(res, sql.ptr()));
      sqlclient::ObMySQLResult *result = res.get_result();
      ASSERT_NE(nullptr, result);
      ASSERT_EQ(OB_SUCCESS, result->next());
      ASSERT_EQ(OB_SUCCESS, result->get_int("table_id", tmp_table_id));
    }
  }

  OB_LOG(INFO, "get tablet_id");
  {
    ObSqlString sql;
    ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("select tablet_id from oceanbase.__all_virtual_tablet_to_ls where table_id = %ld", tmp_table_id));
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ASSERT_EQ(OB_SUCCESS, sql_proxy.read(res, sql.ptr()));
      sqlclient::ObMySQLResult *result = res.get_result();
      ASSERT_NE(nullptr, result);
      ASSERT_EQ(OB_SUCCESS, result->next());
      ASSERT_EQ(OB_SUCCESS, result->get_int("tablet_id", tmp_tablet_id));
    }
  }

  OB_LOG(INFO, "get ls_id");
  {
    ObSqlString sql;
    ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("select ls_id from oceanbase.__all_virtual_tablet_to_ls where table_id = %ld", tmp_table_id));
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ASSERT_EQ(OB_SUCCESS, sql_proxy.read(res, sql.ptr()));
      sqlclient::ObMySQLResult *result = res.get_result();
      ASSERT_NE(nullptr, result);
      ASSERT_EQ(OB_SUCCESS, result->next());
      ASSERT_EQ(OB_SUCCESS, result->get_int("ls_id", tmp_ls_id));
    }
  }

  table_id_ = tmp_table_id;
  tablet_id_ = tmp_tablet_id;
  ls_id_ = tmp_ls_id;
  OB_LOG(INFO, "tmp_table_id", K(tmp_table_id));
  OB_LOG(INFO, "tmp_tablet_id", K(tmp_tablet_id));
  OB_LOG(INFO, "tmp_ls_id", K(tmp_ls_id));
  OB_LOG(INFO, "tablet_id", K(table_id_));
  OB_LOG(INFO, "ls_id", K(ls_id_));
  ASSERT_EQ(true, tablet_id_.is_valid());
  ASSERT_EQ(true, ls_id_.is_valid());
}

void ObMinorFreezeTest::get_ls()
{
  OB_LOG(INFO, "get_ls");
  share::ObTenantSwitchGuard tenant_guard;
  OB_LOG(INFO, "tenant_id", K(RunCtx.tenant_id_));
  ASSERT_EQ(OB_SUCCESS, tenant_guard.switch_to(RunCtx.tenant_id_));
  ObLSService *ls_srv = MTL(ObLSService *);
  ASSERT_NE(nullptr, ls_srv);
  OB_LOG(INFO, "ls_id", K(ls_id_));
  ASSERT_EQ(OB_SUCCESS, ls_srv->get_ls(ls_id_, ls_handle_, ObLSGetMod::STORAGE_MOD));
  ASSERT_EQ(true, ls_handle_.is_valid());
}

void ObMinorFreezeTest::insert_data(int start)
{
  int ret = OB_SUCCESS;
  common::ObMySQLProxy &sql_proxy = get_curr_simple_server().get_sql_proxy2();
  const int64_t start_time = ObTimeUtility::current_time();

  OB_LOG(INFO, "insert data start");
  int i = 0;
  while (ObTimeUtility::current_time() - start_time <= freeze_duration_) {
    ObSqlString sql;
    ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("insert into t1 values(%d, %d)", i + start, i + start));
    int64_t affected_rows = 0;
    if (OB_FAIL(sql_proxy.write(sql.ptr(), affected_rows))) {
      ret = OB_SUCCESS;
    }
    ASSERT_EQ(OB_SUCCESS, ret);
    ++i;
  }
}

void ObMinorFreezeTest::tenant_freeze()
{
  common::ObMySQLProxy &sql_proxy = get_curr_simple_server().get_sql_proxy2();
  const int64_t start = ObTimeUtility::current_time();

  OB_LOG(INFO, "test tenant_freeze");
  while (ObTimeUtility::current_time() - start <= freeze_duration_) {
    ObSqlString sql;
    int64_t affected_rows = 0;

    ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("alter system minor freeze tenant tt1"));
    ASSERT_EQ(OB_SUCCESS, sql_proxy.write(sql.ptr(), affected_rows));
  }
}

void ObMinorFreezeTest::logstream_freeze()
{
  common::ObMySQLProxy &sql_proxy = get_curr_simple_server().get_sql_proxy2();
  share::ObTenantSwitchGuard tenant_guard;
  OB_LOG(INFO, "tenant_id", K(RunCtx.tenant_id_));
  ASSERT_EQ(OB_SUCCESS, tenant_guard.switch_to(RunCtx.tenant_id_));
  int64_t i = 0;

  const int64_t start = ObTimeUtility::current_time();
  while (ObTimeUtility::current_time() - start <= freeze_duration_) {
    ASSERT_EQ(OB_SUCCESS, ls_handle_.get_ls()->logstream_freeze((i % 2 == 0) ? true : false));
    i = i + 1;
  }
}

void ObMinorFreezeTest::tablet_freeze()
{
  common::ObMySQLProxy &sql_proxy = get_curr_simple_server().get_sql_proxy2();
  share::ObTenantSwitchGuard tenant_guard;
  OB_LOG(INFO, "tenant_id", K(RunCtx.tenant_id_));
  ASSERT_EQ(OB_SUCCESS, tenant_guard.switch_to(RunCtx.tenant_id_));
  int64_t i = 0;

  const int64_t start = ObTimeUtility::current_time();
  while (ObTimeUtility::current_time() - start <= freeze_duration_) {
    ASSERT_EQ(OB_SUCCESS, ls_handle_.get_ls()->tablet_freeze(tablet_id_, (i % 2 == 0) ? true : false));
    i = i + 1;
  }
}

void ObMinorFreezeTest::tablet_freeze_for_replace_tablet_meta()
{
  common::ObMySQLProxy &sql_proxy = get_curr_simple_server().get_sql_proxy2();
  share::ObTenantSwitchGuard tenant_guard;
  OB_LOG(INFO, "tenant_id", K(RunCtx.tenant_id_));
  ASSERT_EQ(OB_SUCCESS, tenant_guard.switch_to(RunCtx.tenant_id_));
  const int64_t start = ObTimeUtility::current_time();
  while (ObTimeUtility::current_time() - start <= freeze_duration_) {
    ObTableHandleV2 handle;
    int ret = ls_handle_.get_ls()->get_freezer()->tablet_freeze_for_replace_tablet_meta(tablet_id_, handle);
    if (OB_EAGAIN == ret || OB_ENTRY_EXIST == ret) {
      ret = OB_SUCCESS;
    }
    ASSERT_EQ(OB_SUCCESS, ret);
    if (OB_SUCC(ret)) {
      ASSERT_EQ(OB_SUCCESS, ls_handle_.get_ls()->get_freezer()->handle_frozen_memtable_for_replace_tablet_meta(tablet_id_, handle));
    }
  }
}

void ObMinorFreezeTest::insert_and_freeze()
{
  std::thread tenant_freeze_thread([this]() { tenant_freeze(); });
  std::thread tablet_freeze_thread([this]() { tablet_freeze(); });
  std::thread tablet_freeze_for_replace_tablet_meta_thread([this]() { tablet_freeze_for_replace_tablet_meta(); });
  std::vector<std::thread> insert_threads;
  for (int i = 0; i < insert_thread_num_; ++i) {
    int start = i * insert_num_;
    insert_threads.push_back(std::thread([this, start]() { insert_data(start); }));
  }

  tenant_freeze_thread.join();
  tablet_freeze_thread.join();
  tablet_freeze_for_replace_tablet_meta_thread.join();
  for (int i = 0; i < insert_thread_num_; ++i) {
    insert_threads[i].join();
  }
}

void ObMinorFreezeTest::empty_memtable_flush()
{
  SERVER_LOG(INFO, "start empty_memtable_flush");
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  common::ObMySQLProxy &sql_proxy = get_curr_simple_server().get_sql_proxy2();

  ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("alter system minor freeze tenant tt1"));
  ASSERT_EQ(OB_SUCCESS, sql_proxy.write(sql.ptr(), affected_rows));
  usleep(100 * 1000); //100ms

  ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("set autocommit=0"));

  const int64_t start_time = ObTimeUtility::current_time();

  OB_LOG(INFO, "empty memtable flush start");
  int i = 0;
  while (ObTimeUtility::current_time() - start_time <= freeze_duration_) {
    if (i % 2 == 0) {
      ASSERT_EQ(OB_SUCCESS, sql_proxy.write(sql.ptr(), affected_rows));
      ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("begin"));
      ASSERT_EQ(OB_SUCCESS, sql_proxy.write(sql.ptr(), affected_rows));
      ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("insert into t1 values(%d, %d)", i, i));
      if (OB_FAIL(sql_proxy.write(sql.ptr(), affected_rows))) {
        ret = OB_SUCCESS;
      }
      ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("rollback"));
      ASSERT_EQ(OB_SUCCESS, sql_proxy.write(sql.ptr(), affected_rows));
      ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("alter system minor freeze tenant tt1"));
      ASSERT_EQ(OB_SUCCESS, sql_proxy.write(sql.ptr(), affected_rows));
    } else {
      ASSERT_EQ(OB_SUCCESS, sql_proxy.write(sql.ptr(), affected_rows));
      ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("begin"));
      ASSERT_EQ(OB_SUCCESS, sql_proxy.write(sql.ptr(), affected_rows));
      ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("insert into t1 values(%d, %d)", i, i));
      if (OB_FAIL(sql_proxy.write(sql.ptr(), affected_rows))) {
        ret = OB_SUCCESS;
      }
      ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("commit"));
      ASSERT_EQ(OB_SUCCESS, sql_proxy.write(sql.ptr(), affected_rows));
      ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("alter system minor freeze tenant tt1"));
      ASSERT_EQ(OB_SUCCESS, sql_proxy.write(sql.ptr(), affected_rows));
    }
    ++i;
  }
}

TEST_F(ObMinorFreezeTest, observer_start)
{
  SERVER_LOG(INFO, "observer_start succ");
}

TEST_F(ObMinorFreezeTest, add_tenant)
{
  // 创建普通租户tt1
  ASSERT_EQ(OB_SUCCESS, create_tenant("tt1"));
  // 获取租户tt1的tenant_id
  ASSERT_EQ(OB_SUCCESS, get_tenant_id(RunCtx.tenant_id_));
  ASSERT_NE(0, RunCtx.tenant_id_);
  // 初始化普通租户tt1的sql proxy
  ASSERT_EQ(OB_SUCCESS, get_curr_simple_server().init_sql_proxy2());
}

TEST_F(ObMinorFreezeTest, create_table)
{
  common::ObMySQLProxy &sql_proxy = get_curr_simple_server().get_sql_proxy2();

  {
    OB_LOG(INFO, "create_table start");
    ObSqlString sql;
    sql.assign_fmt("create table if not exists t1 (c1 int, c2 int, primary key(c1))");
    int64_t affected_rows = 0;
    ASSERT_EQ(OB_SUCCESS, sql_proxy.write(sql.ptr(), affected_rows));
    OB_LOG(INFO, "create_table succ");
  }
}

TEST_F(ObMinorFreezeTest, insert_and_freeze)
{
  get_tablet_id_and_ls_id();
  get_ls();
  insert_and_freeze();
  empty_memtable_flush();
}

} // end unittest
} // end oceanbase

int main(int argc, char **argv)
{
  oceanbase::unittest::init_log_and_gtest(argc, argv);
  OB_LOGGER.set_log_level("INFO");
  GCONF._enable_defensive_check = false;
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
