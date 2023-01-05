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

#ifndef OCEANBASE_ROOTSERVER_FREEZE_OB_MAJOR_MERGE_PROGRESS_CHECKER_
#define OCEANBASE_ROOTSERVER_FREEZE_OB_MAJOR_MERGE_PROGRESS_CHECKER_

#include "share/ob_zone_merge_info.h"
#include "share/tablet/ob_tablet_info.h"
#include "rootserver/ob_root_utils.h"
#include "rootserver/freeze/ob_checksum_validator.h"
#include "common/ob_tablet_id.h"

namespace oceanbase
{
namespace share
{
class ObTabletTableOperator;
class ObLSInfo;
class ObLSTableOperator; 
class ObIServerTrace;
struct ObTabletInfo;
class ObLSReplica;
namespace schema
{
class ObSchemaGetterGuard;
}
}
namespace common
{
class ObMySQLProxy;
}

namespace rootserver
{
class ObZoneMergeManager;

class ObMajorMergeProgressChecker
{
public:
  ObMajorMergeProgressChecker();
  virtual ~ObMajorMergeProgressChecker() {}

  int init(const uint64_t tenant_id,
           common::ObMySQLProxy &sql_proxy, 
           share::schema::ObMultiVersionSchemaService &schema_service,
           ObZoneMergeManager &zone_merge_mgr,
           share::ObLSTableOperator &lst_operator,
           share::ObIServerTrace &server_trace);

  int prepare_handle(); // For each round major_freeze, need invoke this once.

  int check_merge_progress(const volatile bool &stop,
                           const share::SCN &global_broadcast_scn,
                           share::ObAllZoneMergeProgress &all_progress);

  int check_verification(const share::SCN &global_broadcast_scn,
                         const int64_t expected_epoch);

  // @exist_uncompacted means not all table finished compaction
  // @exist_unverified means not all table finished verification
  int check_table_status(bool &exist_uncompacted, bool &exist_unverified);

private:
  int check_tablet(const share::ObTabletInfo &tablet_info,
                   const common::hash::ObHashMap<ObTabletID, uint64_t> &tablet_map,
                   share::ObAllZoneMergeProgress &all_progress,
                   const share::SCN &global_broadcast_scn,
                   share::schema::ObSchemaGetterGuard &schema_guard);
  int check_tablet_compaction_scn(share::ObAllZoneMergeProgress &all_progress,
                                  const share::SCN &global_broadcast_scn,
                                  const share::ObTabletInfo &tablet,
                                  const share::ObLSInfo &ls_info);
  int check_majority_integrated(share::schema::ObSchemaGetterGuard &schema_guard, 
                                const share::ObTabletInfo &tablet_info,
                                const share::ObLSInfo &ls_info);

  int get_associated_replica_num(share::schema::ObSchemaGetterGuard &schema_guard,
                                 int64_t &paxos_replica_num,
                                 int64_t &full_replica_num,
                                 int64_t &all_replica_num,
                                 int64_t &majority);
  // get member_list of ls leader replica
  int get_member_list(const share::ObLSInfo &ls_info,
                      share::ObLSReplica::MemberList &member_list) const;
  int is_replica_in_ls_member_list(const share::ObTabletReplica &replica,
                                   const share::ObLSReplica::MemberList &member_list,
                                   bool &is_in_member_list) const;

private:
  bool is_inited_;
  uint64_t tenant_id_;
  common::ObMySQLProxy *sql_proxy_;
  share::schema::ObMultiVersionSchemaService *schema_service_;
  ObZoneMergeManager *zone_merge_mgr_;
  share::ObLSTableOperator *lst_operator_;
  share::ObIServerTrace *server_trace_;
  // record each tablet compaction status: INITIAL/COMPACTED/FINISHED
  hash::ObHashMap<share::ObTabletLSPair, share::ObTabletCompactionStatus> tablet_compaction_map_;
  int64_t table_count_;
  // record each table compaction/verify status
  hash::ObHashMap<uint64_t, share::ObTableCompactionInfo> table_compaction_map_; // <table_id, conpaction_info>
  ObIndexChecksumValidator index_validator_;

  DISALLOW_COPY_AND_ASSIGN(ObMajorMergeProgressChecker);
};

} // namespace rootserver
} // namespace oceanbase

#endif // OCEANBASE_ROOTSERVER_FREEZE_OB_MAJOR_MERGE_PROGRESS_CHECKER_
