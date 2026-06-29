#pragma once
#include <stdint.h>
#include "ota_config.h"

// =============================================================================
// ENUM FAIL REASON
// =============================================================================
enum OTAFailReason : uint8_t
{
    FAIL_OTA_END_CHECKSUM = 1,    // esp_ota_end() checksum gagal
    FAIL_SET_BOOT_PARTITION = 2,  // esp_ota_set_boot_partition() gagal
    FAIL_FLASH_WRITE = 3,         // penulisan flash berulang kali gagal
    FAIL_MAX_RETRY_EXCEEDED = 4,  // melebihi batas retry finalize
    FAIL_PARTITION_NOT_FOUND = 5, // partisi OTA tidak ditemukan
    FAIL_OUT_OF_MEMORY = 6,       // calloc gagal
    FAIL_ABORTED_BY_CORE = 7      // Core kirim MsgAbort
};

// =============================================================================
// STRUCT PROTOKOL - type 0–11
// =============================================================================
typedef struct __attribute__((packed))
{
    uint8_t type; // 0: MsgManifest
    char version[10];
    uint32_t fileSize;
    uint16_t totalChunks;
} MsgManifest;

typedef struct __attribute__((packed))
{
    uint8_t type; // 1: MsgRelay
    uint8_t relayerMac[6];
    char version[10];
    uint32_t fileSize;
    uint16_t totalChunks;
    uint8_t coreMac[6];
    uint8_t ttl; // Time-To-Live relay
} MsgRelay;

typedef struct __attribute__((packed))
{
    uint8_t type;       // 2: MsgRegister
    uint8_t mac[6];     // MAC node pengirim asli
    uint8_t coreMac[6]; // MAC Core Node tujuan (untuk relay)
    uint8_t ttl;        // Time-To-Live relay
} MsgRegister;

typedef struct __attribute__((packed))
{
    uint8_t type; // 3: MsgHave
    uint8_t mac[6];
    uint16_t startChunk;
    uint8_t bitfield[40];
} MsgHave;

typedef struct __attribute__((packed))
{
    uint8_t type; // 4: MsgRequestP2P
    uint8_t requesterMac[6];
    uint16_t chunkIndex;
} MsgRequestP2P;

typedef struct __attribute__((packed))
{
    uint8_t type; // 5: MsgChunk
    uint16_t chunkIndex;
    uint8_t dataLen;
    uint32_t crc32;
    uint8_t data[CHUNK_SIZE];
} MsgChunk;

typedef struct __attribute__((packed))
{
    uint8_t type; // 6: MsgSuccess
    uint8_t mac[6];
    uint16_t chunksFromCore;
    uint16_t chunksFromPeer;
    uint16_t chunksSentToPeer;
} MsgSuccess;

typedef struct __attribute__((packed))
{
    uint8_t type; // 7: MsgFail
    uint8_t mac[6];
    OTAFailReason reason;
} MsgFail;

typedef struct __attribute__((packed))
{
    uint8_t type; // 8: MsgRelayReport
    uint8_t relayerMac[6];
    uint8_t originalMac[6];
    uint8_t reportType; // 6=Success, 7=Fail, 9=Ready
    uint16_t chunksFromCore;
    uint16_t chunksFromPeer;
    uint16_t chunksSentToPeer;
    OTAFailReason reason;
} MsgRelayReport;

typedef struct __attribute__((packed))
{
    uint8_t type; // 9: MsgReady
    uint8_t mac[6];
} MsgReady;

typedef struct __attribute__((packed))
{
    uint8_t type;         // 10: MsgReboot
    uint8_t targetMac[6]; // MAC node yang diizinkan reboot
    uint8_t ttl;          // Time-To-Live relay: 1 = boleh relay sekali
} MsgReboot;

typedef struct __attribute__((packed))
{
    uint8_t type; // 11: MsgAbort
    uint8_t ttl;  // Time-To-Live relay: 1 = boleh relay sekali
} MsgAbort;

// =============================================================================
// STRUCT INTERNAL
// =============================================================================
struct PendingRequest
{
    uint8_t mac[6];
    uint16_t chunkIndex;
};
struct PendingReply
{
    uint8_t mac[6];
    uint16_t chunkIndex;
};