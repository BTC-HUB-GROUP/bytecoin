// Copyright (c) 2012-2015, The CryptoNote developers, The Bytecoin developers
//
// This file is part of Bytecoin.
//
// Bytecoin is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Bytecoin is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with Bytecoin.  If not, see <http://www.gnu.org/licenses/>.

#include "NodeRpcProxy.h"
#include "NodeErrors.h"

#include <atomic>
#include <system_error>
#include <thread>

#include <HTTP/HttpRequest.h>
#include <HTTP/HttpResponse.h>
#include <System/Dispatcher.h>

#include "cryptonote_core/cryptonote_basic_impl.h"
#include "cryptonote_core/cryptonote_format_utils.h"
#include "rpc/core_rpc_server_commands_defs.h"
#include "rpc/HttpClient.h"
#include "rpc/JsonRpc.h"

namespace CryptoNote {

namespace {
std::error_code interpretResponseStatus(const std::string& status) {
  if (CORE_RPC_STATUS_BUSY == status) {
    return make_error_code(error::NODE_BUSY);
  } else if (CORE_RPC_STATUS_OK != status) {
    return make_error_code(error::INTERNAL_NODE_ERROR);
  }
  return std::error_code();
}

template <typename Request, typename Response>
std::error_code binaryCommand(HttpClient& client, const std::string& url, const Request& req, Response& res) {
  std::error_code ec;
  
  try {
    invokeBinaryCommand(client, url, req, res);
    ec = interpretResponseStatus(res.status);
  } catch (const std::exception&) {
    ec = make_error_code(error::NETWORK_ERROR);
  }

  return ec;
}

template <typename Request, typename Response>
std::error_code jsonCommand(HttpClient& client, const std::string& url, const Request& req, Response& res) {
  std::error_code ec;

  try {
    invokeJsonCommand(client, url, req, res);
    ec = interpretResponseStatus(res.status);
  } catch (const std::exception&) {
    ec = make_error_code(error::NETWORK_ERROR);
  }

  return ec;
}

template <typename Request, typename Response>
std::error_code jsonRpcCommand(HttpClient& client, const std::string& method, const Request& req, Response& res) {
  std::error_code ec = make_error_code(error::INTERNAL_NODE_ERROR);

  try {
    JsonRpc::JsonRpcRequest jsReq;

    jsReq.setMethod(method);
    jsReq.setParams(req);

    HttpRequest httpReq;
    HttpResponse httpRes;

    httpReq.setUrl("/json_rpc");
    httpReq.setBody(jsReq.getBody());

    client.request(httpReq, httpRes);

    JsonRpc::JsonRpcResponse jsRes;

    if (httpRes.getStatus() == HttpResponse::STATUS_200) {
      jsRes.parse(httpRes.getBody());
      if (jsRes.getResult(res)) {
        ec = interpretResponseStatus(res.status);
      }
    }
  } catch (const std::exception&) {
    ec = make_error_code(error::NETWORK_ERROR);
  }

  return ec;
}


}

NodeRpcProxy::NodeRpcProxy(const std::string& nodeHost, unsigned short nodePort) :
  m_rpcTimeout(10000),
  m_pullTimer(m_ioService),
  m_pullInterval(10000),
  m_nodeHost(nodeHost),
  m_nodePort(nodePort),
  m_lastLocalBlockTimestamp(0) {
  resetInternalState();
}

NodeRpcProxy::~NodeRpcProxy() {
  shutdown();
}

void NodeRpcProxy::resetInternalState() {
  m_ioService.reset();
  m_peerCount = 0;
  m_nodeHeight = 0;
  m_networkHeight = 0;
  m_lastKnowHash = CryptoNote::null_hash;
}

void NodeRpcProxy::init(const INode::Callback& callback) {
  if (!m_initState.beginInit()) {
    callback(make_error_code(error::ALREADY_INITIALIZED));
    return;
  }

  resetInternalState();
  m_workerThread = std::thread(std::bind(&NodeRpcProxy::workerThread, this, callback));
}

bool NodeRpcProxy::shutdown() {
  if (!m_initState.beginShutdown()) {
    return false;
  }

  boost::system::error_code ignored_ec;
  m_pullTimer.cancel(ignored_ec);
  m_ioService.stop();
  if (m_workerThread.joinable()) {
    m_workerThread.join();
    m_initState.endShutdown();
  }

  return true;
}

void NodeRpcProxy::workerThread(const INode::Callback& initialized_callback) {
  if (!m_initState.endInit()) {
    return;
  }

  System::Dispatcher dispatcher;
  HttpClient httpClient(dispatcher, m_nodeHost, m_nodePort);
  m_httpClient = &httpClient;

  initialized_callback(std::error_code());

  pullNodeStatusAndScheduleTheNext();

  while (!m_ioService.stopped()) {
    m_ioService.run_one();
  }
}

void NodeRpcProxy::pullNodeStatusAndScheduleTheNext() {
  updateNodeStatus();

  m_pullTimer.expires_from_now(boost::posix_time::milliseconds(m_pullInterval));
  m_pullTimer.async_wait([=](const boost::system::error_code& ec) {
    if (ec != boost::asio::error::operation_aborted) {
      pullNodeStatusAndScheduleTheNext();
    }
  });
}

void NodeRpcProxy::updateNodeStatus() {
  CryptoNote::COMMAND_RPC_GET_LAST_BLOCK_HEADER::request req = AUTO_VAL_INIT(req);
  CryptoNote::COMMAND_RPC_GET_LAST_BLOCK_HEADER::response rsp = AUTO_VAL_INIT(rsp);

  std::error_code ec = jsonRpcCommand(*m_httpClient, "getlastblockheader", req, rsp);

  if (!ec) {
    crypto::hash blockHash;
    if (!parse_hash256(rsp.block_header.hash, blockHash)) {
      return;
    }

    if (blockHash != m_lastKnowHash) {
      m_lastKnowHash = blockHash;
      m_nodeHeight = rsp.block_header.height;
      m_lastLocalBlockTimestamp = rsp.block_header.timestamp;
      // TODO request and update network height
      m_networkHeight = m_nodeHeight;
      m_observerManager.notify(&INodeObserver::lastKnownBlockHeightUpdated, m_networkHeight);
      //if (m_networkHeight != rsp.block_header.network_height) {
      //  m_networkHeight = rsp.block_header.network_height;
      //  m_observerManager.notify(&INodeObserver::lastKnownBlockHeightUpdated, m_networkHeight);
      //}
      m_observerManager.notify(&INodeObserver::localBlockchainUpdated, m_nodeHeight);
    }
  }

  updatePeerCount();
}

void NodeRpcProxy::updatePeerCount() {

  CryptoNote::COMMAND_RPC_GET_INFO::request req = AUTO_VAL_INIT(req);
  CryptoNote::COMMAND_RPC_GET_INFO::response rsp = AUTO_VAL_INIT(rsp);

  std::error_code ec = jsonCommand(*m_httpClient, "/getinfo", req, rsp);

  if (!ec) {
    size_t peerCount = rsp.incoming_connections_count + rsp.outgoing_connections_count;
    if (peerCount != m_peerCount) {
      m_peerCount = peerCount;
      m_observerManager.notify(&INodeObserver::peerCountUpdated, m_peerCount);
    }
  }
}

bool NodeRpcProxy::addObserver(INodeObserver* observer) {
  return m_observerManager.add(observer);
}

bool NodeRpcProxy::removeObserver(INodeObserver* observer) {
  return m_observerManager.remove(observer);
}

size_t NodeRpcProxy::getPeerCount() const {
  return m_peerCount;
}

uint64_t NodeRpcProxy::getLastLocalBlockHeight() const {
  return m_nodeHeight;
}

uint64_t NodeRpcProxy::getLastKnownBlockHeight() const {
  return m_networkHeight;
}

uint64_t NodeRpcProxy::getLocalBlockCount() const {
  return m_nodeHeight;
}

uint64_t NodeRpcProxy::getKnownBlockCount() const {
  return m_networkHeight;
}

uint64_t NodeRpcProxy::getLastLocalBlockTimestamp() const {
  return m_lastLocalBlockTimestamp;
}

void NodeRpcProxy::relayTransaction(const CryptoNote::Transaction& transaction, const Callback& callback) {
  if (!m_initState.initialized()) {
    callback(make_error_code(error::NOT_INITIALIZED));
    return;
  }

  // TODO: m_ioService.stop() won't inkove callback(aborted). Fix it
  m_ioService.post(std::bind(&NodeRpcProxy::doRelayTransaction, this, transaction, callback));
}

void NodeRpcProxy::getRandomOutsByAmounts(std::vector<uint64_t>&& amounts, uint64_t outsCount, std::vector<COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount>& outs, const Callback& callback) {
  if (!m_initState.initialized()) {
    callback(make_error_code(error::NOT_INITIALIZED));
    return;
  }

  m_ioService.post(std::bind(&NodeRpcProxy::doGetRandomOutsByAmounts, this, std::move(amounts), outsCount, std::ref(outs), callback));
}

void NodeRpcProxy::getNewBlocks(std::list<crypto::hash>&& knownBlockIds, std::list<CryptoNote::block_complete_entry>& newBlocks, uint64_t& startHeight, const Callback& callback) {
  if (!m_initState.initialized()) {
    callback(make_error_code(error::NOT_INITIALIZED));
    return;
  }

  m_ioService.post(std::bind(&NodeRpcProxy::doGetNewBlocks, this, std::move(knownBlockIds), std::ref(newBlocks), std::ref(startHeight), callback));
}

void NodeRpcProxy::getTransactionOutsGlobalIndices(const crypto::hash& transactionHash, std::vector<uint64_t>& outsGlobalIndices, const Callback& callback) {
  if (!m_initState.initialized()) {
    callback(make_error_code(error::NOT_INITIALIZED));
    return;
  }

  m_ioService.post(std::bind(&NodeRpcProxy::doGetTransactionOutsGlobalIndices, this, transactionHash, std::ref(outsGlobalIndices), callback));
}

void NodeRpcProxy::queryBlocks(std::list<crypto::hash>&& knownBlockIds, uint64_t timestamp, std::list<CryptoNote::BlockCompleteEntry>& newBlocks, uint64_t& startHeight, const Callback& callback) {
  if (!m_initState.initialized()) {
    callback(make_error_code(error::NOT_INITIALIZED));
    return;
  }

  m_ioService.post(std::bind(&NodeRpcProxy::doQueryBlocks, this, std::move(knownBlockIds), timestamp, std::ref(newBlocks), std::ref(startHeight), callback));
}

void NodeRpcProxy::doRelayTransaction(const CryptoNote::Transaction& transaction, const Callback& callback) {
  COMMAND_RPC_SEND_RAW_TX::request req;
  COMMAND_RPC_SEND_RAW_TX::response rsp;
  req.tx_as_hex = blobToHex(CryptoNote::tx_to_blob(transaction));
  std::error_code ec = jsonCommand(*m_httpClient, "/sendrawtransaction", req, rsp); 
  callback(ec);
}

void NodeRpcProxy::doGetRandomOutsByAmounts(std::vector<uint64_t>& amounts, uint64_t outsCount, std::vector<COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount>& outs, const Callback& callback) {
  COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::request req = AUTO_VAL_INIT(req);
  COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::response rsp = AUTO_VAL_INIT(rsp);
  req.amounts = std::move(amounts);
  req.outs_count = outsCount;

  std::error_code ec = binaryCommand(*m_httpClient, "/getrandom_outs.bin", req, rsp);

  if (!ec) {
    outs = std::move(rsp.outs);
  }
  callback(ec);
}

void NodeRpcProxy::doGetNewBlocks(std::list<crypto::hash>& knownBlockIds, std::list<CryptoNote::block_complete_entry>& newBlocks, uint64_t& startHeight, const Callback& callback) {
  CryptoNote::COMMAND_RPC_GET_BLOCKS_FAST::request req = AUTO_VAL_INIT(req);
  CryptoNote::COMMAND_RPC_GET_BLOCKS_FAST::response rsp = AUTO_VAL_INIT(rsp);
  req.block_ids = std::move(knownBlockIds);

  std::error_code ec = binaryCommand(*m_httpClient, "/getblocks.bin", req, rsp);

  if (!ec) {
    newBlocks = std::move(rsp.blocks);
    startHeight = rsp.start_height;
  }

  callback(ec);
}

void NodeRpcProxy::doGetTransactionOutsGlobalIndices(const crypto::hash& transactionHash, std::vector<uint64_t>& outsGlobalIndices, const Callback& callback) {
  CryptoNote::COMMAND_RPC_GET_TX_GLOBAL_OUTPUTS_INDEXES::request req = AUTO_VAL_INIT(req);
  CryptoNote::COMMAND_RPC_GET_TX_GLOBAL_OUTPUTS_INDEXES::response rsp = AUTO_VAL_INIT(rsp);
  req.txid = transactionHash;

  std::error_code ec = binaryCommand(*m_httpClient, "/get_o_indexes.bin", req, rsp);

  if (!ec) {
    outsGlobalIndices = std::move(rsp.o_indexes);
  }

  callback(ec);
}

void NodeRpcProxy::doQueryBlocks(const std::list<crypto::hash>& knownBlockIds, uint64_t timestamp, std::list<CryptoNote::BlockCompleteEntry>& newBlocks, uint64_t& startHeight, const Callback& callback) {
  CryptoNote::COMMAND_RPC_QUERY_BLOCKS::request req = AUTO_VAL_INIT(req);
  CryptoNote::COMMAND_RPC_QUERY_BLOCKS::response rsp = AUTO_VAL_INIT(rsp);

  req.block_ids = knownBlockIds;
  req.timestamp = timestamp;

  std::error_code ec = binaryCommand(*m_httpClient, "/queryblocks.bin", req, rsp);

  if (!ec) {
    for (auto& item : rsp.items) {
      BlockCompleteEntry entry;

      entry.blockHash = item.block_id;
      entry.block = std::move(item.block);
      entry.txs = std::move(item.txs);

      newBlocks.push_back(std::move(entry));
    }

    startHeight = rsp.start_height;
  }
  callback(ec);
}

void NodeRpcProxy::getPoolSymmetricDifference(std::vector<crypto::hash>&& known_pool_tx_ids, crypto::hash known_block_id, bool& is_bc_actual, std::vector<CryptoNote::Transaction>& new_txs, std::vector<crypto::hash>& deleted_tx_ids, const Callback& callback) { 
  is_bc_actual = true;
  callback(std::error_code()); 
};

}
