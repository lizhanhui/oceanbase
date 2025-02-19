#!/usr/bin/env python
# -*- coding: utf-8 -*-

import sys
import os
import time
import mysql.connector
from mysql.connector import errorcode
import logging
import time
import actions

#### START
# 1 检查版本号
def check_cluster_version(cur, timeout):
  current_cluster_version = actions.get_current_cluster_version()
  actions.wait_parameter_sync(cur, "min_observer_version", current_cluster_version, timeout)

# 2 检查租户版本号
def check_data_version(cur, query_cur, timeout):
  # check compatible
  current_data_version = actions.get_current_data_version()
  actions.wait_parameter_sync(cur, "compatible", current_data_version, 10)

  # check target_data_version/current_data_version except standby tenant
  sql = "select tenant_id from oceanbase.__all_tenant except select tenant_id from oceanbase.__all_virtual_tenant_info where tenant_role = 'STANDBY'"
  (desc, results) = query_cur.exec_query(sql)
  if len(results) == 0:
    logging.warn('result cnt not match')
    raise e
  tenant_count = len(results)
  tenant_ids_str = ''
  for index, row in enumerate(results):
    tenant_ids_str += """{0}{1}""".format((',' if index > 0 else ''), row[0])

  int_current_data_version = actions.get_version(current_data_version)
  sql = "select count(*) from __all_virtual_core_table where column_name in ('target_data_version', 'current_data_version') and column_value = {0} and tenant_id in ({1})".format(int_current_data_version, tenant_ids_str)
  (desc, results) = query_cur.exec_query(sql)
  if len(results) != 1 or len(results[0]) != 1:
    logging.warn('result cnt not match')
    raise e
  elif 2 * tenant_count != results[0][0]:
    logging.warn('target_data_version/current_data_version not match with {0}, tenant_cnt:{1}, result_cnt:{2}'.format(current_data_version, tenant_count, results[0][0]))
    raise e
  else:
    logging.info("all tenant's target_data_version/current_data_version are match with {0}".format(current_data_version))

# 3 检查内部表自检是否成功
def check_root_inspection(query_cur, timeout):
  sql = "select count(*) from oceanbase.__all_virtual_upgrade_inspection where info != 'succeed'"
  times = timeout if timeout > 0 else 180
  while times > 0 :
    (desc, results) = query_cur.exec_query(sql)
    if results[0][0] == 0:
      break
    time.sleep(10)
    times -= 1
  if times == 0:
    logging.warn('check root inspection failed!')
    raise e
  logging.info('check root inspection success')

# 4 开ddl
def enable_ddl(cur, timeout):
  actions.set_parameter(cur, 'enable_ddl', 'True', timeout)

# 5 打开rebalance
def enable_rebalance(cur, timeout):
  actions.set_parameter(cur, 'enable_rebalance', 'True', timeout)

# 6 打开rereplication
def enable_rereplication(cur, timeout):
  actions.set_parameter(cur, 'enable_rereplication', 'True', timeout)

# 7 打开major freeze
def enable_major_freeze(cur, timeout):
  actions.set_parameter(cur, 'enable_major_freeze', 'True', timeout)

# 开始升级后的检查
def do_check(conn, cur, query_cur, timeout):
  try:
    check_cluster_version(cur, timeout)
    check_data_version(cur, query_cur, timeout)
    check_root_inspection(query_cur, timeout)
    enable_ddl(cur, timeout)
    enable_rebalance(cur, timeout)
    enable_rereplication(cur, timeout)
    enable_major_freeze(cur, timeout)
  except Exception, e:
    logging.exception('run error')
    raise e
