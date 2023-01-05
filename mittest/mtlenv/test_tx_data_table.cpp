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

#define protected public
#define private public

#include <iostream>
#include <thread>
#include "lib/oblog/ob_log.h"
#include "storage/ls/ob_freezer.h"
#include "storage/ls/ob_ls.h"
#include "storage/ls/ob_ls_tablet_service.h"
#include "storage/ls/ob_ls_tx_service.h"
#include "storage/tx_table/ob_tx_data_memtable_mgr.h"
#include "storage/tx_table/ob_tx_data_table.h"
#include "storage/tx_table/ob_tx_table_iterator.h"
#include "storage/checkpoint/ob_data_checkpoint.h"
#include "storage/meta_mem/ob_tenant_meta_mem_mgr.h"
#include "mtlenv/mock_tenant_module_env.h"

#undef private
#undef protected

static int64_t const_data_num;
int64_t tx_data_num CACHE_ALIGNED = 0;
int64_t inserted_cnt = 0;
share::SCN insert_start_scn = share::SCN::min_scn();
const int64_t ONE_SEC_NS = 1000LL * 1000LL * 1000LL;
const int64_t MOD_NS = 1000LL * ONE_SEC_NS;

namespace oceanbase
{
using namespace share;
using namespace palf;

namespace storage
{
class TestTxDataTable;
class MockTxDataTable;
class MockTxTable;
static const uint64_t TEST_TENANT_ID = 1;

// shrink select interval to push more points in cur_commit_scns
// then the code will merge commit versions array with step_len larger than 1
int64_t ObTxDataMemtableScanIterator::PERIODICAL_SELECT_INTERVAL_NS = 10LL;

class MockTxDataMemtableMgr : public ObTxDataMemtableMgr
{
public:
  int init(const common::ObTabletID &tablet_id,
           ObTxDataTable *tx_data_table,
           ObFreezer *freezer,
           ObTenantMetaMemMgr *t3m)
  {
    int ret = OB_SUCCESS;
    ObLSHandle ls_handle;
    ObTxTableGuard tx_table_guard;
    if (IS_INIT) {
      ret = OB_INIT_TWICE;
      STORAGE_LOG(WARN, "ObTxDataMemtableMgr has been initialized.", KR(ret));
    } else if (OB_UNLIKELY(!tablet_id.is_valid()) || OB_ISNULL(freezer) || OB_ISNULL(t3m)
               || OB_ISNULL(tx_data_table)) {
      ret = OB_INVALID_ARGUMENT;
      STORAGE_LOG(WARN, "invalid arguments", K(ret), K(tablet_id), KP(freezer), KP(t3m));
    } else {
      tablet_id_ = tablet_id;
      t3m_ = t3m;
      table_type_ = ObITable::TableType::TX_DATA_MEMTABLE;
      freezer_ = freezer;
      tx_data_table_ = tx_data_table;
      ls_tablet_svr_ = ls_handle.get_ls()->get_tablet_svr();
      if (OB_ISNULL(tx_data_table_) || OB_ISNULL(ls_tablet_svr_)) {
        ret = OB_ERR_NULL_VALUE;
        STORAGE_LOG(WARN, "Init tx data memtable mgr failed.", KR(ret));
      } else {
        is_inited_ = true;
      }
    }

    if (IS_NOT_INIT) {
      destroy();
    }
    return ret;
  }

};

class MockTxDataTable : public ObTxDataTable
{
  friend TestTxDataTable;

public:
  MockTxDataTable() : ObTxDataTable() {}

  int init(ObLS &ls)
  {
    int ret = OB_SUCCESS;
    ObTabletID tablet_id(LS_TX_DATA_TABLET);
    ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr *);
    OB_ASSERT(OB_SUCCESS == ObTxDataTable::init(&ls, &tx_ctx_table_));
    OB_ASSERT(OB_SUCCESS == mgr_.init(tablet_id, this, &freezer_, t3m));
    mgr_.set_slice_allocator(get_slice_allocator());

    return ret;
  }

private:
  virtual ObTxDataMemtableMgr *get_memtable_mgr_()
  {
    return &mgr_;
  }

private:
  ObFreezer freezer_;
  ObTabletHandle th_;
  MockTxDataMemtableMgr mgr_;
  ObLSTabletService ls_tablet_svr_;
  ObTxCtxTable tx_ctx_table_;
  // ObOccamTimer occam_timer_;
  // ObOccamThreadPool pool_;
};

class MockTxTable : public ObTxTable
{
public:
  MockTxTable(MockTxDataTable *tx_data_table) : ObTxTable(*tx_data_table) {}
};

class TestTxDataTable : public ::testing::Test
{
public:
  TestTxDataTable() : tx_table_(&tx_data_table_) {}
  virtual ~TestTxDataTable() {}

  static void SetUpTestCase()
  {
    EXPECT_EQ(OB_SUCCESS, MockTenantModuleEnv::get_instance().init());
  }

  virtual void SetUp() override
  {
    fake_ls_(ls_);
  }

  static void TearDownTestCase()
  {
    MockTenantModuleEnv::get_instance().destroy();
  }

  void do_basic_test();

  void do_undo_status_test();

  void do_tx_data_serialize_test();

  void do_repeat_insert_test();

  void do_multiple_init_iterator_test();

  void do_print_leak_slice_test();


private:
  void insert_tx_data_();
  void insert_abort_tx_data_();

  void make_freezing_to_frozen_(ObTxDataMemtableMgr *memtable_mgr);

  void test_serialize_with_action_cnt_(int cnt);

  void generate_past_commit_scn_(ObCommitSCNsArray &past_commit_scns);

  void set_freezer_();

  void init_memtable_mgr_(ObTxDataMemtableMgr *memtable_mgr);

  void check_freeze_(ObTxDataMemtableMgr *memtable_mgr,
                     ObTxDataMemtable *&freezing_memtable,
                     ObTxDataMemtable *&active_memtable);

  void check_commit_scn_row_(ObTxDataMemtableScanIterator &iter, ObTxDataMemtable *freezing_memtable);

  void make_freezing_to_frozen(ObTxDataMemtableMgr *memtable_mgr);

  void test_serialize_with_action_cnt(int cnt);

  void insert_rollback_tx_data_();

  void test_commit_scns_serialize_();

  void fake_ls_(ObLS &ls);

public:
  MockTxDataTable tx_data_table_;
  MockTxTable tx_table_;
  ObTenantMetaMemMgr *t3m_;
  ObLS ls_;
  ObArenaAllocator allocator_;
};

void TestTxDataTable::make_freezing_to_frozen_(ObTxDataMemtableMgr *memtable_mgr)
{
  ObArray<ObTableHandleV2> memtables;
  ObTxDataMemtable *memtable = nullptr;
  memtable_mgr->get_all_memtables(memtables);
  for (int i = 0; i < memtables.count(); i++) {
    ASSERT_EQ(OB_SUCCESS, memtables[i].get_tx_data_memtable(memtable));
    if (ObTxDataMemtable::State::FREEZING == memtable->get_state()) {
      memtable->set_state(ObTxDataMemtable::State::FROZEN);
    }
  }
}

void TestTxDataTable::insert_tx_data_()
{
  insert_start_scn.convert_for_logservice(ObTimeUtil::current_time_ns());
  ObTxData *tx_data = nullptr;
  transaction::ObTransID tx_id;

  while (true) {
    int64_t int_tx_id = 0;
    if ((int_tx_id = ATOMIC_AAF(&tx_data_num, -1)) < 0) { break; }
    tx_id = int_tx_id;
    bool is_abort_tx = int_tx_id % 5 == 0;

    tx_data = nullptr;
    ASSERT_EQ(OB_SUCCESS, tx_data_table_.alloc_tx_data(tx_data));

    // fill in data
    tx_data->tx_id_ = tx_id;
    tx_data->start_scn_ = share::SCN::plus(insert_start_scn, (rand64(ObTimeUtil::current_time_ns()) % MOD_NS));
    tx_data->commit_version_ = is_abort_tx ? share::SCN::invalid_scn() : share::SCN::plus(tx_data->start_scn_, (rand64(ObTimeUtil::current_time()) % MOD_NS));
    tx_data->end_scn_ = is_abort_tx ? tx_data->start_scn_ : tx_data->commit_version_;
    tx_data->state_ = is_abort_tx ? ObTxData::ABORT : ObTxData::COMMIT;
    int undo_act_num = is_abort_tx ? 0 : rand() & 15;
    for (int j = 0; j < undo_act_num; j++) {
      auto from = random();
      auto to = (from - 10000 > 0) ? (from - 100000) : 1;
      transaction::ObUndoAction undo_action(from, to);
      ASSERT_EQ(OB_SUCCESS, tx_data->add_undo_action(&tx_table_, undo_action));
    }

    ASSERT_EQ(OB_SUCCESS, tx_data_table_.insert(tx_data));
    ASSERT_EQ(nullptr, tx_data);
  }
}

void TestTxDataTable::insert_rollback_tx_data_()
{
  auto tx_id = transaction::ObTransID(INT64_MAX-2);
  share::SCN max_end_scn = share::SCN::min_scn();
  ObTableHandleV2 handle;
  ObTxDataMemtable *memtable;
  ASSERT_EQ(OB_SUCCESS, tx_data_table_.get_memtable_mgr_()->get_active_memtable(handle));
  ASSERT_EQ(OB_SUCCESS, handle.get_tx_data_memtable(memtable));
  ASSERT_NE(nullptr, memtable);

  for (int i = 0; i < 200; i++) {
    ObTxData *tx_data = nullptr;
    ASSERT_EQ(OB_SUCCESS, tx_data_table_.alloc_tx_data(tx_data));

    // fill in data
    tx_data->tx_id_ = tx_id;
    tx_data->start_scn_ = i % 2 ? insert_start_scn : share::SCN::max_scn();
    tx_data->commit_version_ = share::SCN::invalid_scn();
    tx_data->end_scn_ = share::SCN::plus(insert_start_scn, (rand64(ObTimeUtil::current_time_ns()) % (100LL * ONE_SEC_NS)));
    if (tx_data->end_scn_ > max_end_scn) {
      max_end_scn = tx_data->end_scn_;
    }
    transaction::ObUndoAction undo_action(100, 10);
    tx_data->add_undo_action(&tx_table_, undo_action);
    tx_data->state_ = ObTxData::RUNNING;

    ASSERT_EQ(OB_SUCCESS, tx_data_table_.insert(tx_data));
    ASSERT_EQ(nullptr, tx_data);
    {
      ObTxDataGuard guard;
      memtable->get_tx_data(tx_id, guard);
      ASSERT_EQ(max_end_scn, guard.tx_data().end_scn_);
    }
  }
}

void TestTxDataTable::insert_abort_tx_data_()
{
  insert_start_scn.convert_for_logservice(ObTimeUtil::current_time_ns());
  ObTxData *tx_data = nullptr;
  transaction::ObTransID tx_id;

  tx_id = INT64_MAX - 3;

  tx_data = nullptr;
  ASSERT_EQ(OB_SUCCESS, tx_data_table_.alloc_tx_data(tx_data));

  // fill in data
  tx_data->tx_id_ = tx_id;
  tx_data->start_scn_ = share::SCN::plus(insert_start_scn, MOD_NS + 1); // bigger than all tx datas to be the last one
  tx_data->commit_version_ = share::SCN::invalid_scn();
  tx_data->end_scn_ = tx_data->start_scn_;
  tx_data->state_ = ObTxData::ABORT;

  ASSERT_EQ(OB_SUCCESS, tx_data_table_.insert(tx_data));
  ASSERT_EQ(nullptr, tx_data);
}

void TestTxDataTable::generate_past_commit_scn_(ObCommitSCNsArray &past_commit_scns)
{
  share::SCN start_scn = share::SCN::minus(insert_start_scn, 300LL * ONE_SEC_NS);
  share::SCN commit_version = share::SCN::plus(start_scn, 2LL * ONE_SEC_NS);
  for (int i = 0; i < 500; i++) {
    past_commit_scns.array_.push_back(ObCommitSCNsArray::Node(start_scn, commit_version));
    start_scn = share::SCN::plus(start_scn, 1LL * ONE_SEC_NS + (rand64(ObTimeUtil::current_time_ns()) % ONE_SEC_NS));
    commit_version = share::SCN::plus(std::max(commit_version, start_scn), (rand64(ObTimeUtil::current_time_ns()) % (2LL * ONE_SEC_NS)));
  }
}

void TestTxDataTable::set_freezer_()
{
  ObLSID ls_id(1);
  ObLSWRSHandler ls_loop_worker;
  ObLSTxService ls_tx_svr(&ls_);
  ObLSTabletService *ls_tablet_svr = ls_.get_tablet_svr();
  observer::ObIMetaReport *fake_reporter = (observer::ObIMetaReport *)0xff;
  ASSERT_EQ(OB_SUCCESS, ls_tablet_svr->init(&ls_, fake_reporter));
  ls_.data_checkpoint_.is_inited_ = true;

  tx_data_table_.freezer_.init(&ls_);
}

void TestTxDataTable::init_memtable_mgr_(ObTxDataMemtableMgr *memtable_mgr)
{
  ASSERT_NE(nullptr, memtable_mgr);
  memtable_mgr->set_freezer(&tx_data_table_.freezer_);
  ASSERT_EQ(OB_SUCCESS, memtable_mgr->create_memtable(SCN::min_scn(), 1));
  ASSERT_EQ(1, memtable_mgr->get_memtable_count_());
}

void TestTxDataTable::check_freeze_(ObTxDataMemtableMgr *memtable_mgr,
                                    ObTxDataMemtable *&freezing_memtable,
                                    ObTxDataMemtable *&active_memtable)
{
  // do freeze
  STORAGETEST_LOG(INFO, "tx_data_table_.freeze_memtable() start.");
  ASSERT_EQ(OB_SUCCESS, memtable_mgr->freeze());
  ASSERT_EQ(2, memtable_mgr->get_memtable_count_());

  // check freeze result
  ObArray<ObTableHandleV2> memtables;
  ASSERT_EQ(OB_SUCCESS, memtable_mgr->get_all_memtables(memtables));
  ASSERT_EQ(2, memtables.size());

  ASSERT_EQ(OB_SUCCESS, memtables[0].get_tx_data_memtable(freezing_memtable));
  ASSERT_EQ(OB_SUCCESS, memtables[1].get_tx_data_memtable(active_memtable));
  ASSERT_EQ(ObTxDataMemtable::State::FREEZING, freezing_memtable->get_state());
  ASSERT_EQ(ObTxDataMemtable::State::ACTIVE, active_memtable->get_state());
  ASSERT_EQ(freezing_memtable->get_end_scn(), active_memtable->get_start_scn());

  // set frozen state
  freezing_memtable->set_state(ObTxDataMemtable::State::FROZEN);
}

void TestTxDataTable::check_commit_scn_row_(ObTxDataMemtableScanIterator &iter, ObTxDataMemtable *freezing_memtable)
{
  // int ret = OB_SUCCESS;
  ObCommitSCNsArray cur_commit_scns ;
  ObCommitSCNsArray past_commit_scns;
  ObCommitSCNsArray merged_commit_scns;
  auto &cur_array = cur_commit_scns.array_;
  auto &past_array = past_commit_scns.array_;
  auto &merged_array = merged_commit_scns.array_;
  share::SCN max_commit_version = share::SCN::min_scn();
  share::SCN max_start_scn = share::SCN::min_scn();

  // check sort commit version result
  {
    ASSERT_EQ(OB_SUCCESS, freezing_memtable->do_sort_by_start_scn_());
    share::SCN pre_start_scn = share::SCN::min_scn();
    ObTxData *last_commit_tx_data = nullptr;
    auto cur_node = freezing_memtable->sort_list_head_.next_;
    ASSERT_NE(nullptr, cur_node);
    int64_t cnt = 0;
    while (nullptr != cur_node) {
      ObTxData *tx_data = ObTxData::get_tx_data_by_sort_list_node(cur_node);
      ASSERT_GE(tx_data->start_scn_, pre_start_scn);
      if (ObTxData::COMMIT == tx_data->state_) {
        last_commit_tx_data = tx_data;
        max_commit_version = std::max(max_commit_version, tx_data->commit_version_);
        max_start_scn = std::max(max_start_scn, tx_data->start_scn_);
      }
      STORAGETEST_LOG(DEBUG,
                      "check_commit_scn_row",
                      KPC(tx_data),
                      KTIME(tx_data->start_scn_.convert_to_ts()),
                      KTIME(tx_data->end_scn_.convert_to_ts()));
      pre_start_scn = tx_data->start_scn_;
      cur_node = cur_node->next_;
      cnt++;
    }
    last_commit_tx_data->commit_version_ = share::SCN::plus(max_commit_version, 1);
    max_commit_version = last_commit_tx_data->commit_version_;
    ASSERT_EQ(cnt, inserted_cnt);
    fprintf(stdout, "total insert %ld tx data\n", cnt);

    ASSERT_EQ(OB_SUCCESS, iter.fill_in_cur_commit_scns_(cur_commit_scns));
    STORAGETEST_LOG(INFO, "cur_commit_scns count", K(cur_commit_scns.array_.count()));
    ASSERT_NE(0, cur_commit_scns.array_.count());
    for (int i = 1; i < cur_array.count() - 1; i++) {
      ASSERT_GE(cur_array.at(i).start_scn_,
                share::SCN::plus(cur_array.at(i - 1).start_scn_, iter.PERIODICAL_SELECT_INTERVAL_NS));
      ASSERT_GE(cur_array.at(i).commit_version_, cur_array.at(i - 1).commit_version_);
    }
    int i = cur_array.count() - 1;
    ASSERT_GE(cur_array.at(i).start_scn_, cur_array.at(i-1).start_scn_);
    ASSERT_GE(cur_array.at(i).commit_version_, cur_array.at(i-1).commit_version_);
    ASSERT_EQ(cur_array.at(i).start_scn_, max_start_scn);
    ASSERT_EQ(cur_array.at(i).commit_version_, max_commit_version);
  }

  // generate a fake past commit versions
  {
    generate_past_commit_scn_(past_commit_scns);
    ASSERT_NE(0, past_commit_scns.array_.count());
    ASSERT_EQ(true, past_commit_scns.is_valid());
  }

  // check merged result
  {
    share::SCN recycle_scn = share::SCN::minus(insert_start_scn, 100LL * ONE_SEC_NS/*100 seconds*/);
    ASSERT_EQ(OB_SUCCESS, iter.merge_cur_and_past_commit_verisons_(recycle_scn, cur_commit_scns,
                                                                   past_commit_scns,
                                                                   merged_commit_scns));
    for (int i = 0; i < merged_array.count(); i++) {
      STORAGE_LOG(INFO, "print merged array", K(merged_array.at(i)));
    }
    ASSERT_EQ(true, merged_commit_scns.is_valid());
    fprintf(stdout,
            "merge commit versions finish. past array count = %ld current array count = %ld merged array count = %ld\n",
            past_array.count(),
            cur_array.count(),
            merged_array.count());
  }

  // check commit versions serialization and deserialization
  {
    int64_t m_size = merged_commit_scns.get_serialize_size();
    ObArenaAllocator allocator;
    ObTxLocalBuffer buf_(allocator);
    buf_.reserve(m_size);

    int64_t pos = 0;
    ASSERT_EQ(OB_SUCCESS, merged_commit_scns.serialize(buf_.get_ptr(), m_size, pos));

    // void *ptr = allocator_.alloc(sizeof(ObCommitSCNsArray));
    ObCommitSCNsArray deserialize_commit_scns;
    pos = 0;
    ASSERT_EQ(OB_SUCCESS, deserialize_commit_scns.deserialize(buf_.get_ptr(), m_size, pos));

    const auto &deserialize_array = deserialize_commit_scns.array_;
    ASSERT_EQ(merged_array.count(), deserialize_array.count());
    for (int i = 0; i < merged_commit_scns.array_.count(); i++) {
      ASSERT_EQ(merged_array.at(i), deserialize_array.at(i));
    }
  }

  share::SCN sstable_end_scn = share::SCN::min_scn();
  share::SCN upper_trans_scn = share::SCN::min_scn();
  tx_data_table_.calc_upper_trans_version_cache_.commit_scns_ = merged_commit_scns;

  // check the situation when sstable_end_scn is greater than the greatest start_scn in
  // merged_array
  {
    sstable_end_scn = share::SCN::plus(max_start_scn, 1);
    upper_trans_scn.set_max();
    ASSERT_EQ(OB_SUCCESS,
              tx_data_table_.calc_upper_trans_scn_(sstable_end_scn, upper_trans_scn));
    ASSERT_EQ(max_commit_version, upper_trans_scn);
  }

  // check the normal calculation
  {
    share::SCN second_max_start_scn = share::SCN::minus(merged_array.at(merged_array.count() - 2).start_scn_, 1);
    for (int i = 0; i < 100; i++) {
      sstable_end_scn =
          share::SCN::minus(second_max_start_scn, rand64(ObTimeUtil::current_time_ns()) % (1100LL * ONE_SEC_NS));
      upper_trans_scn = share::SCN::max_scn();
      ASSERT_EQ(OB_SUCCESS,
                tx_data_table_.calc_upper_trans_scn_(sstable_end_scn, upper_trans_scn));
      ASSERT_NE(SCN::max_scn(), upper_trans_scn);
      ASSERT_NE(max_commit_version, upper_trans_scn);
    }
  }
}

void TestTxDataTable::do_basic_test()
{
  tx_data_table_.TEST_print_alloc_size_();
  // init tx data table
  ASSERT_EQ(OB_SUCCESS, tx_data_table_.init(ls_));
  set_freezer_();

  ObTxDataMemtableMgr *memtable_mgr = tx_data_table_.get_memtable_mgr_();
  init_memtable_mgr_(memtable_mgr);
  insert_tx_data_();
  insert_rollback_tx_data_();
  insert_abort_tx_data_();

  ObTxDataMemtable *freezing_memtable = nullptr;
  ObTxDataMemtable *active_memtable = nullptr;
  check_freeze_(memtable_mgr, freezing_memtable, active_memtable);
  inserted_cnt = freezing_memtable->get_tx_data_count();

  // sort tx data by trans id
  ObTxDataMemtableScanIterator iter(tx_data_table_.get_read_schema().iter_param_);
  ASSERT_EQ(OB_SUCCESS, iter.init(freezing_memtable));

  // check sort result
  {
    transaction::ObTransID pre_tx_id = INT64_MIN;
    auto cur_node = freezing_memtable->sort_list_head_.next_;
    ASSERT_NE(nullptr, cur_node);
    int64_t cnt = 0;
    while (nullptr != cur_node) {
      auto tx_id = ObTxData::get_tx_data_by_sort_list_node(cur_node)->tx_id_;
      ASSERT_GT(tx_id.get_id(), pre_tx_id.get_id());
      pre_tx_id = tx_id;
      cur_node = cur_node->next_;
      cnt++;
    }
    ASSERT_EQ(inserted_cnt, cnt);
  }

  check_commit_scn_row_(iter, freezing_memtable);

  // free memtable
  freezing_memtable->reset();
  active_memtable->reset();
}

void TestTxDataTable::do_undo_status_test()
{
  // init tx data table
  ASSERT_EQ(OB_SUCCESS, tx_data_table_.init(ls_));

  // the last undo action covers all the previous undo actions
  {
    ObTxData *tx_data = nullptr;
    tx_data = nullptr;
    ASSERT_EQ(OB_SUCCESS, tx_data_table_.alloc_tx_data(tx_data));

    tx_data->tx_id_ = rand();
    for (int i = 1; i <= 1000; i++) {
      transaction::ObUndoAction undo_action(10 * (i + 1), 10 * i);
      ASSERT_EQ(OB_SUCCESS, tx_data->add_undo_action(&tx_table_, undo_action));
    }
    ASSERT_EQ(1000 / 7 + 1, tx_data->undo_status_list_.undo_node_cnt_);

    {
      transaction::ObUndoAction undo_action(10000000, 10);
      ASSERT_EQ(OB_SUCCESS, tx_data->add_undo_action(&tx_table_, undo_action));
    }

    STORAGETEST_LOG(INFO, "", K(tx_data->undo_status_list_));
    ASSERT_EQ(1, tx_data->undo_status_list_.head_->size_);
    ASSERT_EQ(nullptr, tx_data->undo_status_list_.head_->next_);
    ASSERT_EQ(1, tx_data->undo_status_list_.undo_node_cnt_);
  }

  {
    // the last undo action covers eight previous undo actions
    // so the undo status just have one undo status node
    ObTxData *tx_data = nullptr;
    tx_data = nullptr;
    ASSERT_EQ(OB_SUCCESS, tx_data_table_.alloc_tx_data(tx_data));
    tx_data->tx_id_ = rand();

    for (int i = 1; i <= 14; i++) {
      transaction::ObUndoAction undo_action(i + 1, i);
      ASSERT_EQ(OB_SUCCESS, tx_data->add_undo_action(&tx_table_, undo_action));
    }
    ASSERT_EQ(2, tx_data->undo_status_list_.undo_node_cnt_);

    {
      transaction::ObUndoAction undo_action(15, 7);
      ASSERT_EQ(OB_SUCCESS, tx_data->add_undo_action(&tx_table_, undo_action));
    }

    STORAGETEST_LOG(INFO, "", K(tx_data->undo_status_list_));
    ASSERT_EQ(7, tx_data->undo_status_list_.head_->size_);
    ASSERT_EQ(nullptr, tx_data->undo_status_list_.head_->next_);
    ASSERT_EQ(1, tx_data->undo_status_list_.undo_node_cnt_);
  }
}

void TestTxDataTable::test_serialize_with_action_cnt_(int cnt)
{
    ObTxData *tx_data = nullptr;
    ASSERT_EQ(OB_SUCCESS, tx_data_table_.alloc_tx_data(tx_data));
    tx_data->tx_id_ = transaction::ObTransID(269381);
    tx_data->commit_version_.convert_for_logservice(ObTimeUtil::current_time_ns());
    tx_data->end_scn_.convert_for_logservice(ObTimeUtil::current_time_ns());
    tx_data->start_scn_.convert_for_logservice(tx_data->end_scn_.get_val_for_logservice() - 10000);
    tx_data->state_ = ObTxData::COMMIT;

    for (int i = 1; i <= cnt; i++) {
      transaction::ObUndoAction undo_action(10 * (i + 1), 10 * i);
      ASSERT_EQ(OB_SUCCESS, tx_data->add_undo_action(&tx_table_, undo_action));
    }
    int64_t node_cnt = 0;
    if (cnt % 7 == 0) {
      node_cnt = cnt / 7;
    } else {
      node_cnt = cnt / 7 + 1;
    }
    ASSERT_EQ(node_cnt, tx_data->undo_status_list_.undo_node_cnt_);

    char *buf = nullptr;
    ObArenaAllocator allocator;
    int64_t serialize_size = tx_data->get_serialize_size();
    int64_t pos = 0;
    ASSERT_NE(nullptr, buf = static_cast<char *>(allocator.alloc(serialize_size)));
    ASSERT_EQ(OB_SUCCESS, tx_data->serialize(buf, serialize_size, pos));

    ObTxData *new_tx_data = nullptr;
    ASSERT_EQ(OB_SUCCESS, tx_data_table_.alloc_tx_data(new_tx_data));
    new_tx_data->tx_id_ = transaction::ObTransID(269381);
    pos = 0;
    ASSERT_EQ(OB_SUCCESS, new_tx_data->deserialize(buf, serialize_size, pos,
                                                   *tx_data_table_.get_slice_allocator()));
    ASSERT_TRUE(new_tx_data->equals_(*tx_data));
    tx_data_table_.free_tx_data(tx_data);
    tx_data_table_.free_tx_data(new_tx_data);
}


void TestTxDataTable::do_tx_data_serialize_test()
{
  // init tx data table
  ASSERT_EQ(OB_SUCCESS, tx_data_table_.init(ls_));

  test_serialize_with_action_cnt_(0);
  test_serialize_with_action_cnt_(7);
  test_serialize_with_action_cnt_(8);
  test_serialize_with_action_cnt_(7 * 10000);
  test_serialize_with_action_cnt_(7 * 10000 + 1);
  test_commit_scns_serialize_();
}

void TestTxDataTable::test_commit_scns_serialize_()
{
  ObCommitSCNsArray cur_array;
  ObCommitSCNsArray past_array;
  ObCommitSCNsArray merged_array;
  ObCommitSCNsArray deserialized_array;

  share::SCN start_scn;
  start_scn.convert_for_logservice(ObTimeUtil::current_time_ns());
  int64_t MOD = 137171;
  share::SCN recycle_scn = share::SCN::plus(start_scn, 1000LL * MOD);

  int64_t array_cnt = 50000;
  STORAGE_LOG(INFO, "start generate past array");
  for (int64_t i = 0; i < array_cnt; i++) {
    start_scn = share::SCN::plus(start_scn, (rand64(ObTimeUtil::current_time_ns()) % MOD));
    ObCommitSCNsArray::Node node(start_scn, share::SCN::plus(start_scn, (rand64(ObTimeUtil::current_time_ns()) % MOD)));
    STORAGE_LOG(INFO, "", K(node));
    ASSERT_EQ(OB_SUCCESS, past_array.array_.push_back(node));
  }
  ASSERT_EQ(true, past_array.is_valid());

  STORAGE_LOG(INFO, "start generate cur array");
  for (int i = 0; i < array_cnt; i++) {
    start_scn = share::SCN::plus(start_scn, (rand64(ObTimeUtil::current_time_ns()) % MOD));
    ObCommitSCNsArray::Node node(start_scn, share::SCN::plus(start_scn, (rand64(ObTimeUtil::current_time_ns()) % MOD)));
    STORAGE_LOG(DEBUG, "", K(node));
    ASSERT_EQ(OB_SUCCESS, cur_array.array_.push_back(node));
  }
  ASSERT_EQ(true, cur_array.is_valid());

  ObTxDataMemtableScanIterator iter(tx_data_table_.get_read_schema().iter_param_);
  ASSERT_EQ(OB_SUCCESS, iter.merge_cur_and_past_commit_verisons_(recycle_scn, cur_array, past_array,
                                                                 merged_array));
  ASSERT_EQ(true, merged_array.is_valid());

  int64_t serialize_size = merged_array.get_serialize_size();
  ObArenaAllocator allocator;
  ObTxLocalBuffer buf(allocator);
  int64_t s_pos = 0;
  ASSERT_EQ(OB_SUCCESS, buf.reserve(serialize_size));
  ASSERT_EQ(OB_SUCCESS, merged_array.serialize(buf.get_ptr(), serialize_size, s_pos));

  int64_t d_pos = 0;
  ASSERT_EQ(OB_SUCCESS, deserialized_array.deserialize(buf.get_ptr(), buf.get_length(), d_pos));
  ASSERT_EQ(true, deserialized_array.is_valid());
  ASSERT_EQ(s_pos, d_pos);

  // do deserialize again on the same array
  d_pos = 0;
  ASSERT_EQ(OB_SUCCESS, deserialized_array.deserialize(buf.get_ptr(), buf.get_length(), d_pos));
  ASSERT_EQ(true, deserialized_array.is_valid());
  ASSERT_EQ(s_pos, d_pos);
}

void TestTxDataTable::do_repeat_insert_test() {
  ASSERT_EQ(OB_SUCCESS, tx_data_table_.init(ls_));

  set_freezer_();
  ObTxDataMemtableMgr *memtable_mgr = tx_data_table_.get_memtable_mgr_();
  ASSERT_NE(nullptr, memtable_mgr);
  memtable_mgr->set_freezer(&tx_data_table_.freezer_);
  ASSERT_EQ(OB_SUCCESS, memtable_mgr->create_memtable(SCN::min_scn(), 1));
  ASSERT_EQ(1, memtable_mgr->get_memtable_count_());

  insert_start_scn.convert_for_logservice(ObTimeUtil::current_time_ns());

  ObTxData *tx_data = nullptr;
  int64_t int_tx_id = 0;
  transaction::ObTransID tx_id;

  for (int i = 1; i <= 100; i++) {
    tx_id = transaction::ObTransID(269381);
    tx_data = nullptr;
    ASSERT_EQ(OB_SUCCESS, tx_data_table_.alloc_tx_data(tx_data));

    // fill in data
    tx_data->tx_id_ = tx_id;
    tx_data->start_scn_ = insert_start_scn;
    tx_data->commit_version_.set_invalid();
    tx_data->end_scn_ = share::SCN::plus(insert_start_scn, i);
    tx_data->state_ = ObTxData::RUNNING;
    int undo_act_num = (rand() & 31) + 1;
    for (int j = 0; j < undo_act_num; j++) {
      auto from = random();
      auto to = (from - 10000 > 0) ? (from - 100000) : 1;
      transaction::ObUndoAction undo_action(from, to);
      ASSERT_EQ(OB_SUCCESS, tx_data->add_undo_action(&tx_table_, undo_action));
    }

    ASSERT_EQ(OB_SUCCESS, tx_data_table_.insert(tx_data));
  }

  memtable_mgr->destroy();

}

void TestTxDataTable::do_multiple_init_iterator_test()
{
  ASSERT_EQ(OB_SUCCESS, tx_data_table_.init(ls_));
  set_freezer_();

  ObTxDataMemtableMgr *memtable_mgr = tx_data_table_.get_memtable_mgr_();
  init_memtable_mgr_(memtable_mgr);
  tx_data_num = 10240;
  insert_tx_data_();

  ObTxDataMemtable *freezing_memtable = nullptr;
  ObTxDataMemtable *active_memtable = nullptr;
  check_freeze_(memtable_mgr, freezing_memtable, active_memtable);
  inserted_cnt = freezing_memtable->get_tx_data_count();

  // sort tx data by trans id
  {
    ObTxDataMemtableScanIterator iter1(tx_data_table_.get_read_schema().iter_param_);
    ASSERT_EQ(OB_SUCCESS, iter1.init(freezing_memtable));

    ObTxDataMemtableScanIterator iter2(tx_data_table_.get_read_schema().iter_param_);
    ASSERT_EQ(OB_STATE_NOT_MATCH, iter2.init(freezing_memtable));

    // iter1 can init succeed because of the reset function in init()
    ASSERT_EQ(OB_SUCCESS, iter1.init(freezing_memtable));

    // iter2 still can not init
    ASSERT_EQ(OB_STATE_NOT_MATCH, iter2.init(freezing_memtable));

    iter1.reset();
    // now iter2 can successfully init
    ASSERT_EQ(OB_SUCCESS, iter2.init(freezing_memtable));
  }

  // iter3 can successfully init due to the destruct function of iterator
  ObTxDataMemtableScanIterator iter3(tx_data_table_.get_read_schema().iter_param_);
  ASSERT_EQ(OB_SUCCESS, iter3.init(freezing_memtable));
}

void TestTxDataTable::fake_ls_(ObLS &ls)
{
  ls.ls_meta_.tenant_id_ = 1;
  ls.ls_meta_.ls_id_.id_ = 1001;
  ls.ls_meta_.gc_state_ = logservice::LSGCState::NORMAL;
  ls.ls_meta_.migration_status_ = ObMigrationStatus::OB_MIGRATION_STATUS_NONE;
  ls.ls_meta_.restore_status_ = ObLSRestoreStatus::RESTORE_NONE;
  ls.ls_meta_.replica_type_ = ObReplicaType::REPLICA_TYPE_FULL;
  ls.ls_meta_.rebuild_seq_ = 0;
}

void TestTxDataTable::do_print_leak_slice_test()
{
  const int32_t CONCURRENCY = 4;
  ObMemAttr attr;
  ObSliceAlloc slice_allocator;

  slice_allocator.init(128, OB_MALLOC_NORMAL_BLOCK_SIZE, default_blk_alloc, attr);
  slice_allocator.set_nway(CONCURRENCY);
  std::vector<std::thread> alloc_threads;

  for (int i = 0; i < CONCURRENCY; i++) {
    alloc_threads.push_back(std::thread([&slice_allocator](){
      int64_t alloc_times = 1123;
      std::vector<void*> allocated_mem_ptr;
      while (--alloc_times > 0) {
        void *ret = slice_allocator.alloc();
        if (nullptr != ret) {
          allocated_mem_ptr.push_back(ret);
        }
      }

      STORAGE_LOG(INFO, "unfreed slice", KP(allocated_mem_ptr[0]));
      for (int k = 1; k < allocated_mem_ptr.size(); k++) {
        slice_allocator.free(allocated_mem_ptr[k]);
      }
    }));
  }

  for (int i = 0; i < CONCURRENCY; i++) {
    alloc_threads[i].join();
  }

  slice_allocator.destroy();
}

TEST_F(TestTxDataTable, basic_test)
{
  tx_data_num = const_data_num;
  do_basic_test();
}

TEST_F(TestTxDataTable, repeat_insert_test) { do_repeat_insert_test(); }

TEST_F(TestTxDataTable, undo_status_test) { do_undo_status_test(); }

TEST_F(TestTxDataTable, serialize_test) { do_tx_data_serialize_test(); }

// TEST_F(TestTxDataTable, print_leak_slice) { do_print_leak_slice_test(); }

// TEST_F(TestTxDataTable, iterate_init_test) { do_multiple_init_iterator_test(); }


}  // namespace storage
}  // namespace oceanbase

int main(int argc, char **argv)
{
  int ret = 1;
  system("rm -f test_tx_data_table.log");
  ObLogger &logger = ObLogger::get_logger();
  logger.set_file_name("test_tx_data_table.log", true);
  logger.set_log_level(OB_LOG_LEVEL_INFO);
  TRANS_LOG(WARN, "init memory pool error!");
  // OB_LOGGER.set_enable_async_log(false);

  // TEST_LOG("GCONF.syslog_io_bandwidth_limit %ld ", GCONF.syslog_io_bandwidth_limit.get_value());
  // LOG_INFO("GCONF.syslog_io_bandwidth_limit ", K(GCONF.syslog_io_bandwidth_limit.get_value()));
  // GCONF.syslog_io_bandwidth_limit.set_value("4000MB");
  // TEST_LOG("GCONF.syslog_io_bandwidth_limit %ld ", GCONF.syslog_io_bandwidth_limit.get_value());
  // LOG_INFO("GCONF.syslog_io_bandwidth_limit ", K(GCONF.syslog_io_bandwidth_limit.get_value()));

  if (OB_SUCCESS != ObClockGenerator::init()) {
    TRANS_LOG(WARN, "ObClockGenerator::init error!");
  } else {
    if (argc > 1) {
      const_data_num = atoi(argv[1]);
    } else {
      const_data_num = 524288;
    }
    testing::InitGoogleTest(&argc, argv);
    ret = RUN_ALL_TESTS();
  }

  return ret;
}
