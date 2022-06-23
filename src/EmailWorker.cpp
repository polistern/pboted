/**
 * Copyright (C) 2019-2022 polistern
 *
 * This file is part of pboted and licensed under BSD3
 *
 * See full license text in LICENSE file at top of project tree
 */

#include <ctime>
#include <iterator>
#include <openssl/sha.h>
#include <utility>
#include <vector>

#include "BoteContext.h"
#include "DHTworker.h"
#include "EmailWorker.h"

namespace pbote
{
namespace kademlia
{

EmailWorker email_worker;

EmailWorker::EmailWorker ()
  : started_ (false),
    m_send_thread_ (nullptr),
    m_worker_thread_ (nullptr)
{
}

EmailWorker::~EmailWorker ()
{
  stop ();

  if (m_worker_thread_)
    {
      m_worker_thread_->join ();

      delete m_worker_thread_;
      m_worker_thread_ = nullptr;
    }
}

void
EmailWorker::start ()
{
  if (started_ && m_worker_thread_)
    return;

  if (context.get_identities_count () == 0)
    LogPrint (eLogError, "EmailWorker: Have no Bote identities for start");
  else
    {
      startSendEmailTask ();
      startCheckEmailTasks ();
    }

  started_ = true;
  m_worker_thread_ = new std::thread ([this] { run (); });
}

void
EmailWorker::stop ()
{
  if (!started_)
    return;

  started_ = false;
  stopSendEmailTask ();
  stopCheckEmailTasks ();

  LogPrint (eLogWarning, "EmailWorker: Stopped");
}

void
EmailWorker::startCheckEmailTasks ()
{
  if (!started_ || !context.get_identities_count ())
    return;

  auto email_identities = context.getEmailIdentities ();
  // ToDo: move to object?
  for (const auto &identity : email_identities)
    {
      bool thread_exist = check_thread_exist (identity->publicName);
      if (thread_exist)
        continue;

      auto new_thread = std::make_shared<std::thread> (
          [this, identity] { checkEmailTask (identity); });

      LogPrint (eLogInfo, "EmailWorker: Start check task for ",
                identity->publicName);
      m_check_threads_[identity->publicName] = std::move (new_thread);
    }
}

bool
EmailWorker::stopCheckEmailTasks ()
{
  LogPrint (eLogInfo, "EmailWorker: Stopping check tasks");

  while (!m_check_threads_.empty ())
    {
      auto it = m_check_threads_.begin ();

      it->second->join ();
      m_check_threads_.erase (it->first);
      LogPrint (eLogInfo, "EmailWorker: Task for ", it->first, " stopped");
    }

  LogPrint (eLogInfo, "EmailWorker: Check tasks stopped");
  return true;
}

void
EmailWorker::startSendEmailTask ()
{
  if (!started_ || !context.get_identities_count ())
    return;

  LogPrint (eLogInfo, "EmailWorker: Start send task");
  m_send_thread_ = new std::thread ([this] { sendEmailTask (); });
}

bool
EmailWorker::stopSendEmailTask ()
{
  LogPrint (eLogInfo, "EmailWorker: Stopping send task");

  if (m_send_thread_ && !started_)
    {
      m_send_thread_->join ();

      delete m_send_thread_;
      m_send_thread_ = nullptr;
    }

  LogPrint (eLogInfo, "EmailWorker: Send task stopped");
  return true;
}

void
EmailWorker::run ()
{
  while (started_)
    {
      size_t id_count = context.get_identities_count ();

      if (id_count)
        {
          LogPrint (eLogInfo, "EmailWorker: Identities now: ", id_count);
          startCheckEmailTasks ();

          if (!m_send_thread_)
            {
              LogPrint (eLogDebug, "EmailWorker: Try to start send task");
              startSendEmailTask ();
            }
        }
      else
        {
          LogPrint (eLogWarning, "EmailWorker: Have no identities for start");
          stopSendEmailTask ();
          stopCheckEmailTasks ();
        }

      std::this_thread::sleep_for (std::chrono::seconds (60));
    }
}

void
EmailWorker::checkEmailTask (const sp_id_full &email_identity)
{
  bool first_complete = false;
  std::string id_name = email_identity->publicName;
  while (started_)
    {
      // ToDo: read interval parameter from config
      if (first_complete)
        std::this_thread::sleep_for (
            std::chrono::seconds (CHECK_EMAIL_INTERVAL));
      first_complete = true;

      auto index_packets = retrieveIndex (email_identity);

      auto local_index_packet
          = DHT_worker.getIndex (email_identity->identity.GetIdentHash ());

      if (!local_index_packet.empty ())
        {
          LogPrint (eLogDebug, "EmailWorker: Check task: ", id_name, ": got ",
                    local_index_packet.size (), " local index");

          /// from_net is true, because we save it as is
          pbote::IndexPacket parsed_local_index_packet;
          bool parsed = parsed_local_index_packet.fromBuffer (
              local_index_packet, true);

          if (parsed
              && parsed_local_index_packet.data.size ()
                     == parsed_local_index_packet.nump)
            {
              index_packets.push_back (parsed_local_index_packet);
            }
        }
      else
        {
          LogPrint (eLogDebug, "EmailWorker: Check task: ", id_name,
                    ": Can't find local index");
        }

      LogPrint (eLogDebug, "EmailWorker: Check task: ", id_name,
                ": Index count: ", index_packets.size ());

      auto enc_mail_packets = retrieveEmailPacket (index_packets);

      LogPrint (eLogDebug, "EmailWorker: Check task: ", id_name,
                ": Mail count: ", enc_mail_packets.size ());

      if (enc_mail_packets.empty ())
        {
          LogPrint (eLogDebug, "EmailWorker: Check task: ", id_name,
                    ": Have no mail for process");
          LogPrint (eLogInfo, "EmailWorker: Check task: ", id_name,
                    ": Round complete");
          continue;
        }

      auto emails = processEmail (email_identity, enc_mail_packets);

      LogPrint (eLogInfo, "EmailWorker: Check task: ", id_name,
                ": email(s) processed: ", emails.size ());

      // ToDo: check mail signature
      for (auto mail : emails)
        {
          mail.save ("inbox");

          pbote::EmailDeleteRequestPacket delete_email_packet;

          auto email_packet = mail.getDecrypted ();
          memcpy (delete_email_packet.DA, email_packet.DA, 32);
          auto enc_email_packet = mail.getEncrypted ();
          memcpy (delete_email_packet.key, enc_email_packet.key, 32);

          i2p::data::Tag<32> email_dht_key (enc_email_packet.key);
          i2p::data::Tag<32> email_del_auth (email_packet.DA);

          /// We need to remove packets for all received email from nodes
          // ToDo: check status of responses
          DHT_worker.deleteEmail (email_dht_key, DataE, delete_email_packet);

          /// Delete index packets
          // ToDo: add multipart email support
          DHT_worker.deleteIndexEntry (
              email_identity->identity.GetIdentHash (), email_dht_key,
              email_del_auth);
        }

      // ToDo: check sent emails status -> check_email_delivery_task
      //   if nodes sent empty response - mark as deleted (delivered)

      LogPrint (eLogInfo, "EmailWorker: Check task:  ", id_name, ": complete");
    }
}

void
EmailWorker::incompleteEmailTask ()
{
  // ToDo: need to implement for multipart mail packets
}

void
EmailWorker::sendEmailTask ()
{
  while (started_)
    {
      // ToDo: read interval parameter from config
      std::this_thread::sleep_for (std::chrono::seconds (SEND_EMAIL_INTERVAL));

      std::vector<std::string> nodes;
      auto outbox = checkOutbox ();
      if (outbox.empty ())
        continue;

      /// Create Encrypted Email Packet
      // ToDo: move to function
      for (const auto &email : outbox)
        {
          pbote::EmailEncryptedPacket enc_packet;
          auto packet = email->getDecrypted ();

          // Get hash of Delete Auth
          SHA256 (packet.DA, 32, enc_packet.delete_hash);
          i2p::data::Tag<32> del_hash (enc_packet.delete_hash), del_auth (packet.DA);

          LogPrint (eLogDebug, "EmailWorker: Send: del_auth: ", del_auth.ToBase64 ());
          LogPrint (eLogDebug, "EmailWorker: Send: del_hash: ", del_hash.ToBase64 ());

          email->setField ("X-I2PBote-Delete-Auth-Hash", del_hash.ToBase64 ());

          /// Create recipient
          std::shared_ptr<BoteIdentityPublic> recipient_identity;
          std::string to_address = email->getToAddresses ();

          std::string format_prefix = to_address.substr(0, to_address.find(".") + 1);

          if (format_prefix.compare(ADDRESS_B32_PREFIX) == 0)
            recipient_identity = parse_address_v1(to_address);
          else if (format_prefix.compare(ADDRESS_B64_PREFIX) == 0)
            recipient_identity = parse_address_v1(to_address);
          else
            recipient_identity = parse_address_v0(to_address);

          if (recipient_identity == nullptr)
            {
              LogPrint (eLogWarning, "EmailWorker: Send: Can't create "
                        "identity from \"TO\" header, skip mail");
              email->skip (true);
              continue;
            }

          LogPrint (eLogDebug, "EmailWorker: Send: recipient_identity: ",
                    recipient_identity->ToBase64 ());
          LogPrint (eLogDebug, "EmailWorker: Send: email: recipient hash: ",
                    recipient_identity->GetIdentHash ().ToBase64 ());

          /// Get and check FROM identity
          auto from_name = email->field ("From");
          auto identity_name = from_name.substr (0, from_name.find (' '));
          //sp_id_full identity = pbote::context.identityByName (identity_name);

          /*if (!identity)
            {
              if (context.get_identities_count ())
                {
                  auto email_identities = context.getEmailIdentities ();
                  LogPrint (eLogWarning, "EmailWorker: Send: Can't find "
                            "identity with name: ", identity_name);

                  identity = email_identities[0];
                  LogPrint (eLogWarning, "EmailWorker: Send: Try to use ",
                            identity->publicName, " just for encrypt data");
                }
              else
                {
                  LogPrint (eLogError, "EmailWorker: Send: Have no "
                            "identities, stopping send task");
                  stopSendEmailTask ();
                  return;
                }
            }*/

          /// Sign data
          // ToDo: sign email here

          /// Encrypt data

          /// Create sender
          std::shared_ptr<BoteIdentityPublic> sender_identity;
          std::string from_address = email->get_from_address ();

          LogPrint (eLogDebug, "EmailWorker: Send: from_address: ", from_address);

          format_prefix = from_address.substr(0, from_address.find(".") + 1);

          if (format_prefix.compare(ADDRESS_B32_PREFIX) == 0)
            sender_identity = parse_address_v1(from_address);
          else if (format_prefix.compare(ADDRESS_B64_PREFIX) == 0)
            sender_identity = parse_address_v1(from_address);
          else
            sender_identity = parse_address_v0(from_address);

          if (sender_identity == nullptr)
            {
              LogPrint (eLogWarning, "EmailWorker: Send: Can't create "
                        "identity from \"FROM\" header, skip mail");
              email->skip (true);
              continue;
            }

          LogPrint (eLogDebug, "EmailWorker: Send: sender_identity: ",
                    sender_identity->ToBase64 ());
          LogPrint (eLogDebug, "EmailWorker: Send: email: sender hash: ",
                    sender_identity->GetIdentHash ().ToBase64 ());


          LogPrint (eLogDebug, "EmailWorker: Send: packet.data.size: ",
                    packet.data.size ());

          auto packet_bytes = packet.toByte ();
          /*enc_packet.edata
              = identity->identity.GetPublicIdentity ()->Encrypt (
                  packet_bytes.data (), packet_bytes.size (),
                  recipient_identity->GetCryptoPublicKey ());*/
          enc_packet.edata = sender_identity->Encrypt (
                  packet_bytes.data (), packet_bytes.size (),
                  recipient_identity->GetCryptoPublicKey ());

          if (enc_packet.edata.empty ())
          {
            email->skip (true);
            LogPrint (eLogError, "EmailWorker: Send: Encrypted data is empty, skipped");
            continue;
          }

          enc_packet.length = enc_packet.edata.size ();
          enc_packet.alg = sender_identity->GetKeyType ();
          enc_packet.stored_time = 0;

          LogPrint (eLogDebug, "EmailWorker: Send: enc_packet.edata.size(): ",
                    enc_packet.edata.size ());

          /// Get hash of data + length for DHT key
          const size_t data_for_hash_len = 2 + enc_packet.edata.size ();
          std::vector<uint8_t> data_for_hash
              = { static_cast<uint8_t> (enc_packet.length >> 8),
                  static_cast<uint8_t> (enc_packet.length & 0xff) };
          data_for_hash.insert (data_for_hash.end (),
                                enc_packet.edata.begin (),
                                enc_packet.edata.end ());

          SHA256 (data_for_hash.data (), data_for_hash_len, enc_packet.key);

          i2p::data::Tag<32> dht_key (enc_packet.key);
          LogPrint (eLogDebug, "EmailWorker: Send: dht_key: ",
                    dht_key.ToBase64 ());
          email->setField ("X-I2PBote-DHT-Key", dht_key.ToBase64 ());
          LogPrint (eLogDebug, "EmailWorker: Send: enc_packet.length : ",
                    enc_packet.length);

          email->setEncrypted (enc_packet);
        }

      /// Store Encrypted Email Packet
      // ToDo: move to function
      for (const auto &email : outbox)
        {
          if (email->skip ())
            continue;

          pbote::StoreRequestPacket store_packet;

          /// For now, HashCash not checking from Java-Bote side
          store_packet.hashcash = email->getHashCash ();
          store_packet.hc_length = store_packet.hashcash.size ();
          LogPrint (eLogDebug, "EmailWorker: Send: store_packet.hc_length: ",
                    store_packet.hc_length);

          store_packet.length = email->getEncrypted ().toByte ().size ();
          store_packet.data = email->getEncrypted ().toByte ();
          LogPrint (eLogDebug, "EmailWorker: Send: store_packet.length: ",
                    store_packet.length);

          /// Send Store Request with Encrypted Email Packet to nodes
          nodes = DHT_worker.store (
              i2p::data::Tag<32> (email->getEncrypted ().key),
              email->getEncrypted ().type, store_packet);

          /// If have no OK store responses - mark message as skipped
          if (nodes.empty ())
            {
              email->skip (true);
              LogPrint (eLogWarning, "EmailWorker: Send: email not sent");
            }
          else
            {
              DHT_worker.safe (email->getEncrypted ().toByte ());
              LogPrint (eLogDebug, "EmailWorker: Send: Email sent to ",
                        nodes.size (), " node(s)");
            }
        }

      /// Create and store Index Packet
      // ToDo: move to function
      for (const auto &email : outbox)
        {
          if (email->skip ())
            continue;

          pbote::IndexPacket new_index_packet;

          // Create recipient
          // ToDo: re-use from previous step
          std::shared_ptr<BoteIdentityPublic> recipient_identity;
          std::string to_address = email->getToAddresses ();

          std::string format_prefix = to_address.substr(0, to_address.find(".") + 1);

          if (format_prefix.compare(ADDRESS_B32_PREFIX) == 0)
            recipient_identity = parse_address_v1(to_address);
          else if (format_prefix.compare(ADDRESS_B64_PREFIX) == 0)
            recipient_identity = parse_address_v1(to_address);
          else
            recipient_identity = parse_address_v0(to_address);

          if (recipient_identity == nullptr)
            {
              LogPrint (eLogWarning, "EmailWorker: Send: Can't create "
                        "identity from \"TO\" header, skip mail");
              email->skip (true);
              continue;
            }

          LogPrint (eLogDebug, "EmailWorker: Send: recipient_identity: ",
                    recipient_identity->ToBase64 ());
          LogPrint (eLogDebug, "EmailWorker: Send: index recipient hash: ",
                    recipient_identity->GetIdentHash ().ToBase64 ());

          memcpy (new_index_packet.hash,
                  recipient_identity->GetIdentHash ().data (), 32);

          // ToDo: for test, need to rewrite
          new_index_packet.nump = 1;
          // for (const auto &email : encryptedEmailPackets) {
          pbote::IndexPacket::Entry entry{};
          memcpy (entry.key, email->getEncrypted ().key, 32);
          memcpy (entry.dv, email->getEncrypted ().delete_hash, 32);

          auto unix_timestamp = std::chrono::seconds (std::time (nullptr));
          auto value = std::chrono::duration_cast<std::chrono::seconds> (
              unix_timestamp);
          entry.time = value.count ();

          new_index_packet.data.push_back (entry);
          //}

          pbote::StoreRequestPacket store_index_packet;

          /// For now it's not checking from Java-Bote side
          store_index_packet.hashcash = email->getHashCash ();
          store_index_packet.hc_length = store_index_packet.hashcash.size ();
          LogPrint (eLogDebug, "EmailWorker: Send: store_index.hc_length: ",
              store_index_packet.hc_length);

          auto index_packet = new_index_packet.toByte ();

          store_index_packet.length = index_packet.size ();
          store_index_packet.data = index_packet;

          /// Send Store Request with Index Packet to nodes
          nodes = DHT_worker.store (recipient_identity->GetIdentHash (),
                                    new_index_packet.type,
                                    store_index_packet);

          /// If have no OK store responses - mark message as skipped
          if (nodes.empty ())
            {
              email->skip (true);
              LogPrint (eLogWarning, "EmailWorker: Send: Index not sent");
            }
          else
            {
              DHT_worker.safe (new_index_packet.toByte ());
              LogPrint (eLogDebug, "EmailWorker: Send: Index send to ",
                        nodes.size (), " node(s)");
            }
        }

      // ToDo: move to function
      for (const auto &email : outbox)
        {
          if (email->skip ())
            continue;

          email->setField ("X-I2PBote-Deleted", "false");
          /// Write new metadata before move file to sent
          email->save ("");
          email->move ("sent");
        }

      LogPrint (eLogInfo, "EmailWorker: Send: Round complete");
    }
}

std::vector<pbote::IndexPacket>
EmailWorker::retrieveIndex (const sp_id_full &identity)
{
  auto identity_hash = identity->identity.GetIdentHash ();
  LogPrint (eLogDebug, "EmailWorker: retrieveIndex: Try to find index for: ",
            identity_hash.ToBase64 ());
  /* Use findAll rather than findOne because some peers might have an
   *  incomplete set of Email Packet keys, and because we want to send
   *  IndexPacketDeleteRequests to all of them.
   */

  auto results = DHT_worker.findAll (identity_hash, DataI);
  if (results.empty ())
    {
      LogPrint (eLogWarning,
                "EmailWorker: retrieveIndex: Can't find index for: ",
                identity_hash.ToBase64 ());
      return {};
    }

  std::map<i2p::data::Tag<32>, pbote::IndexPacket> index_packets;
  /// Retrieve index packets
  for (const auto &response : results)
    {
      if (response->type != type::CommN)
        {
          // ToDo: looks like in case if we got request to ourself, for now we
          // just skip it
          LogPrint (eLogWarning,
                    "EmailWorker: retrieveIndex: Got non-response packet in "
                    "batch, type: ",
                    response->type, ", ver: ", unsigned (response->ver));
          continue;
        }

      LogPrint (eLogDebug, "EmailWorker: retrieveIndex: Got response from: ",
                response->from.substr (0, 15), "...");
      size_t offset = 0;
      uint8_t status;
      uint16_t dataLen;

      std::memcpy (&status, response->payload.data (), 1);
      offset += 1;
      std::memcpy (&dataLen, response->payload.data () + offset, 2);
      offset += 2;
      dataLen = ntohs (dataLen);

      if (status != StatusCode::OK)
        {
          LogPrint (eLogWarning, "EmailWorker: retrieveIndex: Response status: ",
                    statusToString (status));
          continue;
        }

      if (dataLen < 4)
        {
          LogPrint (eLogWarning, "EmailWorker: retrieveIndex: Packet without "
                                 "payload, parsing skipped");
          continue;
        }

      std::vector<uint8_t> data (response->payload.begin () + offset,
                                 response->payload.begin () + offset
                                     + dataLen);

      if (DHT_worker.safe (data))
        LogPrint (eLogDebug, "EmailWorker: retrieveIndex: Index packet saved");

      pbote::IndexPacket index_packet;
      bool parsed = index_packet.fromBuffer (data, true);

      if (parsed && !index_packet.data.empty ())
        {
          i2p::data::Tag<32> hash (index_packet.hash);
          index_packets.insert (
              std::pair<i2p::data::Tag<32>, pbote::IndexPacket> (
                  hash, index_packet));
        }
      else
        LogPrint (eLogWarning,
                  "EmailWorker: retrieveIndex: Packet without entries");
    }
  LogPrint (eLogDebug, "EmailWorker: retrieveIndex: Index packets parsed: ",
            index_packets.size ());

  std::vector<pbote::IndexPacket> res;
  res.reserve (index_packets.size ());

  for (const auto &packet : index_packets)
    res.push_back (packet.second);

  // save index packets for interrupt case
  // ToDo: check if we have packet locally and sent delete request now

  return res;
}

std::vector<pbote::EmailEncryptedPacket>
EmailWorker::retrieveEmailPacket (
    const std::vector<pbote::IndexPacket> &index_packets)
{
  std::vector<std::shared_ptr<pbote::CommunicationPacket> > responses;
  std::vector<pbote::EmailEncryptedPacket> local_email_packets;

  for (const auto &index : index_packets)
    {
      for (auto entry : index.data)
        {
          i2p::data::Tag<32> hash (entry.key);

          auto local_email_packet = DHT_worker.getEmail (hash);
          if (!local_email_packet.empty ())
            {
              LogPrint (eLogDebug,
                        "EmailWorker: retrieveEmailPacket: Got local "
                        "encrypted email for key: ",
                        hash.ToBase64 ());
              pbote::EmailEncryptedPacket parsed_local_email_packet;
              bool parsed = parsed_local_email_packet.fromBuffer (
                  local_email_packet.data (), local_email_packet.size (),
                  true);

              if (parsed && !parsed_local_email_packet.edata.empty ())
                {
                  local_email_packets.push_back (parsed_local_email_packet);
                }
            }
          else
            {
              LogPrint (eLogDebug,
                        "EmailWorker: retrieveEmailPacket: Can't find local "
                        "encrypted email for key: ",
                        hash.ToBase64 ());
            }

          auto temp_results = DHT_worker.findAll (hash, DataE);
          responses.insert (responses.end (), temp_results.begin (),
                            temp_results.end ());
        }
    }

  LogPrint (eLogDebug, "EmailWorker: retrieveEmailPacket: Responses: ",
            responses.size ());

  std::map<i2p::data::Tag<32>, pbote::EmailEncryptedPacket> mail_packets;
  for (const auto &response : responses)
    {
      if (response->type != type::CommN)
        {
          // ToDo: looks like we got request to ourself, for now just skip it
          LogPrint (eLogWarning,
                    "EmailWorker: retrieveIndex: Got non-response packet in "
                    "batch, type: ",
                    response->type, ", ver: ", unsigned (response->ver));
          continue;
        }

      size_t offset = 0;
      uint8_t status;
      uint16_t dataLen;

      std::memcpy (&status, response->payload.data (), 1);
      offset += 1;
      std::memcpy (&dataLen, response->payload.data () + offset, 2);
      offset += 2;
      dataLen = ntohs (dataLen);

      if (status != StatusCode::OK)
        {
          LogPrint (eLogWarning,
                    "EmailWorker: retrieveEmailPacket: Response status: ",
                    statusToString (status));
          continue;
        }

      if (dataLen == 0)
        {
          LogPrint (eLogWarning, "EmailWorker: retrieveEmailPacket: Packet "
                                 "without payload, parsing skipped");
          continue;
        }

      LogPrint (
          eLogDebug,
          "EmailWorker: retrieveEmailPacket: Got email packet, payload size: ",
          dataLen);
      std::vector<uint8_t> data
          = { response->payload.data () + offset,
              response->payload.data () + offset + dataLen };

      if (DHT_worker.safe (data))
        LogPrint (eLogDebug, "EmailWorker: retrieveEmailPacket: Save "
                             "encrypted email packet locally");

      pbote::EmailEncryptedPacket parsed_packet;
      bool parsed = parsed_packet.fromBuffer (data.data (), dataLen, true);

      if (parsed && !parsed_packet.edata.empty ())
        {
          i2p::data::Tag<32> hash (parsed_packet.key);
          mail_packets.insert (
              std::pair<i2p::data::Tag<32>, pbote::EmailEncryptedPacket> (
                  hash, parsed_packet));
        }
      else
        LogPrint (
            eLogWarning,
            "EmailWorker: retrieveEmailPacket: Mail packet without entries");
    }
  LogPrint (eLogDebug,
            "EmailWorker: retrieveEmailPacket: Parsed mail packets: ",
            mail_packets.size ());

  for (auto local_packet : local_email_packets)
    {
      i2p::data::Tag<32> hash (local_packet.key);
      mail_packets.insert (
          std::pair<i2p::data::Tag<32>, pbote::EmailEncryptedPacket> (
              hash, local_packet));
    }

  LogPrint (eLogDebug, "EmailWorker: retrieveEmailPacket: Mail packets: ",
            mail_packets.size ());

  std::vector<pbote::EmailEncryptedPacket> res;
  res.reserve (mail_packets.size ());

  for (const auto &packet : mail_packets)
    res.push_back (packet.second);

  // save encrypted email packets for interrupt case
  // ToDo: check if we have packet locally and sent delete request now

  return res;
}

std::vector<pbote::EmailUnencryptedPacket>
EmailWorker::loadLocalIncompletePacket ()
{
  // ToDo: TBD
  // ToDo: move to ?
  /*std::string indexPacketPath = pbote::fs::DataDirPath("incomplete");
  std::vector<std::string> packets_path;
  std::vector<pbote::EmailUnencryptedPacket> indexPackets;
  auto result = pbote::fs::ReadDir(indexPacketPath, packets_path);
  if (result) {
    for (const auto &packet_path : packets_path) {
      std::ifstream file(packet_path, std::ios::binary);

      std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
  (std::istreambuf_iterator<char>()));

      file.close();
      auto indexPacket = parseEmailUnencryptedPkt(bytes.data(), bytes.size(),
  false); if (!indexPacket.data.empty()) indexPackets.push_back(indexPacket);
    }
    LogPrint(eLogDebug, "Email: loadLocalIndex: loaded index files: ",
  indexPackets.size()); return indexPackets;
  }
  LogPrint(eLogWarning, "Email: loadLocalIndex: have no index files");*/
  return {};
}

std::vector<std::shared_ptr<pbote::Email> >
EmailWorker::checkOutbox ()
{
  /// outbox - plain text packet
  // ToDo: encrypt all local stored emails
  std::string outboxPath = pbote::fs::DataDirPath ("outbox");
  std::vector<std::string> mails_path;
  auto result = pbote::fs::ReadDir (outboxPath, mails_path);

  if (!result)
    {
      LogPrint (eLogDebug, "EmailWorker: checkOutbox: No emails in outbox ");
      return {};
    }

  std::vector<std::shared_ptr<pbote::Email> > emails;

  for (const auto &mail_path : mails_path)
    {
      /// Read mime packet
      std::ifstream file (mail_path, std::ios::binary);
      std::vector<uint8_t> bytes ((std::istreambuf_iterator<char> (file)),
                                  (std::istreambuf_iterator<char> ()));
      file.close ();

      pbote::Email mailPacket;
      mailPacket.fromMIME (bytes);
      mailPacket.bytes ();

      if (mailPacket.length () > 0)
        {
          LogPrint (eLogDebug,
                    "EmailWorker: checkOutbox: file loaded: ", mail_path);
        }
      else
        {
          LogPrint (eLogWarning,
                    "EmailWorker: checkOutbox: can't read file: ", mail_path);
          continue;
        }

      mailPacket.filename (mail_path);

      /* Check if if FROM and TO fields have valid public names, else
       * Check if <name@domain> in AddressBook for replacement
       * if not found - log warning and skip
       * if replaced - save modified email to file to keep changes
       */
      // ToDo: need to simplify

      std::string from_address = mailPacket.field ("From");
      std::string to_address = mailPacket.field ("To");
      if (from_address.empty () || to_address.empty ())
        {
          LogPrint (eLogWarning,
                    "EmailWorker: checkOutbox: FROM or TO field are empty");
          continue;
        }

      //bool changed = false;
      std::string et_char ("@"), less_char ("<"), more_char (">");
      size_t from_less_pos = from_address.find (less_char);
      size_t from_et_pos = from_address.find (et_char);

      /// Check if we got "@" and "<" it in order "alias <name@domain>"
      if (from_less_pos != std::string::npos
          && from_et_pos != std::string::npos
          && from_less_pos < from_et_pos)
        {
          LogPrint (eLogDebug,
                    "EmailWorker: checkOutbox: Try to replace FROM: ",
                    from_address);

          std::string old_from_address = from_address;
          std::string pub_name = from_address.substr (0, from_less_pos - 1);
          from_address.erase (0, from_less_pos + 1);
          from_et_pos = from_address.find (et_char);
          std::string alias_name = from_address.substr (0, from_et_pos);

          auto pub_from_identity = context.identityByName (pub_name);
          auto alias_from_identity = context.identityByName (alias_name);
          if (!pub_from_identity && !alias_from_identity)
            {
              LogPrint (eLogWarning,
                        "EmailWorker: checkOutbox: Can't find address for name: ",
                        pub_name, ", alias: ", alias_name);
              continue;
            }
          std::string new_from;
          if (pub_from_identity)
            {
              std::string pub_str = pub_from_identity->full_key.substr (0, 86);
              new_from.append (pub_from_identity->publicName + " <"
                               + pub_str + ">");
            }
          else if (alias_from_identity)
            {
              std::string alias_str
                = alias_from_identity->full_key.substr (0, 86);
              new_from.append (alias_from_identity->publicName + " <"
                               + alias_str + ">");
            }
          else
            {
              LogPrint (eLogError,
                        "EmailWorker: checkOutbox: Unknown error, name: ",
                        pub_name, ", alias: ", alias_name);
              continue;
            }
          LogPrint (eLogDebug,
                    "EmailWorker: checkOutbox: FROM replaced, old: ",
                    old_from_address, ", new: ", new_from);
          mailPacket.setField ("From", new_from);
          //changed = true;
        }

      // Now replace TO
      size_t to_less_pos = to_address.find (less_char);
      size_t to_et_pos = to_address.find (et_char);
      size_t to_more_pos = to_address.find (more_char);
      /// Check if we got "@" and "<" it in order "alias <name@domain>"
      if (to_less_pos != std::string::npos
          && to_et_pos != std::string::npos
          && to_less_pos < to_et_pos)
        {
          LogPrint (eLogDebug,
                    "EmailWorker: checkOutbox: Try to replace TO: ", to_address);

          std::string old_to_address = to_address;
          std::string pub_name = to_address.substr (0, to_less_pos - 1);
          to_address.erase (0, to_less_pos + 1);
          //to_et_pos = to_address.find (et_char);
          //std::string alias_name = to_address.substr (0, to_et_pos);
          to_more_pos = to_address.find (more_char);
          std::string alias_name = to_address.substr (0, to_more_pos);

          LogPrint (eLogDebug,
                    "EmailWorker: checkOutbox: pub_name  : ", pub_name);
          LogPrint (eLogDebug,
                    "EmailWorker: checkOutbox: alias_name: ", alias_name);

          auto pub_to_address = context.address_for_name (pub_name);
          auto alias_to_address = context.address_for_alias (alias_name);

          if (pub_to_address.empty () && alias_to_address.empty ())
            {
              LogPrint (eLogWarning,
                        "EmailWorker: checkOutbox: Can't find address for ",
                        to_address);
              continue;
            }

          std::string new_to;
          if (!pub_to_address.empty ())
            {
              new_to.append (pub_name + " <" + pub_to_address + ">");
            }
          else if (!alias_to_address.empty ())
            {
              new_to.append (alias_name + " <" + alias_to_address + ">");
            }
          else
            {
              LogPrint (eLogError,
                        "EmailWorker: checkOutbox: Unknown error, name: ",
                        pub_name, ", alias: ", alias_name);
              continue;
            }
          LogPrint (eLogDebug,
                    "EmailWorker: checkOutbox: TO replaced, old: ",
                    old_to_address, ", new: ", new_to);
          mailPacket.setField ("To", new_to);
          //changed = true;
        }

      /// On this step will be generated Message-ID and
      ///   it will be saved and not be re-generated
      ///   on the next loading (if first attempt failed)
      mailPacket.compose ();
      mailPacket.save ("");

      // ToDo: compress to gzip for 25519 address (pboted)
      // ToDo: don't forget, for tests sent uncompressed
      // ToDo: slice big packet after compress
      mailPacket.compress (pbote::Email::CompressionAlgorithm::UNCOMPRESSED);

      if (!mailPacket.empty ())
        {
          emails.push_back (std::make_shared<pbote::Email> (mailPacket));
        }
    }

  LogPrint (eLogDebug, "EmailWorker: checkOutbox: Got ", emails.size ()
            , " email(s)");
  return emails;
}

std::vector<std::shared_ptr<pbote::Email> >
EmailWorker::check_inbox ()
{
  // ToDo: encrypt all local stored emails
  std::string outboxPath = pbote::fs::DataDirPath ("inbox");
  std::vector<std::string> mails_path;
  auto result = pbote::fs::ReadDir (outboxPath, mails_path);

  std::vector<std::shared_ptr<pbote::Email> > emails;

  if (result)
    {
      for (const auto &mail_path : mails_path)
        {
          /// Read mime packet
          std::ifstream file (mail_path, std::ios::binary);
          std::vector<uint8_t> bytes ((std::istreambuf_iterator<char> (file)),
                                      (std::istreambuf_iterator<char> ()));
          file.close ();

          pbote::Email mailPacket;
          mailPacket.fromMIME (bytes);

          if (mailPacket.length () > 0)
            {
              LogPrint (eLogDebug, "EmailWorker: check_inbox: File loaded: ",
                        mail_path);
            }
          else
            {
              LogPrint (eLogWarning,
                        "EmailWorker: check_inbox: Can't read file: ",
                        mail_path);
              continue;
            }

          // ToDo: check signature and set header field

          mailPacket.compose ();
          mailPacket.filename (mail_path);

          if (!mailPacket.empty ())
            emails.push_back (std::make_shared<pbote::Email> (mailPacket));
        }
    }

  LogPrint (eLogDebug, "EmailWorker: check_inbox: Found ", emails.size (),
            " email(s).");

  return emails;
}

std::vector<pbote::Email>
EmailWorker::processEmail (
    const sp_id_full &identity,
    const std::vector<pbote::EmailEncryptedPacket> &mail_packets)
{
  // ToDo: move to incompleteEmailTask?
  LogPrint (eLogDebug, "EmailWorker: processEmail: Emails for process: ",
            mail_packets.size ());
  std::vector<pbote::Email> emails;

  for (auto enc_mail : mail_packets)
    {
      std::vector<uint8_t> unencrypted_email_data;

      if (enc_mail.edata.empty ())
        {
          LogPrint (eLogWarning, "EmailWorker: processEmail: Packet is empty ");
          continue;
        }

      unencrypted_email_data = identity->identity.Decrypt (
          enc_mail.edata.data (), enc_mail.edata.size ());

      if (unencrypted_email_data.empty ())
        {
          LogPrint (eLogWarning, "EmailWorker: processEmail: Can't decrypt ");
          continue;
        }

      pbote::Email temp_mail (unencrypted_email_data, true);

      if (!temp_mail.verify (enc_mail.delete_hash))
        {
          i2p::data::Tag<32> cur_hash (enc_mail.delete_hash);
          LogPrint (eLogWarning, "EmailWorker: processEmail: email ",
                    cur_hash.ToBase64 (), " is unequal");
          continue;
        }

      temp_mail.setEncrypted (enc_mail);

      if (!temp_mail.empty ())
        emails.push_back (temp_mail);
    }

  LogPrint (eLogDebug,
            "EmailWorker: processEmail: Emails processed: ", emails.size ());

  return emails;
}

bool
EmailWorker::check_thread_exist (const std::string &identity_name)
{
  auto it = m_check_threads_.find (identity_name);
  if (it != m_check_threads_.end ())
    return true;

  return false;
}

std::shared_ptr<BoteIdentityPublic>
EmailWorker::parse_address_v0(std::string address)
{
  BoteIdentityPublic identity;
  size_t base64_key_len = 0, offset = 0;

  if (address.length() == ECDH256_ECDSA256_PUBLIC_BASE64_LENGTH)
    {
      identity = BoteIdentityPublic(KEY_TYPE_ECDH256_ECDSA256_SHA256_AES256CBC);
      base64_key_len = ECDH256_ECDSA256_PUBLIC_BASE64_LENGTH / 2;
    }
  else if (address.length() == ECDH521_ECDSA521_PUBLIC_BASE64_LENGTH)
    {
      identity = BoteIdentityPublic(KEY_TYPE_ECDH521_ECDSA521_SHA512_AES256CBC);
      base64_key_len = ECDH521_ECDSA521_PUBLIC_BASE64_LENGTH / 2;
    }
  else
    {
      LogPrint(eLogWarning, "EmailWorker: parse_address_v0: Unsupported identity type");
      return nullptr;
    }

  // Restore keys
  std::string cryptoPublicKey = "A" + address.substr(offset, (base64_key_len));
  offset += (base64_key_len);
  std::string signingPublicKey = "A" + address.substr(offset, (base64_key_len));

  std::string restored_identity_str;
  restored_identity_str.append(cryptoPublicKey);
  restored_identity_str.append(signingPublicKey);

  identity.FromBase64(restored_identity_str);

  LogPrint(eLogDebug, "EmailWorker: parse_address_v0: identity.ToBase64: ",
           identity.ToBase64());
  LogPrint(eLogDebug, "EmailWorker: parse_address_v0: idenhash.ToBase64: ",
           identity.GetIdentHash().ToBase64());

  return std::make_shared<BoteIdentityPublic>(identity);
}

std::shared_ptr<BoteIdentityPublic>
EmailWorker::parse_address_v1(std::string address)
{
  BoteIdentityPublic identity;
  std::string format_prefix = address.substr (0, address.find (".") + 1);
  std::string base_str = address.substr (format_prefix.length ());
  // ToDo: Define length from base32/64
  uint8_t identity_bytes[2048];
  size_t identity_len = 0;

  if (format_prefix.compare (ADDRESS_B32_PREFIX) == 0)
    identity_len = i2p::data::Base32ToByteStream (base_str.c_str (), base_str.length (), identity_bytes, 2048);
  else if (format_prefix.compare (ADDRESS_B64_PREFIX) == 0)
    identity_len = i2p::data::Base64ToByteStream (base_str.c_str (), base_str.length (), identity_bytes, 2048);
  else
    return nullptr;

  if (identity_len < 5)
    {
      LogPrint (eLogError, "identitiesStorage: parse_identity_v1: Malformed address");
      return nullptr;
    }

  if (identity_bytes[0] != ADDRES_FORMAT_V1)
    {
      LogPrint (eLogError, "identitiesStorage: parse_identity_v1: Unsupported address format");
      return nullptr;
    }

  if (identity_bytes[1] == CRYP_TYPE_ECDH256 &&
      identity_bytes[2] == SIGN_TYPE_ECDH256 &&
      identity_bytes[3] == SYMM_TYPE_AES_256 &&
      identity_bytes[4] == HASH_TYPE_SHA_256)
    {
      identity = BoteIdentityPublic(KEY_TYPE_ECDH256_ECDSA256_SHA256_AES256CBC);
    }
  else if (identity_bytes[1] == CRYP_TYPE_ECDH521 &&
           identity_bytes[2] == SIGN_TYPE_ECDH521 &&
           identity_bytes[3] == SYMM_TYPE_AES_256 &&
           identity_bytes[4] == HASH_TYPE_SHA_512)
    {
      identity = BoteIdentityPublic(KEY_TYPE_ECDH521_ECDSA521_SHA512_AES256CBC);
    }
  else if (identity_bytes[1] == CRYP_TYPE_X25519 &&
           identity_bytes[2] == SIGN_TYPE_ED25519 &&
           identity_bytes[3] == SYMM_TYPE_AES_256 &&
           identity_bytes[4] == HASH_TYPE_SHA_512)
    {
      identity = BoteIdentityPublic(KEY_TYPE_X25519_ED25519_SHA512_AES256CBC);
    }

  size_t len = identity.FromBuffer(identity_bytes + 5, identity_len);

  if (len == 0)
    return nullptr;

  LogPrint(eLogDebug, "identitiesStorage: parse_identity_v1: identity.ToBase64: ",
           identity.ToBase64());
  LogPrint(eLogDebug, "identitiesStorage: parse_identity_v1: idenhash.ToBase64: ",
           identity.GetIdentHash().ToBase64());

  return std::make_shared<BoteIdentityPublic>(identity);
}

} // namespace kademlia
} // namespace pbote
