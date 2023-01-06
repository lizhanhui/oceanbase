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

#ifndef OCEANBASE_SHARE_OB_TENANT_INFO_PROXY_H_
#define OCEANBASE_SHARE_OB_TENANT_INFO_PROXY_H_

#include "share/ob_ls_id.h"//share::ObLSID
#include "share/ob_tenant_role.h"//ObTenantRole
#include "share/ob_tenant_switchover_status.h"//ObTenantSwitchoverStatus
#include "lib/container/ob_array.h"//ObArray
#include "lib/container/ob_iarray.h"//ObIArray
#include "share/scn.h"
//#include "share/ls/ob_ls_status_operator.h"


namespace oceanbase
{

namespace common
{
class ObMySQLProxy;
class ObISQLClient;
namespace sqlclient
{
class ObMySQLResult;
}
}
namespace share
{

struct ObAllTenantInfo
{
  OB_UNIS_VERSION(1);
public:
 ObAllTenantInfo() {reset();};
 virtual ~ObAllTenantInfo() {}
 /**
  * @description: init_tenant_info by cluster_info
  * @param[in] cluster_info
  * @param[in] is_restore
  * @param[in] tenant_id
  */
 int init(const uint64_t tenant_id, const ObTenantRole type);
 int init(const uint64_t tenant_id, const ObTenantRole &tenant_role, const ObTenantSwitchoverStatus &switchover_status, 
          int64_t switchover_epoch, const SCN &sync_scn, const SCN &replayable_scn,
          const SCN &standby_scn, const SCN &recovery_until_scn);
 ObAllTenantInfo &operator=(const ObAllTenantInfo &other);
 int assign(const ObAllTenantInfo &other);
 void reset();
 bool is_valid() const;
 const SCN get_ref_scn() const;

 // ObTenantRole related function
 bool is_standby() const { return tenant_role_.is_standby(); }
 bool is_primary() const { return tenant_role_.is_primary(); }
 bool is_restore() const { return tenant_role_.is_restore(); }

 // ObTenantSwitchoverStatus related function
#define IS_TENANT_STATUS(STATUS) \
  bool is_##STATUS##_status() const { return switchover_status_.is_##STATUS##_status(); };

IS_TENANT_STATUS(normal) 
IS_TENANT_STATUS(switching) 
IS_TENANT_STATUS(prepare_flashback) 
IS_TENANT_STATUS(flashback) 
#undef IS_TENANT_STATUS 

 TO_STRING_KV(K_(tenant_id), K_(tenant_role), K_(switchover_status),
              K_(switchover_epoch), K_(sync_scn), K_(replayable_scn),
              K_(standby_scn), K_(recovery_until_scn));

  // Getter&Setter
  const ObTenantRole &get_tenant_role() const { return tenant_role_; }
  const ObTenantSwitchoverStatus &get_switchover_status() const { return switchover_status_; }

#define Property_declare_var(variable_type, variable_name)\
private:\
  variable_type variable_name##_;\
public:\
  variable_type get_##variable_name() const\
  { return variable_name##_;}

  Property_declare_var(uint64_t, tenant_id)
  Property_declare_var(int64_t, switchover_epoch)
  Property_declare_var(share::SCN, sync_scn)
  Property_declare_var(share::SCN, replayable_scn)
  Property_declare_var(share::SCN, standby_scn)
  //TODO msy164651 no use now
  Property_declare_var(share::SCN, recovery_until_scn)
#undef Property_declare_var
private:
  ObTenantRole tenant_role_;
  ObTenantSwitchoverStatus switchover_status_;
};

class ObAllTenantInfoProxy
{
public:
  ObAllTenantInfoProxy() {};
  virtual ~ObAllTenantInfoProxy(){}

public:
  /**
   * @description: init_tenant_info to inner table while create tenant
   * @param[in] tenant_info 
   * @param[in] proxy
   */ 
  static int init_tenant_info(const ObAllTenantInfo &tenant_info, ObISQLClient *proxy);
  /**
   * @description: get all normal tenant's tenant_info from inner table
   * @param[in] proxy
   * @param[out] tenant_infos
   */
  static int load_all_tenant_infos(
             ObISQLClient *proxy,
             common::ObIArray<ObAllTenantInfo> &tenant_infos);
  /**
   * @description: get all standby tenants from inner table
   * @param[in] proxy
   * @param[out] tenant_ids
   */
  static int get_standby_tenants(
             ObISQLClient *proxy,
             common::ObIArray<uint64_t> &tenant_ids);
  /**
   * @description: get target tenant's tenant_info from inner table 
   * @param[in] tenant_id
   * @param[in] proxy
   * @param[out] tenant_info 
   */
  static int load_tenant_info(const uint64_t tenant_id, ObISQLClient *proxy,
                              const bool for_update,
                              ObAllTenantInfo &tenant_info);
  /**
   * @description: update tenant recovery status 
   * @param[in] tenant_id
   * @param[in] proxy
   * @param[in] status: the target status while update recovery status
   * @param[in] sync_scn : sync point 
   * @param[in] replay_scn : max replay point 
   * @param[in] reabable_scn : standby readable scn
   */
  static int update_tenant_recovery_status(const uint64_t tenant_id,
                                           ObMySQLProxy *proxy,
                                           ObTenantSwitchoverStatus status,
                                           const SCN &sync_scn,
                                           const SCN &replay_scn,
                                           const SCN &reabable_scn);
  /**
   * @description: update tenant switchover status of __all_tenant_info
   * @param[in] tenant_id : user tenant id
   * @param[in] proxy
   * @param[in] switchover_epoch, for operator concurrency
   * @param[in] old_status : old_status of current, which must be match
   * @param[in] status : target switchover status to be update
   * return :
   *   OB_SUCCESS update tenant switchover status successfully
   *   OB_NEED_RETRY switchover_epoch or old_status not match, need retry 
   */
  static int update_tenant_switchover_status(const uint64_t tenant_id, ObISQLClient *proxy,
                                int64_t switchover_epoch,
                                const ObTenantSwitchoverStatus &old_status,
                                const ObTenantSwitchoverStatus &status);
  /**
   * @description: update tenant role of __all_tenant_info
   * @param[in] tenant_id
   * @param[in] proxy
   * @param[in] old_switchover_epoch, for operator concurrency
   * @param[in] new_role : target tenant role to be update
   * @param[in] status : target switchover status to be update
   * return :
   *   OB_SUCCESS update tenant role successfully
   *   OB_NEED_RETRY old_switchover_epoch not match, need retry
   */
  static int update_tenant_role(const uint64_t tenant_id, ObISQLClient *proxy,
    int64_t old_switchover_epoch,
    const ObTenantRole &new_role, const ObTenantSwitchoverStatus &status,
    int64_t &new_switchover_epoch);
  static int fill_cell(common::sqlclient::ObMySQLResult *result, ObAllTenantInfo &tenant_info);
};
}
}

#endif /* !OCEANBASE_SHARE_OB_TENANT_INFO_PROXY_H_ */
