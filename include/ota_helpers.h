#pragma once
#include <Arduino.h>
#include <esp_now.h>
#include <Preferences.h>
#include <esp_crc.h>
#include "ota_context.h"
#include "ota_protocol.h"
#include "ota_config.h"

extern Preferences preferences;

// =============================================================================
// HELPER UMUM
// =============================================================================
inline String macToString(const uint8_t *mac)
{
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

inline void addPeerIfNew(const uint8_t *mac)
{
    if (!esp_now_is_peer_exist(mac))
    {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, mac, 6);
        peer.channel = WIFI_CHANNEL;
        esp_now_add_peer(&peer);
    }
}

inline void setBit(int index)
{
    if (ota.bitmask)
        ota.bitmask[index / 8] |= (1 << (index % 8));
}

inline bool checkBit(int index)
{
    if (!ota.bitmask)
        return false;
    return (ota.bitmask[index / 8] & (1 << (index % 8))) != 0;
}

// =============================================================================
// FUNGSI REPORT KE CORE + RELAY
// =============================================================================
inline void reportFail(OTAFailReason reason)
{
    MsgFail fail;
    fail.type = 7;
    fail.reason = reason;
    memcpy(fail.mac, selfMac, 6);
    addPeerIfNew(ota.coreMac);
    esp_now_send(ota.coreMac, (uint8_t *)&fail, sizeof(MsgFail));

    uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    MsgRelayReport relay;
    relay.type = 8;
    memcpy(relay.relayerMac, selfMac, 6);
    memcpy(relay.originalMac, selfMac, 6);
    relay.reportType = 7;
    relay.chunksFromCore = 0;
    relay.chunksFromPeer = 0;
    relay.reason = reason;
    esp_now_send(broadcast, (uint8_t *)&relay, sizeof(MsgRelayReport));
}

inline void reportSuccess()
{
    MsgSuccess success;
    success.type = 6;
    success.chunksFromCore = ota.fromCore;
    success.chunksFromPeer = ota.fromPeer;
    success.chunksSentToPeer = ota.sentToPeer;
    memcpy(success.mac, selfMac, 6);
    addPeerIfNew(ota.coreMac);
    esp_now_send(ota.coreMac, (uint8_t *)&success, sizeof(MsgSuccess));

    uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    MsgRelayReport relay;
    relay.type = 8;
    memcpy(relay.relayerMac, selfMac, 6);
    memcpy(relay.originalMac, selfMac, 6);
    relay.reportType = 6;
    relay.chunksFromCore = ota.fromCore;
    relay.chunksFromPeer = ota.fromPeer;
    relay.chunksSentToPeer = ota.sentToPeer;
    relay.reason = (OTAFailReason)0;
    esp_now_send(broadcast, (uint8_t *)&relay, sizeof(MsgRelayReport));
}

inline void reportReady()
{
    MsgReady ready;
    ready.type = 9;
    memcpy(ready.mac, selfMac, 6);
    addPeerIfNew(ota.coreMac);
    for (int i = 0; i < 3; i++)
    {
        esp_now_send(ota.coreMac, (uint8_t *)&ready, sizeof(MsgReady));
        delay(200);
    }

    uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    MsgRelayReport relay;
    relay.type = 8;
    memcpy(relay.relayerMac, selfMac, 6);
    memcpy(relay.originalMac, selfMac, 6);
    relay.reportType = 9;
    relay.chunksFromCore = 0;
    relay.chunksFromPeer = 0;
    relay.reason = (OTAFailReason)0;
    esp_now_send(broadcast, (uint8_t *)&relay, sizeof(MsgRelayReport));
}

// =============================================================================
// KIRIM MSGREGISTER KE CORE
// =============================================================================
inline void sendRegister()
{
    MsgRegister reg;
    reg.type = 2;
    memcpy(reg.mac, selfMac, 6);
    memcpy(reg.coreMac, ota.coreMac, 6);
    reg.ttl = 2; // boleh diteruskan hingga dua kali

    // Broadcast agar node perantara bisa relay ke Core
    // jika kita tidak bisa reach Core langsung
    uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_send(broadcast, (uint8_t *)&reg, sizeof(MsgRegister));
    Serial.println("[NODE] MsgRegister broadcast (TTL=2)");
}
inline void serveChunkToPeer(const uint8_t *peerMac, uint16_t chunkIndex)
{
    if (!ota.bitmask || !checkBit(chunkIndex) || !ota.partition)
        return;

    uint8_t dataLen = (uint8_t)min(
        (size_t)CHUNK_SIZE,
        ota.fileSize - (size_t)chunkIndex * CHUNK_SIZE);

    MsgChunk msg;
    msg.type = 5;
    msg.chunkIndex = chunkIndex;
    msg.dataLen = dataLen;
    esp_partition_read(ota.partition,
                       (size_t)chunkIndex * CHUNK_SIZE,
                       msg.data, dataLen);
    msg.crc32 = esp_crc32_le(0, msg.data, dataLen);

    addPeerIfNew(peerMac);
    esp_now_send(peerMac, (uint8_t *)&msg, sizeof(MsgChunk));
    ota.sentToPeer++;
}

// =============================================================================
// NVS SESSION MANAGEMENT
// =============================================================================
inline void saveOTASession()
{
    if (!ota.bitmask || ota.total == 0)
        return;
    int bitmaskSize = (ota.total + 7) / 8;
    preferences.begin("otasess", false);
    preferences.putString("targetVer", String(ota.targetVersion));
    preferences.putUInt("size", ota.fileSize);
    preferences.putUShort("total", ota.total);
    preferences.putBytes("mac", ota.coreMac, 6);
    preferences.putBytes("bitmask", ota.bitmask, bitmaskSize);
    preferences.end();
    ota.lastSavedCount = ota.received;
}

inline bool loadOTASession()
{
    preferences.begin("otasess", true);
    bool hasSession = preferences.isKey("targetVer");
    if (!hasSession)
    {
        preferences.end();
        return false;
    }

    String targetVer = preferences.getString("targetVer");
    if (targetVer == String(FIRMWARE_VERSION))
    {
        preferences.end();
        return false;
    }

    ota.fileSize = preferences.getUInt("size", 0);
    ota.total = preferences.getUShort("total", 0);
    if (ota.fileSize == 0 || ota.total == 0)
    {
        preferences.end();
        return false;
    }

    strlcpy(ota.targetVersion, targetVer.c_str(), sizeof(ota.targetVersion));
    preferences.getBytes("mac", ota.coreMac, 6);

    int bitmaskSize = (ota.total + 7) / 8;
    ota.bitmask = (uint8_t *)calloc(bitmaskSize, 1);
    if (!ota.bitmask)
    {
        preferences.end();
        return false;
    }
    preferences.getBytes("bitmask", ota.bitmask, bitmaskSize);

    ota.received = 0;
    for (int i = 0; i < ota.total; i++)
    {
        if (checkBit(i))
            ota.received++;
    }

    preferences.end();
    Serial.printf("[NVS] Session recovery: v%s, %d/%d chunk\n",
                  ota.targetVersion, ota.received, ota.total);
    return true;
}

inline void clearOTASession()
{
    preferences.begin("otasess", false);
    preferences.clear();
    preferences.end();
}