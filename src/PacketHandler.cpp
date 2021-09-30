/**
 * Copyright (c) 2019-2020 polistern
 */

#include <utility>
#include <random>

#include "DHTworker.h"
#include "PacketHandler.h"
#include "RelayPeersWorker.h"

namespace pbote {
namespace packet {

RequestHandler packet_handler;

//uint8_t PacketType[] = {0x45, 0x55, 0x49, 0x54, 0x50, 0x43, 0x52, 0x4b, 0x46, 0x4e, 0x41, 0x51, 0x4c, 0x53, 0x44, 0x58, 0x43};
//uint8_t CommunicationPreffix[] = {0x6D, 0x30, 0x52, 0xE9};

IncomingRequest::IncomingRequest(RequestHandler &parent)
    : m_Parent(parent) {
  // ToDo: re-make with std::function
  incomingPacketHandlers_[type::CommR] = &IncomingRequest::receiveRelayRequest;
  incomingPacketHandlers_[type::CommK] = &IncomingRequest::receiveRelayReturnRequest;
  //incomingPacketHandlers_[type::CommF] = &IncomingRequest::receiveFetchRequest;
  incomingPacketHandlers_[type::CommN] = &IncomingRequest::receiveResponsePkt;
  incomingPacketHandlers_[type::CommA] = &IncomingRequest::receivePeerListRequest;

  incomingPacketHandlers_[type::CommQ] = &IncomingRequest::receiveRetrieveRequest;
  incomingPacketHandlers_[type::CommY] = &IncomingRequest::receiveDeletionQueryRequest;
  incomingPacketHandlers_[type::CommS] = &IncomingRequest::receiveStoreRequest;
  incomingPacketHandlers_[type::CommD] = &IncomingRequest::receiveEmailPacketDeleteRequest;
  incomingPacketHandlers_[type::CommX] = &IncomingRequest::receiveIndexPacketDeleteRequest;
  incomingPacketHandlers_[type::CommF] = &IncomingRequest::receiveFindClosePeersRequest;
}

IncomingRequest::~IncomingRequest() {}

bool IncomingRequest::handleNewPacket(const std::shared_ptr<PacketForQueue>& queuePacket) {
  auto packet = pbote::parseCommPacket(queuePacket);
  if (packet != nullptr) {
    /// First we need to check CID in batches
    if (context.receive(packet->from, *packet)) {
      LogPrint(eLogDebug, "PacketHandler: packet ", packet->type, " pass to batch");
      return true;
    }

    auto it = incomingPacketHandlers_.find(packet->type);
    LogPrint(eLogDebug, "PacketHandler: it is ", it->first, "::", it->second);

    if (it != incomingPacketHandlers_.end())
      return (this->*(it->second))(packet);
    else {
      LogPrint(eLogWarning, "PacketHandler: got unknown packet type");
      return false;
    }

  } else {
    LogPrint(eLogWarning, "PacketHandler: can't parse packet");
    return false;
  }
}

/// not implemented
bool IncomingRequest::receiveRelayRequest(const std::shared_ptr<pbote::CommunicationPacket>& packet) {
  LogPrint(eLogDebug, "PacketHandler: receiveRelayRequest");
  return true;
}

/// not implemented
bool IncomingRequest::receiveRelayReturnRequest(const std::shared_ptr<pbote::CommunicationPacket>& packet) {
  LogPrint(eLogDebug, "Packet: receiveRelayReturnRequest");
  return true;
}

/// not implemented
bool IncomingRequest::receiveFetchRequest(const std::shared_ptr<pbote::CommunicationPacket>& packet) {
  LogPrint(eLogDebug, "Packet: receiveFetchRequest");
  return true;
}

bool IncomingRequest::receiveResponsePkt(const std::shared_ptr<pbote::CommunicationPacket>& packet) {
  LogPrint(eLogWarning, "Packet: receiveResponsePkt: Unexpected Response received");

  size_t offset = 0;
  uint8_t status;
  uint16_t dataLen;

  std::memcpy(&status, packet->payload.data(), sizeof status); offset += 1;

  LogPrint(eLogWarning, "Packet: receiveResponsePkt: status: ", status, ", message: ", pbote::statusToString(status));

  std::memcpy(&dataLen, packet->payload.data() + offset, sizeof dataLen); dataLen = ntohs(dataLen); offset += 2;

  if ((packet->payload.size() - offset) != dataLen)
    LogPrint(eLogWarning, "Packet: receiveResponsePkt: size mismatch: size=", (packet->payload.size() - offset), ", dataLen=", dataLen);

  uint8_t data[dataLen];
  std::memcpy(&data, packet->payload.data() + offset, dataLen);

  /// Peer List
  /// L for mhatta, P for str4d
  if (data[0] == (uint8_t)'L' || data[0] == (uint8_t)'P') {
    if (packet->ver == (uint8_t) 4) {
      LogPrint(eLogWarning,
               "Packet: receiveResponsePkt: Peer List, data.type=", data[0], ", data.ver=", unsigned(data[1]));
      return pbote::relay::relay_peers_worker.receivePeerListV4(data, dataLen);
    } else if (packet->ver == (uint8_t) 5) {
      LogPrint(eLogWarning,
               "Packet: receiveResponsePkt: Peer List, data.type=", data[0], ", data.ver=", unsigned(data[1]));
      return pbote::relay::relay_peers_worker.receivePeerListV5(data, dataLen);
    } else {
      LogPrint(eLogWarning,
               "Packet: receiveResponsePkt: Unsupported version, data.type: ", data[0], ", data.ver: ", unsigned(data[1]));
    }
  }

  /// Index Packet
  if (data[0] == (uint8_t)'I') {
    LogPrint(eLogWarning, "Packet: receiveResponsePkt: Index Packet");
    return true;
  }

  /// Email Packet
  if (data[0] == (uint8_t)'E') {
    LogPrint(eLogWarning, "Packet: receiveResponsePkt: Email Packet");
    return true;
  }

  /// Directory Entry Packet
  if (data[0] == (uint8_t)'C') {
    LogPrint(eLogWarning, "Packet: receiveResponsePkt: Directory Entry Packet");
    return true;
  }

  LogPrint(eLogWarning, "Packet: receiveResponsePkt: data.type=", data[0], ", data.ver=", unsigned(data[1]));
  LogPrint(eLogWarning, "Packet: receiveResponsePkt: unsupported data packet type");
  return false;
}

bool IncomingRequest::receivePeerListRequest(const std::shared_ptr<pbote::CommunicationPacket>& packet) {
  LogPrint(eLogDebug, "Packet: receivePeerListRequest");
  if (packet->ver == 4)
    pbote::relay::relay_peers_worker.peerListRequestV4(packet->from, packet->cid);
  else if (packet->ver == 5)
    pbote::relay::relay_peers_worker.peerListRequestV5(packet->from, packet->cid);
  else
    return false;
  return true;
}

///////////////////////////////////////////////////////////////////////////////////////////

bool IncomingRequest::receiveRetrieveRequest(const std::shared_ptr<pbote::CommunicationPacket>& packet) {
  LogPrint(eLogDebug, "Packet: receiveRetrieveRequest");
  if (packet->ver == 4 && packet->type == type::CommQ) {
    pbote::kademlia::DHT_worker.receiveRetrieveRequest(packet);
    return true;
  }
  LogPrint(eLogWarning, "Packet: receiveRetrieveRequest: unknown packet version: ", packet->ver, ", and type: ", unsigned(packet->type));
  return false;
}

bool IncomingRequest::receiveDeletionQueryRequest(const std::shared_ptr<pbote::CommunicationPacket>& packet) {
  LogPrint(eLogDebug, "Packet: receiveDeletionQueryRequest");
  /// Y for mhatta
  if (packet->ver == 4 && packet->type == type::CommY) {
    pbote::kademlia::DHT_worker.receiveDeletionQuery(packet);
    return true;
  }
  /// L for str4d
  if (packet->ver == 4 && packet->type == (uint8_t)'L') {
    pbote::kademlia::DHT_worker.receiveDeletionQuery(packet);
    return true;
  }

  LogPrint(eLogWarning, "Packet: receiveDeletionQueryRequest: unknown packet version: ", packet->ver, ", and type: ", unsigned(packet->type));
  return false;
}

bool IncomingRequest::receiveStoreRequest(const std::shared_ptr<pbote::CommunicationPacket>& packet) {
  LogPrint(eLogDebug, "Packet: receiveStoreRequest");
  if (packet->ver == 4 && packet->type == type::CommS) {
    pbote::kademlia::DHT_worker.receiveStoreRequest(packet);
    return true;
  }
  LogPrint(eLogWarning, "Packet: receiveStoreRequest: unknown packet version: ", packet->ver, ", and type: ", unsigned(packet->type));
  return false;
}

bool IncomingRequest::receiveEmailPacketDeleteRequest(const std::shared_ptr<pbote::CommunicationPacket>& packet) {
  LogPrint(eLogDebug, "Packet: receiveEmailPacketDeleteRequest");
  if (packet->ver == 4 && packet->type == type::CommD) {
    pbote::kademlia::DHT_worker.receiveEmailPacketDeleteRequest(packet);
    return true;
  }
  LogPrint(eLogWarning, "Packet: receiveEmailPacketDeleteRequest: unknown packet version: ", packet->ver, ", and type: ", unsigned(packet->type));
  return false;
}

bool IncomingRequest::receiveIndexPacketDeleteRequest(const std::shared_ptr<pbote::CommunicationPacket>& packet) {
  LogPrint(eLogDebug, "Packet: receiveIndexPacketDeleteRequest");
  if (packet->ver == 4 && packet->type == type::CommX) {
    pbote::kademlia::DHT_worker.receiveIndexPacketDeleteRequest(packet);
    return true;
  }
  LogPrint(eLogWarning, "Packet: receiveIndexPacketDeleteRequest: unknown packet version: ", packet->ver, ", and type: ", unsigned(packet->type));
  return false;
}

bool IncomingRequest::receiveFindClosePeersRequest(const std::shared_ptr<pbote::CommunicationPacket>& packet) {
  LogPrint(eLogDebug, "PacketHandler: receiveFindClosePeersRequest");
  if (packet->ver == 4 && packet->type == type::CommF) {
    pbote::kademlia::DHT_worker.receiveFindClosePeers(packet);
    return true;
  }
  LogPrint(eLogWarning, "Packet: receiveFindClosePeersRequest: unknown packet version: ", packet->ver, ", and type: ", unsigned(packet->type));
  return false;
}

RequestHandler::RequestHandler()
    : started_(false), m_PHandlerThread(nullptr), m_recvQueue(nullptr)/*, m_sendQueue(nullptr)*/ {}

RequestHandler::~RequestHandler() {
  delete m_PHandlerThread;
  m_PHandlerThread = nullptr;
}

void RequestHandler::start() {
  if (!started_) {
    m_recvQueue = context.getRecvQueue();
    //m_sendQueue = context.getSendQueue();
    started_ = true;
    m_PHandlerThread = new std::thread(std::bind(&RequestHandler::run, this));
  }
}

void RequestHandler::stop() {
  LogPrint(eLogWarning, "RequestHandler: stopping");
  started_ = false;
  if (m_PHandlerThread) {
    m_PHandlerThread->join();
    delete m_PHandlerThread;
    m_PHandlerThread = nullptr;
  }
  LogPrint(eLogWarning, "RequestHandler: stopped");
}

void RequestHandler::run() {
  LogPrint(eLogInfo, "PacketHandler: run packet handler thread");
  while (started_) {
    auto queuePacket = m_recvQueue->GetNext();
    IncomingRequest newSession(*this);

    if (!newSession.handleNewPacket(queuePacket))
      LogPrint(eLogWarning, "PacketHandler: parsing failed");
  }
}

} // namespace packet
} // namespace pbote