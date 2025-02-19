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

#include "lib/file/file_directory_utils.h"
#include "logservice/palf/log_define.h"
#include <iostream>
#define USING_LOG_PREFIX RPC_TEST
#include<dirent.h>
#include <memory>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include "util/easy_mod_stat.h"
#include "lib/oblog/ob_log.h"
#include "lib/oblog/ob_log_module.h"
#include "ob_simple_log_server.h"
#include "lib/random/ob_random.h"
#include "common/log/ob_log_constants.h"
#include "share/ob_io_device_helper.h"
#include "share/ob_thread_mgr.h"
#include "logservice/palf/palf_options.h"

namespace oceanbase
{
namespace unittest
{
using namespace oceanbase;
using namespace oceanbase::rpc;
using namespace oceanbase::rpc::frame;
using namespace oceanbase::obrpc;
using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace palf;
using namespace palf::election;

uint32_t get_local_addr(const char *dev_name)
{
  int fd, intrface;
  struct ifreq buf[16];
  struct ifconf ifc;

  if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    return 0;
  }

  ifc.ifc_len = sizeof(buf);
  ifc.ifc_buf = (caddr_t)buf;
  if (ioctl(fd, SIOCGIFCONF, (char *)&ifc) != 0) {
    close(fd);
    return 0;
  }

  intrface = static_cast<int>(ifc.ifc_len / sizeof(struct ifreq));
  while (intrface-- > 0) {
    if (ioctl(fd, SIOCGIFFLAGS, (char *)&buf[intrface]) != 0) {
      continue;
    }
    if ((buf[intrface].ifr_flags & IFF_LOOPBACK) != 0)
      continue;
    if (!(buf[intrface].ifr_flags & IFF_UP))
      continue;
    if (dev_name != NULL && strcmp(dev_name, buf[intrface].ifr_name))
      continue;
    if (!(ioctl(fd, SIOCGIFADDR, (char *)&buf[intrface]))) {
      close(fd);
      return ((struct sockaddr_in *)(&buf[intrface].ifr_addr))->sin_addr.s_addr;
    }
  }
  close(fd);
  return 0;
}

std::string get_local_ip()
{
  uint32_t ip = get_local_addr("bond0");
 if (ip == 0) {
    ip = get_local_addr("eth0");
  }
  if (ip == 0) {
    return "";
  }
  return inet_ntoa(*(struct in_addr *)(&ip));
}

void ObLogDeliver::init_all_propocessor_()
{
  register_rpc_propocessor_<LogPushReqP>(LogPushReqP::PCODE);
  register_rpc_propocessor_<LogPushRespP>(LogPushRespP::PCODE);
  register_rpc_propocessor_<LogFetchReqP>(LogFetchReqP::PCODE);
  register_rpc_propocessor_<LogPrepareReqP>(LogPrepareReqP::PCODE);
  register_rpc_propocessor_<LogPrepareRespP>(LogPrepareRespP::PCODE);
  register_rpc_propocessor_<LogChangeConfigMetaReqP>(LogChangeConfigMetaReqP::PCODE);
  register_rpc_propocessor_<LogChangeConfigMetaRespP>(LogChangeConfigMetaRespP::PCODE);
  register_rpc_propocessor_<LogChangeModeMetaReqP>(LogChangeModeMetaReqP::PCODE);
  register_rpc_propocessor_<LogChangeModeMetaRespP>(LogChangeModeMetaRespP::PCODE);
  register_rpc_propocessor_<LogGetMCStP>(LogGetMCStP::PCODE);
  register_rpc_propocessor_<LogLearnerReqP>(LogLearnerReqP::PCODE);
  register_rpc_propocessor_<LogRegisterParentReqP>(LogRegisterParentReqP::PCODE);
  register_rpc_propocessor_<LogRegisterParentRespP>(LogRegisterParentRespP::PCODE);
  register_rpc_propocessor_<LogNotifyRebuildReqP>(LogNotifyRebuildReqP::PCODE);
  register_rpc_propocessor_<CommittedInfoP>(CommittedInfoP::PCODE);
}

int ObSimpleLogServer::simple_init(
    const std::string &cluster_name,
    const common::ObAddr &addr,
    const int64_t node_id,
    const bool is_bootstrap = false)
{
  int ret = OB_SUCCESS;
  ObTimeGuard guard("simple_init", 0);
  SERVER_LOG(INFO, "simple_log_server simple_init start", K(node_id), K(addr_));
  tenant_base_ = OB_NEW(ObTenantBase, "TestBase", node_id);
  tenant_base_->init();
  guard.click("init tenant_base");
  node_id_ = node_id;
  ObTenantEnv::set_tenant(tenant_base_);
  if (is_bootstrap && OB_FAIL(init_memory_dump_timer_())) {
    SERVER_LOG(ERROR, "init_memory_dump_timer_ failed", K(ret), K_(node_id));
  } else if (FALSE_IT(guard.click("init_memory_dump_timer_")) || OB_FAIL(init_network_(addr, is_bootstrap))) {
    SERVER_LOG(WARN, "init_network failed", K(ret), K(addr));
  } else if (FALSE_IT(guard.click("init_network_")) || OB_FAIL(init_io_(cluster_name))) {
    SERVER_LOG(WARN, "init_io failed", K(ret), K(addr));
  } else if (FALSE_IT(guard.click("init_io_")) || OB_FAIL(init_palf_env_())) {
    SERVER_LOG(WARN, "init_palf_env failed", K(ret), K(addr));
  } else {
    guard.click("init_palf_env_");
    deliver_.set_palf_env_impl(palf_env_->get_palf_env_impl());
    SERVER_LOG(INFO, "simple_log_server simple_init success", K(node_id), K(addr), K(guard));
  }
  return ret;
}

int ObSimpleLogServer::init_memory_dump_timer_()
{
  int ret = OB_SUCCESS;
  common::ObFunction<bool()> print_memory_info = [=](){
      ObMallocAllocator::get_instance()->print_tenant_memory_usage(node_id_);
      ObMallocAllocator::get_instance()->print_tenant_ctx_memory_usage(node_id_);
      ObMallocAllocator::get_instance()->print_tenant_memory_usage(OB_SERVER_TENANT_ID);
      ObMallocAllocator::get_instance()->print_tenant_ctx_memory_usage(OB_SERVER_TENANT_ID);
      return false;};
  if (OB_FAIL(timer_.init_and_start(1, 1_s, "test timer"))) {
    SERVER_LOG(ERROR, "ObOccamTimer init failed", K(ret), K_(node_id));
  } else if (OB_FAIL(timer_.schedule_task_repeat(timer_handle_, 1_s, print_memory_info))) {
    SERVER_LOG(ERROR, "schedule_task failed", K(ret), K_(node_id));
  }
  return ret;
}

int ObSimpleLogServer::init_network_(const common::ObAddr &addr, const bool is_bootstrap = false)
{
  int ret = OB_SUCCESS;
  ObNetOptions opts;
  opts.rpc_io_cnt_ = 1024;
  opts.tcp_user_timeout_ = 10 * 1000 * 1000; // 10s
  addr_ = addr;
  if (is_bootstrap && OB_FAIL(net_.init(opts))) {
    SERVER_LOG(ERROR, "net init fail", K(ret));
  } else if (OB_FAIL(deliver_.init())) {
    SERVER_LOG(ERROR, "deliver_ init failed", K(ret));
  } else if (is_bootstrap && OB_FAIL(net_.add_rpc_listen(addr_.get_port(), handler_, transport_))) {
    SERVER_LOG(ERROR, "net_ listen failed", K(ret));
  }
  return ret;
}

int ObSimpleLogServer::init_io_(const std::string &cluster_name)
{
  int ret = OB_SUCCESS;
  ObTimeGuard guard("init_io_", 0);
  const std::string logserver_dir = cluster_name + "/port_" + std::to_string(addr_.get_port());
  std::string data_dir = logserver_dir;
  std::string clog_dir = logserver_dir + "/clog";
  std::string slog_dir = logserver_dir + "/slog";
  std::string sstable_dir = logserver_dir + "/sstable";
  std::vector<std::string> dirs{data_dir, slog_dir, clog_dir, sstable_dir};

  for (auto &dir : dirs) {
    if (-1 == mkdir(dir.c_str(), 0777)) {
      if (errno == EEXIST) {
        ret = OB_SUCCESS;
        SERVER_LOG(INFO, "for restart");
      } else {
        SERVER_LOG(WARN, "mkdir failed", K(errno));
        ret = OB_IO_ERROR;
        break;
      }
    }
  }
  guard.click("mkdir");
  if (OB_SUCC(ret)) {
    io_device_ = OB_NEW(ObLocalDevice, "TestBase");

    blocksstable::ObStorageEnv storage_env;
    storage_env.data_dir_ = data_dir.c_str();
    storage_env.sstable_dir_ = sstable_dir.c_str();
    storage_env.default_block_size_ = OB_DEFAULT_MACRO_BLOCK_SIZE;
    storage_env.data_disk_size_ = 1024 * 1024 * 1024;
    storage_env.data_disk_percentage_ = 0;
    storage_env.log_disk_size_ = 2LL * 1024 * 1024 * 1024;
    storage_env.log_disk_percentage_ = 0;

    storage_env.log_spec_.log_dir_ = slog_dir.c_str();
    storage_env.log_spec_.max_log_file_size_ = ObLogConstants::MAX_LOG_FILE_SIZE;
    storage_env.clog_dir_ = clog_dir.c_str();
    iod_opts_.opts_ = iod_opt_array_;
    iod_opt_array_[0].set("data_dir", storage_env.data_dir_);
    iod_opt_array_[1].set("sstable_dir", storage_env.sstable_dir_);
    iod_opt_array_[2].set("block_size", reinterpret_cast<const char*>(&storage_env.default_block_size_));
    iod_opt_array_[3].set("datafile_disk_percentage", storage_env.data_disk_percentage_);
    iod_opt_array_[4].set("datafile_size", storage_env.data_disk_size_);
    iod_opts_.opt_cnt_ = MAX_IOD_OPT_CNT;
    const int64_t DEFATULT_RESERVED_SIZE = 10 * 1024 * 1024 * 1024ul;
    if (OB_FAIL(io_device_->init(iod_opts_))) {
      SERVER_LOG(ERROR, "init io device fail", K(ret));
    } else if (OB_FAIL(log_block_pool_.init(storage_env.clog_dir_))) {
      SERVER_LOG(ERROR, "init log pool fail", K(ret));
    } else {
      guard.click("init io_device");
      clog_dir_ = clog_dir;
      SERVER_LOG(INFO, "init_io_ successs", K(ret), K(guard));
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(log_block_pool_.reserve(storage_env.log_disk_size_))) {
        LOG_ERROR("log pool start failed", KR(ret));
      }
    }
  }
  return ret;
}

int ObSimpleLogServer::init_palf_env_()
{
  int ret = OB_SUCCESS;
  palf::PalfDiskOptions opts;
  opts.log_disk_usage_limit_size_ = 10 * 1024 * 1024 * 1024ul;
  opts.log_disk_utilization_threshold_ = 80;
  opts.log_disk_utilization_limit_threshold_ = 95;
  allocator_ = OB_NEW(ObTenantMutilAllocator, "TestBase", node_id_);
  ObMemAttr attr(1, "SimpleLog");
  malloc_mgr_.set_attr(attr);
  std::string clog_dir = clog_dir_ + "/tenant_1";
  bool result = false;
  if (OB_FAIL(FileDirectoryUtils::is_exists(clog_dir.c_str(), result))) {
    SERVER_LOG(ERROR, "is_exists failed", K(ret), K(clog_dir.c_str()));
  } else if (false == result && -1 == ::mkdir(clog_dir.c_str(), 0777))  {
    ret = convert_sys_errno();
    SERVER_LOG(ERROR, "::mkdir failed", K(ret), K(clog_dir.c_str()));
  } else if (OB_FAIL(PalfEnv::create_palf_env(opts, clog_dir.c_str(), addr_,
      transport_, allocator_, &log_block_pool_, palf_env_))) {
    SERVER_LOG(ERROR, "init init_palf_env_ fail", K(ret));
  } else {
    SERVER_LOG(INFO, "init init_palf_env_ success", K(ret));
  }
  return ret;
}

int ObSimpleLogServer::simple_start(const bool is_bootstrap = false)
{
  int ret = OB_SUCCESS;
  if (is_bootstrap && OB_FAIL(net_.start())) {
    SERVER_LOG(ERROR, "net start fail", K(ret));
  } else if (OB_FAIL(deliver_.start())) {
    SERVER_LOG(ERROR, "deliver_ start failed", K(ret));
  }
  return ret;
}

int ObSimpleLogServer::simple_close(const bool is_shutdown = false)
{
  int ret = OB_SUCCESS;
  ObTimeGuard guard("simple_close", 0);
  deliver_.destroy();
  guard.click("destroy");

  PalfEnv::destroy_palf_env(palf_env_);
  log_block_pool_.destroy();
  guard.click("destroy_palf_env");

  if (is_shutdown) {
    net_.rpc_shutdown();
    net_.stop();
    net_.wait();
    net_.destroy();

    timer_handle_.stop_and_wait();
    timer_.stop_and_wait();
  }
  SERVER_LOG(INFO, "stop LogService success", K(ret), K(is_shutdown), K(guard));
  return ret;
}

int ObSimpleLogServer::simple_restart(const std::string &cluster_name, const int64_t node_idx)
{
  int ret = OB_SUCCESS;
  ObTimeGuard guard("simple_restart", 0);
  if (OB_FAIL(simple_close())) {
    SERVER_LOG(ERROR, "simple_close failed", K(ret));
  } else if (FALSE_IT(guard.click("simple_close")) || OB_FAIL(simple_init(cluster_name, addr_, node_idx))) {
    SERVER_LOG(ERROR, "simple_init failed", K(ret));
  } else if (FALSE_IT(guard.click("simple_init")) || OB_FAIL(simple_start())) {
    SERVER_LOG(ERROR, "simple_start failed", K(ret));
  } else {
    guard.click("simple_start");
    SERVER_LOG(INFO, "simple_restart success", K(ret), K(guard));
  }
  return ret;
}

void ObLogDeliver::block_net(const ObAddr &src)
{
  blacklist_.set_refactored(src);
  SERVER_LOG(INFO, "block_net", K(src), K(blacklist_));
}

void ObLogDeliver::unblock_net(const ObAddr &src)
{
  blacklist_.erase_refactored(src);
  SERVER_LOG(INFO, "unblock_net", K(src), K(blacklist_));
}

void ObLogDeliver::set_rpc_loss(const ObAddr &src, const int loss_rate)
{
  // not thread safe
  int old_loss_rate = -1;
  bool is_exist = false;
  for (int64_t i = 0; i < rpc_loss_config_.count(); ++i) {
    LossConfig &tmp_config = rpc_loss_config_[i];
    if (tmp_config.src_ == src) {
      is_exist = true;
      // src exists, update its loss_rate
      old_loss_rate = tmp_config.loss_rate_;
      tmp_config.loss_rate_ = loss_rate;
    }
  }
  if (!is_exist) {
    LossConfig loss_config(src, loss_rate);
    rpc_loss_config_.push_back(loss_config);
  }
  SERVER_LOG(INFO, "set_rpc_loss", K(src), K(loss_rate), K(old_loss_rate));
}

void ObLogDeliver::reset_rpc_loss(const ObAddr &src)
{
  // not thread safe
  int64_t idx = -1;
  for (int64_t i = 0; i < rpc_loss_config_.count(); ++i) {
    LossConfig tmp_config;
    int ret = rpc_loss_config_.at(i, tmp_config);
    if (OB_SUCCESS != ret) {
      SERVER_LOG(WARN, "at failed", K(ret), K(i), "count", rpc_loss_config_.count());
    } else if (tmp_config.src_ == src) {
      // src exists, update its loss_rate
      idx = i;
    }
  }
  if (idx >= 0) {
    rpc_loss_config_.remove(idx);
  }
  SERVER_LOG(INFO, "reset_rpc_loss", K(src), K(idx));
}

void ObLogDeliver::get_loss_config(const ObAddr &src, bool &exist, LossConfig &loss_config)
{
  // not thread safe
  exist = false;
  for (int64_t i = 0; i < rpc_loss_config_.count(); ++i) {
    LossConfig tmp_config;
    int ret = rpc_loss_config_.at(i, tmp_config);
    if (OB_SUCCESS != ret) {
      SERVER_LOG(WARN, "at failed", K(ret));
    } else if (tmp_config.src_ == src) {
      exist = true;
      loss_config = tmp_config;
    }
  }
  SERVER_LOG(INFO, "get_loss_config", K(src), K(loss_config));
}


int ObLogDeliver::init()
{
  int ret = OB_SUCCESS;
  // init_all_propocessor_();
  if (OB_FAIL(blacklist_.create(1024))) {
    SERVER_LOG(WARN, "create blacklist_ failed", K(ret));
  } else if (false == blacklist_.created()) {
    SERVER_LOG(WARN, "blacklist_ created failed");
  } else if (OB_FAIL(TG_CREATE_TENANT(lib::TGDefIDs::TEST2, tg_id_))) {
    SERVER_LOG(WARN, "ObSimpleThreadPool::init failed", K(ret));
  } else {
    is_inited_ = true;
    SERVER_LOG(INFO, "ObLogDeliver init success", KP(&blacklist_));
  }
  return ret;
}

void ObLogDeliver::destroy()
{
  if (IS_NOT_INIT) {
    SERVER_LOG(INFO, "ObLogDeliver not init");
  } else {
    RWLock::WLockGuard guard(lock_);
    stop();
    wait();
    is_inited_ = false;
    TG_DESTROY(tg_id_);
    blacklist_.destroy();
    tg_id_ = 0;
    SERVER_LOG(INFO, "destroy ObLogDeliver");
  }
}

int ObLogDeliver::start()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    SERVER_LOG(ERROR, "ObLogDeliver not init");
  } else if (OB_FAIL(TG_SET_HANDLER_AND_START(tg_id_, *this))) {
    SERVER_LOG(ERROR, "start ObLogDeliver failed");
  } else {
    is_stopped_ = false;
    SERVER_LOG(INFO, "start ObLogDeliver success");
  }
  return ret;
}

void ObLogDeliver::stop()
{
  if (IS_NOT_INIT) {
    SERVER_LOG(WARN, "ObLogDeliver stop failed");
  } else {
    is_stopped_ = true;
    TG_STOP(tg_id_);
    SERVER_LOG(INFO, "stop ObLogDeliver success");
  }
}

int ObLogDeliver::wait()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    SERVER_LOG(ERROR, "ObLogDeliver not inited!!!", K(ret));
  } else {
    TG_WAIT(tg_id_);
    SERVER_LOG(INFO, "wait ObLogDeliver success", K(tg_id_));
  }
  return ret;
}

bool ObLogDeliver::need_filter_packet_by_blacklist(const ObAddr &addr)
{
  return OB_HASH_EXIST == blacklist_.exist_refactored(addr);
}

bool ObLogDeliver::need_drop_by_loss_config(const ObAddr &addr)
{
  bool bool_ret = false;
  bool exist = false;
  LossConfig loss_config;
  get_loss_config(addr, exist, loss_config);
  if (exist && loss_config.loss_rate_ > 0) {
    const int64_t random_num = ObRandom::rand(1, 100);
    bool_ret = random_num <= loss_config.loss_rate_ ? true : false;
    if (bool_ret) {
      SERVER_LOG(INFO, "need drop req by loss config", K(addr));
    }
  }
  return bool_ret;
}

int ObLogDeliver::deliver(rpc::ObRequest &req)
{
  int ret = OB_SUCCESS;
  const ObRpcPacket &pkt = dynamic_cast<const ObRpcPacket&>(req.get_packet());
  SERVER_LOG(INFO, "deliver request", K(pkt), K(req.ez_req_), KPC(palf_env_impl_));
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else {
    RWLock::RLockGuard guard(lock_);
    do {
      ret = TG_PUSH_TASK(tg_id_, &req);
      if (OB_SUCCESS != ret && OB_EAGAIN != ret) {
        SERVER_LOG(ERROR, "deliver request failed", K(ret), K(pkt), K(req.ez_req_), KPC(palf_env_impl_));
      }
    } while (OB_EAGAIN == ret);
  }
  return ret;
}

void ObLogDeliver::handle(void *task)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    SERVER_LOG(WARN, "ObLogDeliver not init", K(ret));
  } else if (OB_ISNULL(task)) {
    SERVER_LOG(WARN, "invalid argument", KP(task));
  } else {
    rpc::ObRequest *req = static_cast<rpc::ObRequest *>(task);
    SERVER_LOG(INFO, "handle request", KP(req->ez_req_));
    (void) handle_req_(*req);
  }
}

int ObLogDeliver::handle_req_(rpc::ObRequest &req)
{
  int ret = OB_SUCCESS;
  const ObRpcPacket &pkt = dynamic_cast<const ObRpcPacket&>(req.get_packet());
  const ObRpcPacketCode pcode = pkt.get_pcode();
  ObFunction<bool(const ObAddr &src)> filter = [&](const ObAddr &src) -> bool {
    if (this->need_filter_packet_by_blacklist(src)) {
      SERVER_LOG(WARN, "need_filter_packet_by_blacklist", K(ret), K(pcode), K(src), K(blacklist_), KPC(palf_env_impl_));
      return true;
    } else if (this->need_drop_by_loss_config(src)) {
      SERVER_LOG(WARN, "need_drop_by_loss_config", K(ret), K(pcode), K(src), K(rpc_loss_config_), KPC(palf_env_impl_));
      return true;
    }
    return false;
  };

  switch (pkt.get_pcode()) {
    #define PROCESS(processer) \
    processer p;\
    p.init();\
    p.set_ob_request(req);\
    p.set_palf_env_impl(palf_env_impl_, &filter);\
    p.run();      \
    break;
    case obrpc::OB_LOG_PUSH_REQ: {
      PROCESS(LogPushReqP);
    }
    case obrpc::OB_LOG_PUSH_RESP: {
      PROCESS(LogPushRespP);
    }
    case obrpc::OB_LOG_FETCH_REQ: {
      PROCESS(LogFetchReqP);
    }
    case obrpc::OB_LOG_PREPARE_REQ: {
      PROCESS(LogPrepareReqP);
    }
    case obrpc::OB_LOG_PREPARE_RESP: {
      PROCESS(LogPrepareRespP);
    }
    case obrpc::OB_LOG_CHANGE_CONFIG_META_REQ: {
      PROCESS(LogChangeConfigMetaReqP);
    }
    case obrpc::OB_LOG_CHANGE_CONFIG_META_RESP: {
      PROCESS(LogChangeConfigMetaRespP);
    }
    case obrpc::OB_LOG_CHANGE_MODE_META_REQ: {
      PROCESS(LogChangeModeMetaReqP);
    }
    case obrpc::OB_LOG_CHANGE_MODE_META_RESP: {
      PROCESS(LogChangeModeMetaRespP);
    }
    case obrpc::OB_LOG_NOTIFY_REBUILD_REQ: {
      PROCESS(LogNotifyRebuildReqP)
    }
    case obrpc::OB_LOG_COMMITTED_INFO: {
      PROCESS(CommittedInfoP)
    }
    case obrpc::OB_LOG_LEARNER_REQ: {
      PROCESS(LogLearnerReqP)
    }
    case obrpc::OB_LOG_REGISTER_PARENT_REQ: {
      PROCESS(LogRegisterParentReqP)
    }
    case obrpc::OB_LOG_REGISTER_PARENT_RESP: {
      PROCESS(LogRegisterParentRespP)
    }
    case obrpc::OB_LOG_ELECTION_PREPARE_REQUEST : {
      PROCESS(ElectionPrepareRequestMsgP)
    }
    case obrpc::OB_LOG_ELECTION_PREPARE_RESPONSE : {
      PROCESS(ElectionPrepareResponseMsgP)
    }
    case obrpc::OB_LOG_ELECTION_ACCEPT_REQUEST : {
      PROCESS(ElectionAcceptRequestMsgP)
    }
    case obrpc::OB_LOG_ELECTION_ACCEPT_RESPONSE : {
      PROCESS(ElectionAcceptResponseMsgP)
    }
    case obrpc::OB_LOG_ELECTION_CHANGE_LEADER_REQUEST : {
      PROCESS(ElectionChangeLeaderMsgP)
    }
    case obrpc::OB_LOG_GET_MC_ST: {
      PROCESS(LogGetMCStP)
    }
    default:
      SERVER_LOG(ERROR, "invalid req type", K(pkt.get_pcode()));
      break;
  }
  return ret;
}

} // unittest
} // oceanbase
