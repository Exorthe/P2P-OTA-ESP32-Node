#include "ota_tasks.h"
#include "ota_helpers.h"
#include "ota_config.h"
#include <esp_crc.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

// =============================================================================
// MANAJEMEN TASK OTA
// =============================================================================
void initOTATasks()
{
    chunkQueue = xQueueCreate(CHUNK_QUEUE_SIZE, sizeof(MsgChunk));
    requestQueue = xQueueCreate(REQUEST_QUEUE_SIZE, sizeof(PendingRequest));
    replyQueue = xQueueCreate(REPLY_QUEUE_SIZE, sizeof(PendingReply));
    haveQueue = xQueueCreate(HAVE_QUEUE_SIZE, sizeof(MsgHave));

    // Core 0: TaskWriteFlash (isolasi I/O flash dari radio di Core 1)
    xTaskCreatePinnedToCore(TaskWriteFlash, "TaskFlash", 8192, nullptr, 1,
                            &taskWriteFlashHandle, 0);
    // Core 1: task radio dan P2P
    xTaskCreatePinnedToCore(TaskRequest, "TaskReq", 4096, nullptr, 1,
                            &taskRequestHandle, 1);
    xTaskCreatePinnedToCore(TaskReplyChunk, "TaskReply", 8192, nullptr, 2,
                            &taskReplyChunkHandle, 1);
    xTaskCreatePinnedToCore(TaskProcessHave, "TaskHave", 4096, nullptr, 1,
                            &taskProcessHaveHandle, 1);

    Serial.println("[OTA] Task OTA dibuat");
}

void cleanupOTATasks()
{
    if (taskP2PHandle)
    {
        vTaskDelete(taskP2PHandle);
        taskP2PHandle = nullptr;
    }
    if (taskProcessHaveHandle)
    {
        vTaskDelete(taskProcessHaveHandle);
        taskProcessHaveHandle = nullptr;
    }
    if (taskReplyChunkHandle)
    {
        vTaskDelete(taskReplyChunkHandle);
        taskReplyChunkHandle = nullptr;
    }
    if (taskRequestHandle)
    {
        vTaskDelete(taskRequestHandle);
        taskRequestHandle = nullptr;
    }
    if (taskWriteFlashHandle)
    {
        vTaskDelete(taskWriteFlashHandle);
        taskWriteFlashHandle = nullptr;
    }

    if (haveQueue)
    {
        vQueueDelete(haveQueue);
        haveQueue = nullptr;
    }
    if (requestQueue)
    {
        vQueueDelete(requestQueue);
        requestQueue = nullptr;
    }
    if (replyQueue)
    {
        vQueueDelete(replyQueue);
        replyQueue = nullptr;
    }
    if (chunkQueue)
    {
        vQueueDelete(chunkQueue);
        chunkQueue = nullptr;
    }

    Serial.println("[OTA] Task OTA dihapus");
}

bool initOTA(bool recovery)
{
    ota.partition = esp_ota_get_next_update_partition(nullptr);
    if (!ota.partition)
    {
        Serial.println("[OTA] Partisi tidak ditemukan!");
        reportFail(FAIL_PARTITION_NOT_FOUND);
        return false;
    }

    if (!recovery)
    {
        size_t eraseSize = (ota.fileSize + 4095) & ~4095;
        if (eraseSize > ota.partition->size)
        {
            Serial.println("[OTA] Firmware terlalu besar untuk partisi!");
            reportFail(FAIL_PARTITION_NOT_FOUND);
            return false;
        }
        Serial.println("[OTA] Menghapus partisi...");
        esp_partition_erase_range(ota.partition, 0, eraseSize);
    }
    else
    {
        Serial.println("[OTA] Recovery mode — partisi tidak dierase");
    }

    if (esp_ota_begin(ota.partition, OTA_SIZE_UNKNOWN, &ota.handle) != ESP_OK)
    {
        Serial.println("[OTA] esp_ota_begin gagal!");
        return false;
    }

    if (!recovery)
    {
        int bitmaskSize = (ota.total + 7) / 8;
        ota.bitmask = (uint8_t *)calloc(bitmaskSize, 1);
        if (!ota.bitmask)
        {
            Serial.println("[OTA] calloc gagal — memori tidak cukup!");
            esp_ota_abort(ota.handle);
            reportFail(FAIL_OUT_OF_MEMORY);
            return false;
        }
    }

    addPeerIfNew(ota.coreMac);
    return true;
}

bool resetOTAForRetry()
{
    Serial.printf("[OTA] Retry finalize #%d\n", ota.finalizeRetries);

    int bitmaskSize = (ota.total + 7) / 8;
    memset(ota.bitmask, 0, bitmaskSize);
    ota.received = 0;
    ota.fromCore = 0;
    ota.fromPeer = 0;

    size_t eraseSize = (ota.fileSize + 4095) & ~4095;
    esp_partition_erase_range(ota.partition, 0, eraseSize);
    if (esp_ota_begin(ota.partition, OTA_SIZE_UNKNOWN, &ota.handle) != ESP_OK)
    {
        Serial.println("[OTA] esp_ota_begin gagal saat retry!");
        return false;
    }

    if (!taskP2PHandle)
    {
        xTaskCreatePinnedToCore(TaskP2PHandler, "TaskP2P",
                                4096, nullptr, 1, &taskP2PHandle, 1);
    }

    lastChunkReceivedMs = millis();
    return true;
}

// =============================================================================
// TASK: WRITE FLASH — Core 0
// =============================================================================
void TaskWriteFlash(void *pvParameters)
{
    MsgChunk msg;
    for (;;)
    {
        if (xQueueReceive(chunkQueue, &msg, portMAX_DELAY) != pdPASS)
            continue;
        if (!ota.bitmask || checkBit(msg.chunkIndex))
            continue;

        // Validasi CRC
        if (esp_crc32_le(0, msg.data, msg.dataLen) != msg.crc32)
        {
            Serial.printf("[FLASH] CRC gagal chunk %d\n", msg.chunkIndex);
            continue;
        }

        // Tulis langsung ke flash — cek return value
        esp_err_t err = esp_ota_write_with_offset(
            ota.handle,
            msg.data,
            msg.dataLen,
            (size_t)msg.chunkIndex * CHUNK_SIZE);

        if (err != ESP_OK)
        {
            Serial.printf("[FLASH] Write gagal chunk %d (err %d)\n",
                          msg.chunkIndex, err);
            continue;
        }

        setBit(msg.chunkIndex);
        ota.received++;

        // Checkpoint bitmask ke NVS setiap N chunk
        if (ota.received - ota.lastSavedCount >= BITMASK_SAVE_INTERVAL)
        {
            saveOTASession();
        }

        if (ota.received % 50 == 0 || ota.received == ota.total)
            Serial.printf("[FLASH] %d/%d (%.1f%%)\n",
                          ota.received, ota.total,
                          100.0f * ota.received / ota.total);

        if (ota.received >= ota.total)
            currentState = STATE_FINALIZE;
    }
}

// =============================================================================
// TASK: PROCESS HAVE
// =============================================================================
void TaskProcessHave(void *pvParameters)
{
    MsgHave have;
    for (;;)
    {
        if (xQueueReceive(haveQueue, &have, portMAX_DELAY) != pdPASS)
            continue;

        int requestCount = 0;
        bool done = false;
        for (int i = 0; i < 40 && requestCount < P2P_BATCH_REQUEST && !done; i++)
        {
            for (int bit = 0; bit < 8 && requestCount < P2P_BATCH_REQUEST; bit++)
            {
                uint16_t idx = have.startChunk + (i * 8) + bit;
                if (idx >= ota.total)
                {
                    done = true;
                    break;
                }
                if ((have.bitfield[i] & (1 << bit)) && !checkBit(idx))
                {
                    PendingRequest req;
                    memcpy(req.mac, have.mac, 6);
                    req.chunkIndex = idx;
                    xQueueSend(requestQueue, &req, 0);
                    requestCount++;
                }
            }
        }
    }
}

// =============================================================================
// TASK: REQUEST
// =============================================================================
void TaskRequest(void *pvParameters)
{
    PendingRequest req;
    MsgRequestP2P p2pReq;
    p2pReq.type = 4;
    memcpy(p2pReq.requesterMac, selfMac, 6);

    for (;;)
    {
        if (xQueueReceive(requestQueue, &req, portMAX_DELAY) != pdPASS)
            continue;
        if (!ota.bitmask || checkBit(req.chunkIndex))
            continue;

        p2pReq.chunkIndex = req.chunkIndex;
        addPeerIfNew(req.mac);
        esp_now_send(req.mac, (uint8_t *)&p2pReq, sizeof(MsgRequestP2P));
        vTaskDelay(5 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// TASK: REPLY CHUNK
// =============================================================================
void TaskReplyChunk(void *pvParameters)
{
    PendingReply rep;
    for (;;)
    {
        if (xQueueReceive(replyQueue, &rep, portMAX_DELAY) != pdPASS)
            continue;
        serveChunkToPeer(rep.mac, rep.chunkIndex);
        vTaskDelay(5 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// TASK: P2P HANDLER
// =============================================================================
void TaskP2PHandler(void *pvParameters)
{
    unsigned long lastHaveMs = 0;
    unsigned long lastFallbackMs = millis();
    uint16_t windowStart = 0;
    uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    // Cache MAC sekali di awal task
    uint8_t myMac[6];
    memcpy(myMac, selfMac, 6);

    // Relay MsgManifest sekali saat task pertama kali dibuat
    MsgRelay relay;
    relay.type = 1;
    memcpy(relay.relayerMac, myMac, 6);
    strlcpy(relay.version, ota.targetVersion, sizeof(relay.version));
    relay.fileSize = ota.fileSize;
    relay.totalChunks = ota.total;
    memcpy(relay.coreMac, ota.coreMac, 6);
    esp_now_send(broadcast, (uint8_t *)&relay, sizeof(MsgRelay));
    Serial.println("[P2P] MsgRelay broadcast untuk out-of-range node");

    unsigned long currentInterval = MSGHAVE_BASE_MS + (esp_random() % MSGHAVE_JITTER_MS);

    for (;;)
    {
        // --- Fallback ke Core jika tidak ada chunk masuk ---
        if (millis() - lastChunkReceivedMs > FALLBACK_INTERVAL_MS &&
            millis() - lastFallbackMs > FALLBACK_INTERVAL_MS)
        {

            uint16_t start = esp_random() % ota.total;
            int requestCount = 0;

            for (uint16_t j = 0; j < ota.total && requestCount < 5; j++)
            {
                uint16_t i = (start + j) % ota.total;
                if (!checkBit(i))
                {
                    PendingRequest req;
                    memcpy(req.mac, ota.coreMac, 6);
                    req.chunkIndex = i;
                    if (requestQueue)
                        xQueueSend(requestQueue, &req, 0);
                    requestCount++;
                }
            }
            lastFallbackMs = millis();
        }

        // --- Broadcast MsgHave dengan sliding window ---
        if (millis() - lastHaveMs < currentInterval)
        {
            vTaskDelay(50 / portTICK_PERIOD_MS);
            continue;
        }

        MsgHave have;
        have.type = 3;
        have.startChunk = windowStart;
        memcpy(have.mac, myMac, 6);
        memset(have.bitfield, 0, sizeof(have.bitfield));

        uint16_t windowEnd = min((uint16_t)(windowStart + 320), ota.total);
        for (uint16_t i = windowStart; i < windowEnd; i++)
        {
            if (checkBit(i))
            {
                int rel = i - windowStart;
                have.bitfield[rel / 8] |= (1 << (rel % 8));
            }
        }

        esp_now_send(broadcast, (uint8_t *)&have, sizeof(MsgHave));
        lastHaveMs = millis();

        windowStart += 320;
        if (windowStart >= ota.total)
            windowStart = 0;

        currentInterval = MSGHAVE_BASE_MS + (esp_random() % MSGHAVE_JITTER_MS);

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}