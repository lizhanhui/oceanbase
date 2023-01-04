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

#ifndef OB_SHARE_RESOURCE_MANAGER_OB_PLAN_INFO_H_
#define OB_SHARE_RESOURCE_MANAGER_OB_PLAN_INFO_H_

#include "lib/utility/ob_macro_utils.h"
#include "common/data_buffer.h"
#include "lib/string/ob_string.h"
#include "share/ob_define.h"

namespace oceanbase
{
namespace common
{
class ObString;
}
namespace share
{

// 为了便于作为 hash value，所以把 ObString 包一下
class ObGroupName
{
public:
  ObGroupName() {};
  int set_group_name(const common::ObString &name)
  {
    common::ObDataBuffer allocator(group_name_buf_, common::OB_MAX_RESOURCE_PLAN_NAME_LENGTH);
    return common::ob_write_string(allocator, name, group_name_);;
  }
  // 自动隐式类型转换成 ObString
  operator const common::ObString& () const
  {
    return group_name_;
  }
  const common::ObString &get_group_name() const
  {
    return group_name_;
  }
  int assign(const ObGroupName &other)
  {
    common::ObDataBuffer allocator(group_name_buf_, common::OB_MAX_RESOURCE_PLAN_NAME_LENGTH);
    return common::ob_write_string(allocator, other.group_name_, group_name_);
  }
  TO_STRING_KV(K_(group_name));
private:
  common::ObString group_name_;
  char group_name_buf_[common::OB_MAX_RESOURCE_PLAN_NAME_LENGTH];
};

class ObPlanDirective
{
public:
  ObPlanDirective() :
      tenant_id_(common::OB_INVALID_ID),
      mgmt_p1_(100),
      utilization_limit_(100),
      group_name_(),
      level_(1)
  {}
  ~ObPlanDirective() = default;
public:
  int set_tenant_id(uint64_t tenant_id)
  {
    tenant_id_ = tenant_id;
    return common::OB_SUCCESS;
  }
  int set_mgmt_p1(int64_t mgmt_p1)
  {
    mgmt_p1_ = mgmt_p1;
    return common::OB_SUCCESS;
  }
  int set_utilization_limit(int64_t limit)
  {
    utilization_limit_ = limit;
    return common::OB_SUCCESS;
  }
  int set_group_name(const common::ObString &name)
  {
    return group_name_.set_group_name(name);
  }
  int assign(const ObPlanDirective &other);
  TO_STRING_KV(K_(tenant_id),
               "group_name", group_name_.get_group_name(),
               K_(mgmt_p1),
               K_(utilization_limit),
               K_(level));
public:
  uint64_t tenant_id_;
  int64_t mgmt_p1_;
  int64_t utilization_limit_;
  ObGroupName group_name_;
  int level_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObPlanDirective);
};

class ObResourceMappingRule
{
public:
  ObResourceMappingRule() :
      tenant_id_(common::OB_INVALID_ID),
      attr_(),
      value_(),
      group_()
  {}
  ~ObResourceMappingRule() = default;
public:
  int set_tenant_id(uint64_t tenant_id)
  {
    tenant_id_ = tenant_id;
    return common::OB_SUCCESS;
  }
  int set_group(const common::ObString &group)
  {
    common::ObDataBuffer allocator(group_buf_, common::OB_MAX_RESOURCE_PLAN_NAME_LENGTH);
    return common::ob_write_string(allocator, group, group_);;
  }
  int set_attr(const common::ObString &attr)
  {
    common::ObDataBuffer allocator(attr_buf_, common::OB_MAX_RESOURCE_PLAN_NAME_LENGTH);
    return common::ob_write_string(allocator, attr, attr_);;
  }
  int set_value(const common::ObString &value)
  {
    common::ObDataBuffer allocator(value_buf_, common::OB_MAX_RESOURCE_PLAN_NAME_LENGTH);
    return common::ob_write_string(allocator, value, value_);;
  }
  int assign(const ObResourceMappingRule &other);
  TO_STRING_KV(K_(tenant_id),
               K_(attr),
               K_(value),
               K_(group));
public:
  uint64_t tenant_id_;
  common::ObString attr_;
  common::ObString value_;
  common::ObString group_;
private:
  char attr_buf_[common::OB_MAX_RESOURCE_PLAN_NAME_LENGTH];
  char value_buf_[common::OB_MAX_RESOURCE_PLAN_NAME_LENGTH];
  char group_buf_[common::OB_MAX_RESOURCE_PLAN_NAME_LENGTH];
  DISALLOW_COPY_AND_ASSIGN(ObResourceMappingRule);
};

class ObResourceIdNameMappingRule
{
public:
  ObResourceIdNameMappingRule() :
      tenant_id_(common::OB_INVALID_ID),
      group_id_(),
      group_name_()
  {}
  ~ObResourceIdNameMappingRule() = default;
public:
  void set_tenant_id(uint64_t tenant_id)
  {
    tenant_id_ = tenant_id;
  }
  void set_group_id(uint64_t group_id)
  {
    group_id_ = group_id;
  }
  int set_group_name(const common::ObString &name)
  {
    return group_name_.set_group_name(name);
  }
  int assign(const ObResourceIdNameMappingRule &other);
  TO_STRING_KV(K_(tenant_id),
               K_(group_id),
               "group_name", group_name_.get_group_name());
public:
  uint64_t tenant_id_;
  uint64_t group_id_;
  ObGroupName group_name_;
private:
  char group_name_buf_[common::OB_MAX_RESOURCE_PLAN_NAME_LENGTH];
  DISALLOW_COPY_AND_ASSIGN(ObResourceIdNameMappingRule);
};


class ObResourceUserMappingRule
{
public:
  ObResourceUserMappingRule() :
      tenant_id_(common::OB_INVALID_ID),
      user_id_(),
      group_id_(),
      group_name_()
  {}
  ~ObResourceUserMappingRule() = default;
public:
  void set_tenant_id(uint64_t tenant_id)
  {
    tenant_id_ = tenant_id;
  }
  void set_user_id(uint64_t user_id)
  {
    user_id_ = user_id;
  }
  void set_group_id(uint64_t group_id)
  {
    group_id_ = group_id;
  }
  int set_group_name(const common::ObString &name)
  {
    return group_name_.set_group_name(name);
  }
  int assign(const ObResourceUserMappingRule &other);
  TO_STRING_KV(K_(tenant_id),
               K_(user_id),
               K_(group_id),
               "group_name", group_name_.get_group_name());
public:
  uint64_t tenant_id_;
  uint64_t user_id_;
  uint64_t group_id_;
  ObGroupName group_name_;
private:
  char group_name_buf_[common::OB_MAX_RESOURCE_PLAN_NAME_LENGTH];
  DISALLOW_COPY_AND_ASSIGN(ObResourceUserMappingRule);
};


class ObResourceColumnMappingRule
{
public:
  ObResourceColumnMappingRule() :
      tenant_id_(common::OB_INVALID_ID),
      database_id_(common::OB_INVALID_ID),
      table_name_(),
      column_name_(),
      literal_value_(),
      user_name_(),
      group_id_(),
      case_mode_(common::OB_NAME_CASE_INVALID)
  {}
  ~ObResourceColumnMappingRule() = default;
public:
  void set_tenant_id(uint64_t tenant_id) { tenant_id_ = tenant_id; }
  void set_database_id(uint64_t database_id) { database_id_ = database_id; }
  void set_table_name(common::ObString table_name) {table_name_ = table_name; }
  void set_column_name(common::ObString column_name) {column_name_ = column_name; }
  void set_literal_value(common::ObString literal_value) {literal_value_ = literal_value; }
  void set_user_name(common::ObString user_name) { user_name_ = user_name; }
  void set_group_id(uint64_t group_id) { group_id_ = group_id; }
  void set_case_mode(common:: ObNameCaseMode case_mode) { case_mode_ = case_mode; }
  int write_string_values(uint64_t tenant_id,
                          common::ObString table_name, common::ObString column_name,
                          common::ObString literal_value, common::ObString user_name);
  void reset_table_column_name();
  void reset_user_name_literal();
  void reset()
  {
    reset_table_column_name();
    reset_user_name_literal();
  }
  int assign(const ObResourceColumnMappingRule &other);
  TO_STRING_KV(K_(tenant_id), K_(database_id), K_(table_name), K_(column_name),
              K_(literal_value), K_(user_name), K_(group_id), K_(case_mode));
public:
  uint64_t tenant_id_;
  uint64_t database_id_;
  common::ObString table_name_;
  common::ObString column_name_;
  common::ObString literal_value_;
  common::ObString user_name_;
  uint64_t group_id_;
  common:: ObNameCaseMode case_mode_;

private:
  //char group_name_buf_[common::OB_MAX_RESOURCE_PLAN_NAME_LENGTH];
  DISALLOW_COPY_AND_ASSIGN(ObResourceColumnMappingRule);
};

}
}
#endif /* OB_SHARE_RESOURCE_MANAGER_OB_PLAN_INFO_H_ */
//// end of header file
