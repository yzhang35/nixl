/*
 * Copyright (c) 2026 BAAI. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "flagcx_backend.h"
#include "serdes/serdes.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>

// Parse connection string in format: ip_addr:port?gpu_index?notif_port
// FlagCX metadata format includes an extra notif_port field compared to UCCL.
// We only need ip, port, and gpu_index for the connect call.
bool
parseConnectionString(const std::string &conn_str,
                      std::unique_ptr<char[]> &ip_addr,
                      int &port,
                      int &gpu_index) {
    size_t colon_pos = conn_str.find(':');
    if (colon_pos == std::string::npos) {
        NIXL_ERROR << "Invalid connection string format: missing colon separator";
        return false;
    }
    size_t question_pos = conn_str.find('?', colon_pos);
    if (question_pos == std::string::npos) {
        NIXL_ERROR << "Invalid connection string format: missing question mark separator";
        return false;
    }

    std::string ip_str = conn_str.substr(0, colon_pos);
    ip_addr = std::make_unique<char[]>(ip_str.length() + 1);
    strcpy(ip_addr.get(), ip_str.c_str());

    std::string port_str = conn_str.substr(colon_pos + 1, question_pos - colon_pos - 1);
    try {
        port = std::stoi(port_str);
    }
    catch (const std::exception &e) {
        NIXL_ERROR << "Invalid port number: " << port_str;
        return false;
    }

    // gpu_index is the second ?-separated field; ignore any trailing fields (notif_port)
    std::string rest = conn_str.substr(question_pos + 1);
    size_t next_question = rest.find('?');
    std::string gpu_str = (next_question != std::string::npos) ? rest.substr(0, next_question) : rest;
    try {
        gpu_index = std::stoi(gpu_str);
    }
    catch (const std::exception &e) {
        NIXL_ERROR << "Invalid GPU index: " << gpu_str;
        return false;
    }

    return true;
}

int
getNixlParam(const nixl_b_params_t *custom_params, const std::string &key, int default_value) {
    if (!custom_params) {
        return default_value;
    }

    auto it = custom_params->find(key);
    if (it == custom_params->end()) {
        return default_value;
    }

    try {
        return std::stoi(it->second);
    }
    catch (const std::exception &) {
        return default_value;
    }
}

nixlFlagcxEngine::nixlFlagcxEngine(const nixlBackendInitParams *init_params)
    : nixlBackendEngine(init_params),
      stop_listener_(false) {

    local_agent_name_ = init_params->localAgent;
    nixl_b_params_t *custom_params = init_params->customParams;

    size_t num_cpus = getNixlParam(custom_params, "num_cpus", 4);
    int in_python = getNixlParam(custom_params, "in_python", 1);
    engine_ = flagcxP2pEngineCreate(num_cpus, (in_python == 1));
    NIXL_DEBUG << "FlagCX P2P engine created";

    listener_thread_ = std::thread(&nixlFlagcxEngine::startListener, this);
}

nixlFlagcxEngine::~nixlFlagcxEngine() {
    stop_listener_ = true;

    if (engine_) {
        flagcxP2pEngineStopAccept(engine_);
    }

    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(mem_mutex_);
        for (auto &[addr, priv] : mem_reg_info_) {
            if (priv && priv->mr_id != 0) {
                flagcxP2pEngineMrDestroy(engine_, priv->mr_id);
            }
            delete priv;
        }
        mem_reg_info_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        std::set<std::string> destroyed_agents;
        for (auto &[agent_name, conn_id] : connected_agents_) {
            if (destroyed_agents.find(agent_name) == destroyed_agents.end()) {
                FlagcxP2pConn *conn = reinterpret_cast<FlagcxP2pConn *>(conn_id);
                if (conn) {
                    flagcxP2pEngineConnDestroy(conn);
                    destroyed_agents.insert(agent_name);
                }
            }
        }
        connected_agents_.clear();
    }

    if (engine_) {
        flagcxP2pEngineDestroy(engine_);
        engine_ = nullptr;
    }
}

void
nixlFlagcxEngine::startListener() {
    NIXL_DEBUG << "FlagCX accepting connections";
    while (!stop_listener_) {

        char ip_buf[256];
        int remote_gpu_idx;
        FlagcxP2pConn *conn = flagcxP2pEngineAccept(engine_, ip_buf, sizeof(ip_buf), &remote_gpu_idx);
        if (!conn) {
            if (stop_listener_) {
                NIXL_DEBUG << "Listener thread stopping";
                break;
            }
            NIXL_ERROR << "Failed to accept connection from remote agent";
            continue;
        }
        flagcxP2pEngineStartListener(conn);
        NIXL_DEBUG << "Connected to remote agent: " << ip_buf;
        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            connected_agents_[ip_buf] = reinterpret_cast<uint64_t>(conn);
        }
    }
}

nixl_mem_list_t
nixlFlagcxEngine::getSupportedMems() const {
    nixl_mem_list_t mems;
    mems.push_back(DRAM_SEG);
    mems.push_back(VRAM_SEG);

    return mems;
}

nixl_status_t
nixlFlagcxEngine::getPublicData(const nixlBackendMD *meta, std::string &str) const {
    nixlFlagcxBackendMD *priv = (nixlFlagcxBackendMD *)meta;

    // Export desc_buf as hex string
    str.clear();
    str.reserve(FLAGCX_P2P_DESC_SIZE * 2);
    for (int i = 0; i < FLAGCX_P2P_DESC_SIZE; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", static_cast<unsigned char>(priv->desc_buf[i]));
        str += hex;
    }

    return NIXL_SUCCESS;
}

nixl_status_t
nixlFlagcxEngine::getConnInfo(std::string &str) const {
    if (!engine_) {
        return NIXL_ERR_BACKEND;
    }

    char *metadata = nullptr;
    int result = flagcxP2pEngineGetMetadata(engine_, &metadata);
    if (result != 0 || !metadata) {
        return NIXL_ERR_BACKEND;
    }

    str = std::string(metadata);
    delete[] metadata;
    NIXL_DEBUG << "FlagCX engine metadata: " << str;

    return NIXL_SUCCESS;
}

nixl_status_t
nixlFlagcxEngine::loadRemoteConnInfo(const std::string &remote_agent,
                                     const std::string &remote_conn_info) {
    NIXL_DEBUG << "FlagCX engine remote_agent: " << remote_agent
               << " loadRemoteConnInfo: " << remote_conn_info;
    std::lock_guard<std::mutex> lock(conn_mutex_);

    std::unique_ptr<char[]> ip_addr;
    int port = 0;
    int gpu_index = 0;

    if (!parseConnectionString(remote_conn_info, ip_addr, port, gpu_index)) {
        return NIXL_ERR_BACKEND;
    }

    FlagcxP2pConn *conn = nullptr;

    NIXL_DEBUG << "Connecting to " << ip_addr.get() << ":" << port << "?gpu=" << gpu_index
               << std::endl;
    conn = flagcxP2pEngineConnect(engine_, ip_addr.get(), gpu_index, port);
    if (!conn) {
        NIXL_ERROR << "Failed to connect to remote agent " << remote_agent;
        return NIXL_ERR_BACKEND;
    }

    NIXL_DEBUG << "Successfully connected to remote agent " << remote_agent;
    flagcxP2pEngineStartListener(conn);

    connected_agents_[remote_agent] = reinterpret_cast<uint64_t>(conn);

    return NIXL_SUCCESS;
}

nixl_status_t
nixlFlagcxEngine::connect(const std::string &remote_agent) {
    return NIXL_SUCCESS;
}

nixl_status_t
nixlFlagcxEngine::disconnect(const std::string &remote_agent) {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto conn_iter = connected_agents_.find(remote_agent);
    if (conn_iter == connected_agents_.end()) {
        NIXL_ERROR << "No connection found for remote agent: " << remote_agent;
        return NIXL_ERR_BACKEND;
    }
    FlagcxP2pConn *conn = reinterpret_cast<FlagcxP2pConn *>(conn_iter->second);
    if (!conn) {
        NIXL_ERROR << "Invalid connection for remote agent: " << remote_agent;
        return NIXL_ERR_BACKEND;
    }

    if (conn) {
        NIXL_DEBUG << "Disconnecting from agent: " << remote_agent;
        flagcxP2pEngineConnDestroy(conn);
        connected_agents_.erase(remote_agent);
    }

    return NIXL_SUCCESS;
}

nixl_status_t
nixlFlagcxEngine::registerMem(const nixlBlobDesc &mem,
                              const nixl_mem_t &nixl_mem,
                              nixlBackendMD *&out) {
    std::lock_guard<std::mutex> lock(mem_mutex_);

    if (mem_reg_info_.count(mem.addr)) {
        auto priv = mem_reg_info_[mem.addr];
        NIXL_DEBUG << "Registering memory: " << std::hex << mem.addr << ", len: " << std::dec
                   << mem.len;
        priv->ref_cnt++;
        out = priv;
        return NIXL_SUCCESS;
    }

    FlagcxP2pMr mr;
    int result = flagcxP2pEngineReg(engine_, mem.addr, mem.len, mr);
    if (result != 0) {
        NIXL_ERROR << "Failed to register memory with FlagCX engine";
        return NIXL_ERR_BACKEND;
    }

    auto priv = new nixlFlagcxBackendMD(true);
    priv->addr = (void *)mem.addr;
    priv->length = mem.len;
    priv->ref_cnt = 1;
    priv->mr_id = mr;

    // Pre-compute RDMA descriptor for one-sided operations
    result = flagcxP2pEnginePrepareDesc(engine_, mr, (void *)mem.addr, mem.len, priv->desc_buf);
    if (result != 0) {
        NIXL_ERROR << "Failed to prepare descriptor for memory region";
        flagcxP2pEngineMrDestroy(engine_, mr);
        delete priv;
        return NIXL_ERR_BACKEND;
    }

    out = priv;
    mem_reg_info_[mem.addr] = priv;
    NIXL_DEBUG << "Registering memory: " << std::hex << mem.addr << " Device: " << mem.devId
               << " ref_cnt: " << priv->ref_cnt << " mr_id: " << priv->mr_id;

    return NIXL_SUCCESS;
}

nixl_status_t
nixlFlagcxEngine::deregisterMem(nixlBackendMD *meta) {
    std::lock_guard<std::mutex> lock(mem_mutex_);
    auto priv = static_cast<nixlFlagcxBackendMD *>(meta);
    priv->ref_cnt--;
    if (priv->ref_cnt > 0) return NIXL_SUCCESS;

    flagcxP2pEngineMrDestroy(engine_, priv->mr_id);
    NIXL_DEBUG << "Deregistered memory: " << std::hex << priv->addr << " mr_id: " << priv->mr_id;

    mem_reg_info_.erase((uint64_t)priv->addr);
    delete priv;
    return NIXL_SUCCESS;
}

nixl_status_t
nixlFlagcxEngine::loadLocalMD(nixlBackendMD *input, nixlBackendMD *&output) {
    nixlFlagcxBackendMD *input_md = (nixlFlagcxBackendMD *)input;
    NIXL_DEBUG << "FlagCX Load Local MD: " << std::hex << input_md->addr
               << "Meta Info:" << input_md->mr_id;

    nixlFlagcxBackendMD *output_md = (nixlFlagcxBackendMD *)output;
    output_md->addr = (void *)input_md->addr;
    output_md->length = input_md->length;
    output_md->ref_cnt = 1;
    output_md->mr_id = reinterpret_cast<uint64_t>(input_md->mr_id);

    return NIXL_SUCCESS;
}

nixl_status_t
nixlFlagcxEngine::loadRemoteMD(const nixlBlobDesc &input,
                               const nixl_mem_t &nixl_mem,
                               const std::string &remote_agent,
                               nixlBackendMD *&output) {
    NIXL_DEBUG << "FlagCX Load Remote MD: " << std::hex << input.addr
               << " Meta Info:" << input.metaInfo << " remote_agent: " << remote_agent;

    output = new nixlFlagcxBackendMD(true);
    nixlFlagcxBackendMD *output_md = static_cast<nixlFlagcxBackendMD *>(output);
    output_md->addr = (void *)input.addr;
    output_md->length = input.len;
    output_md->ref_cnt = 1;

    // Decode RDMA descriptor from hex string
    const std::string &hex_str = input.metaInfo;

    if (hex_str.length() == FLAGCX_P2P_DESC_SIZE * 2) {
        for (int i = 0; i < FLAGCX_P2P_DESC_SIZE; i++) {
            std::string byte_str = hex_str.substr(i * 2, 2);
            output_md->desc_buf[i] = static_cast<char>(strtoul(byte_str.c_str(), NULL, 16));
        }
    } else {
        NIXL_ERROR << "Invalid descriptor hex string length: " << hex_str.length() << " (expected "
                   << FLAGCX_P2P_DESC_SIZE * 2 << ")";
        delete output_md;
        output = nullptr;
        return NIXL_ERR_INVALID_PARAM;
    }

    return NIXL_SUCCESS;
}

nixl_status_t
nixlFlagcxEngine::unloadMD(nixlBackendMD *input) {
    nixlFlagcxBackendMD *md = (nixlFlagcxBackendMD *)input;
    delete md;

    return NIXL_SUCCESS;
}

nixl_status_t
nixlFlagcxEngine::prepXfer(const nixl_xfer_op_t &operation,
                           const nixl_meta_dlist_t &local,
                           const nixl_meta_dlist_t &remote,
                           const std::string &remote_agent,
                           nixlBackendReqH *&handle,
                           const nixl_opt_b_args_t *opt_args) const {
    nixlFlagcxBackendMD *lmd;
    nixlFlagcxBackendMD *rmd;
    handle = nullptr;

    NIXL_DEBUG << "FlagCX PrepXfer: " << operation << " remote_agent: " << remote_agent;

    FlagcxP2pConn *conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        auto conn_iter = connected_agents_.find(remote_agent);
        if (conn_iter == connected_agents_.end()) {
            NIXL_ERROR << "No connection found for remote agent: " << remote_agent;
            return NIXL_ERR_BACKEND;
        }
        conn = reinterpret_cast<FlagcxP2pConn *>(conn_iter->second);
        if (!conn) {
            NIXL_ERROR << "Invalid connection for remote agent: " << remote_agent;
            return NIXL_ERR_BACKEND;
        }
    }

    size_t lcnt = local.descCount();
    size_t rcnt = remote.descCount();

    if (lcnt != rcnt) {
        NIXL_ERROR << "Local and remote descriptor counts don't match: " << lcnt << " != " << rcnt;
        return NIXL_ERR_INVALID_PARAM;
    }

    handle = new nixlFlagcxReqH(conn);
    nixlFlagcxReqH *flagcx_handle = static_cast<nixlFlagcxReqH *>(handle);

    flagcx_handle->rdma_descs.resize(lcnt);

    std::lock_guard<std::mutex> lock(mem_mutex_);
    for (size_t i = 0; i < lcnt; i++) {
        lmd = (nixlFlagcxBackendMD *)local[i].metadataP;
        rmd = (nixlFlagcxBackendMD *)remote[i].metadataP;
        size_t rsize = remote[i].len;
        uintptr_t remote_addr = remote[i].addr;

        auto local_mem_iter = mem_reg_info_.find((uint64_t)lmd->addr);
        if (local_mem_iter == mem_reg_info_.end()) {
            NIXL_ERROR << "Local memory not registered for address:" << std::hex << lmd->addr;
            return NIXL_ERR_BACKEND;
        }

        // Deserialize RDMA descriptor from char[] into FlagcxP2pRdmaDesc struct
        flagcxP2pDeserializeRdmaDesc(rmd->desc_buf, &flagcx_handle->rdma_descs[i]);

        flagcxP2pEngineUpdateDesc(flagcx_handle->rdma_descs[i], remote_addr, rsize);
    }

    return NIXL_SUCCESS;
}

nixl_status_t
nixlFlagcxEngine::postXfer(const nixl_xfer_op_t &operation,
                           const nixl_meta_dlist_t &local,
                           const nixl_meta_dlist_t &remote,
                           const std::string &remote_agent,
                           nixlBackendReqH *&handle,
                           const nixl_opt_b_args_t *opt_args) const {
    nixlFlagcxReqH *flagcx_handle;
    nixlFlagcxBackendMD *lmd;

    NIXL_DEBUG << "FlagCX PostXfer: " << operation << " remote_agent: " << remote_agent;

    FlagcxP2pConn *conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        auto conn_iter = connected_agents_.find(remote_agent);
        if (conn_iter == connected_agents_.end()) {
            NIXL_ERROR << "No connection found for remote agent: " << remote_agent;
            return NIXL_ERR_BACKEND;
        }

        conn = reinterpret_cast<FlagcxP2pConn *>(conn_iter->second);
        if (!conn) {
            NIXL_ERROR << "Invalid connection for remote agent: " << remote_agent;
            return NIXL_ERR_BACKEND;
        }
    }

    size_t lcnt = local.descCount();
    size_t rcnt = remote.descCount();

    if (lcnt != rcnt) {
        NIXL_ERROR << "Local and remote descriptor counts don't match: " << lcnt << " != " << rcnt;
        return NIXL_ERR_INVALID_PARAM;
    }

    std::vector<FlagcxP2pMr> mr_ids;
    std::vector<void *> addr_v;
    std::vector<size_t> size_v;

    std::lock_guard<std::mutex> lock(mem_mutex_);
    for (size_t i = 0; i < lcnt; i++) {
        lmd = (nixlFlagcxBackendMD *)local[i].metadataP;
        size_t lsize = local[i].len;
        size_t rsize = remote[i].len;
        uintptr_t local_addr = local[i].addr;

        if (lsize != rsize) {
            NIXL_ERROR << "Local and remote sizes don't match: " << lsize << " != " << rsize;
            return NIXL_ERR_INVALID_PARAM;
        }

        auto local_mem_iter = mem_reg_info_.find((uint64_t)lmd->addr);
        if (local_mem_iter == mem_reg_info_.end()) {
            NIXL_ERROR << "Local memory not registered for base address: " << std::hex << lmd->addr;
            return NIXL_ERR_BACKEND;
        }

        auto local_priv = local_mem_iter->second;

        mr_ids.push_back(local_priv->mr_id);
        addr_v.push_back((void *)local_addr);
        size_v.push_back(lsize);
    }

    int result = 0;
    uint64_t transfer_id = 0;
    flagcx_handle = static_cast<nixlFlagcxReqH *>(handle);

    switch (operation) {
    case NIXL_READ: {
        result = flagcxP2pEngineReadVector(
            conn, mr_ids, addr_v, size_v, flagcx_handle->rdma_descs, lcnt, &transfer_id);
        break;
    }
    case NIXL_WRITE: {
        result = flagcxP2pEngineWriteVector(
            conn, mr_ids, addr_v, size_v, flagcx_handle->rdma_descs, lcnt, &transfer_id);
        break;
    }
    default:
        NIXL_ERROR << "Unsupported operation type: " << operation;
        return NIXL_ERR_INVALID_PARAM;
    }

    if (result != 0) {
        NIXL_ERROR << "FlagCX operation failed with result: " << result;
        return NIXL_ERR_BACKEND;
    }

    if (!handle) {
        handle = new nixlFlagcxReqH(conn);
    }
    flagcx_handle->transfer_id = transfer_id;

    NIXL_DEBUG << "Successfully posted vector " << (operation == NIXL_READ ? "READ" : "WRITE")
               << " operation with " << lcnt << " iovecs, transfer_id: " << transfer_id;

    if (opt_args && opt_args->hasNotif) {
        flagcx_handle->notif_msg = opt_args->notifMsg;
    }

    return NIXL_IN_PROG;
}

nixl_status_t
nixlFlagcxEngine::checkXfer(nixlBackendReqH *handle) const {
    if (!handle) {
        NIXL_ERROR << "Invalid handle provided to checkXfer";
        return NIXL_ERR_INVALID_PARAM;
    }

    nixlFlagcxReqH *flagcx_handle = dynamic_cast<nixlFlagcxReqH *>(handle);
    if (!flagcx_handle) {
        NIXL_ERROR << "Invalid handle type for FlagCX backend";
        return NIXL_ERR_INVALID_PARAM;
    }

    FlagcxP2pConn *conn = flagcx_handle->conn;
    if (!conn) {
        NIXL_ERROR << "No connection found in handle";
        return NIXL_ERR_BACKEND;
    }

    bool is_done = flagcxP2pEngineXferStatus(conn, flagcx_handle->transfer_id);
    if (is_done) {
        nixlSerDes ser_des;
        ser_des.addStr("msg", flagcx_handle->notif_msg);
        std::string serialized = ser_des.exportStr();

        if (serialized.size() > sizeof(FlagcxP2pNotifyMsg::msg)) {
            NIXL_ERROR << "Notification message too large: " << serialized.size()
                       << " bytes, max: " << sizeof(FlagcxP2pNotifyMsg::msg) << " bytes";
        } else {
            FlagcxP2pNotifyMsg notify_msg = {};
            strncpy(notify_msg.name, local_agent_name_.c_str(), sizeof(notify_msg.name) - 1);
            memcpy(notify_msg.msg, serialized.c_str(), serialized.size());

            int result = flagcxP2pEngineSendNotif(conn, &notify_msg);
            if (result < 0) {
                NIXL_ERROR << "Failed to send notify message";
                return NIXL_ERR_BACKEND;
            }
            NIXL_DEBUG << "Transfer complete, sent notification: " << flagcx_handle->notif_msg;
        }
        return NIXL_SUCCESS;
    }

    return NIXL_IN_PROG;
}

nixl_status_t
nixlFlagcxEngine::releaseReqH(nixlBackendReqH *handle) const {
    if (!handle) {
        return NIXL_SUCCESS;
    }

    nixlFlagcxReqH *flagcx_handle = dynamic_cast<nixlFlagcxReqH *>(handle);
    if (flagcx_handle) {
        delete flagcx_handle;
    }

    return NIXL_SUCCESS;
}

nixl_status_t
nixlFlagcxEngine::getNotifs(notif_list_t &notif_list) {
    if (notif_list.size() != 0) return NIXL_ERR_INVALID_PARAM;

    std::vector<FlagcxP2pNotifyMsg> notify_msgs = flagcxP2pEngineGetNotifs();
    for (size_t i = 0; i < notify_msgs.size(); i++) {
        size_t msg_len = sizeof(notify_msgs[i].msg);
        std::string serialized_str(notify_msgs[i].msg, msg_len);
        nixlSerDes ser_des;
        nixl_status_t ret = ser_des.importStr(serialized_str);
        if (ret != NIXL_SUCCESS) {
            NIXL_ERROR << "Failed to deserialize notification message";
            continue;
        }
        std::string remote_name(notify_msgs[i].name);
        std::string msg = ser_des.getStr("msg");

        notif_list.push_back(std::make_pair(remote_name, msg));
    }

    return NIXL_SUCCESS;
}

nixl_status_t
nixlFlagcxEngine::genNotif(const std::string &remote_agent, const std::string &msg) const {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto conn_iter = connected_agents_.find(remote_agent);
    if (conn_iter == connected_agents_.end()) {
        NIXL_ERROR << "No connection found for remote agent: " << remote_agent;
        return NIXL_ERR_BACKEND;
    }

    FlagcxP2pConn *conn = reinterpret_cast<FlagcxP2pConn *>(conn_iter->second);
    if (!conn) {
        NIXL_ERROR << "Invalid connection for remote agent: " << remote_agent;
        return NIXL_ERR_BACKEND;
    }

    nixlSerDes ser_des;
    ser_des.addStr("msg", msg);
    std::string serialized = ser_des.exportStr();

    if (serialized.size() > sizeof(FlagcxP2pNotifyMsg::msg)) {
        NIXL_ERROR << "Notification message too large: " << serialized.size()
                   << " bytes, max: " << sizeof(FlagcxP2pNotifyMsg::msg) << " bytes";
        return NIXL_ERR_INVALID_PARAM;
    }

    FlagcxP2pNotifyMsg notify_msg;
    memset(&notify_msg, 0, sizeof(notify_msg));
    strncpy(notify_msg.name, local_agent_name_.c_str(), sizeof(notify_msg.name) - 1);
    memcpy(notify_msg.msg, serialized.c_str(), serialized.size());

    int result = flagcxP2pEngineSendNotif(conn, &notify_msg);
    if (result < 0) {
        NIXL_ERROR << "Failed to send notify message";
        return NIXL_ERR_BACKEND;
    }

    return NIXL_SUCCESS;
}
