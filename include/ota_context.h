#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include "ota_protocol.h"

// =============================================================================
// STATE MACHINE
// =============================================================================
enum NodeState {
    STATE_IDLE,
    STATE_RECEIVING,
    STATE_P2P,
    STATE_FINALIZE
};
extern volatile NodeState currentState;

// =============================================================================
// KONTEKS OTA
// =============================================================================
struct OTAContext {
    const esp_partition_t* partition        = nullptr;
    esp_ota_handle_t       handle           = 0;
    uint8_t*               bitmask          = nullptr;
    volatile uint16_t      received         = 0;
    uint16_t               total            = 0;
    size_t                 fileSize         = 0;
    uint8_t                coreMac[6]       = {0};
    char                   targetVersion[10]= {0};
    volatile uint16_t      fromCore         = 0;
    volatile uint16_t      fromPeer         = 0;
    volatile uint16_t sentToPeer       = 0; 
    unsigned long          receivingStartMs = 0;
    int                    finalizeRetries  = 0;
    volatile bool          rebootPermitted  = false;
    unsigned long          rebootWaitStart  = 0;
    volatile bool          abortRequested   = false;
    uint16_t               lastSavedCount   = 0;

    void reset() {
        if (bitmask) { free(bitmask); bitmask = nullptr; }
        partition        = nullptr;
        handle           = 0;
        received         = 0;
        total            = 0;
        fileSize         = 0;
        fromCore         = 0;
        fromPeer         = 0;
        sentToPeer       = 0;  // ← TAMBAH
        receivingStartMs = 0;
        finalizeRetries  = 0;
        rebootPermitted  = false;
        rebootWaitStart  = 0;
        abortRequested   = false;
        lastSavedCount   = 0;
        memset(coreMac, 0, 6);
        memset(targetVersion, 0, sizeof(targetVersion));
    }
};
extern OTAContext ota;

// =============================================================================
// QUEUE HANDLES — dibuat dinamis saat OTA dimulai
// =============================================================================
extern QueueHandle_t chunkQueue;
extern QueueHandle_t requestQueue;
extern QueueHandle_t replyQueue;
extern QueueHandle_t haveQueue;

// =============================================================================
// TASK HANDLES OTA — dibuat dinamis saat OTA dimulai
// =============================================================================
extern TaskHandle_t taskWriteFlashHandle;
extern TaskHandle_t taskRequestHandle;
extern TaskHandle_t taskReplyChunkHandle;
extern TaskHandle_t taskProcessHaveHandle;
extern TaskHandle_t taskP2PHandle;

// =============================================================================
// VARIABEL GLOBAL LAIN
// =============================================================================
extern volatile bool  manifestPending;
extern MsgManifest    pendingManifest;
extern uint8_t        pendingCoreMac[6];
extern uint8_t        selfMac[6];
extern volatile unsigned long lastChunkReceivedMs;