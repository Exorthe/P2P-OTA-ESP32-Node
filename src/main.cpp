#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_ota_ops.h>
#include <Preferences.h>

#include "ota_config.h"
#include "ota_protocol.h"
#include "ota_context.h"
#include "ota_helpers.h"
#include "ota_tasks.h"
#include "cpu_profiler.h"

// =============================================================================
// SNAPSHOT CPU
// =============================================================================
static CPUSnapshot snapIdle;     // baseline murni idle (2 detik setelah boot)
static CPUSnapshot snapOTAStart; // awal OTA (manifest diterima)
static CPUSnapshot snapP2PStart; // akhir seeding / awal P2P
static CPUSnapshot snapFinalize; // akhir P2P / awal finalisasi
static CPUSnapshot snapPostOTA;  // setelah cleanupOTATasks()

// =============================================================================
// DEFINISI VARIABEL GLOBAL
// =============================================================================
volatile NodeState currentState = STATE_IDLE;
OTAContext ota;
QueueHandle_t chunkQueue = nullptr;
QueueHandle_t requestQueue = nullptr;
QueueHandle_t replyQueue = nullptr;
QueueHandle_t haveQueue = nullptr;
TaskHandle_t taskWriteFlashHandle = nullptr;
TaskHandle_t taskRequestHandle = nullptr;
TaskHandle_t taskReplyChunkHandle = nullptr;
TaskHandle_t taskProcessHaveHandle = nullptr;
TaskHandle_t taskP2PHandle = nullptr;
volatile bool manifestPending = false;
MsgManifest pendingManifest;
uint8_t pendingCoreMac[6] = {0};
uint8_t selfMac[6] = {0};
volatile unsigned long lastChunkReceivedMs = 0;
Preferences preferences;

// =============================================================================
// CALLBACK ESP-NOW
// =============================================================================
void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len)
{
    if (len < 1)
        return;
    uint8_t type = data[0];

    // --- MsgManifest (type 0): dari Core langsung ---
    if (type == 0 &&
        currentState == STATE_IDLE &&
        len == sizeof(MsgManifest))
    {
        MsgManifest manifest;
        memcpy(&manifest, data, sizeof(MsgManifest));
        if (String(manifest.version) != String(FIRMWARE_VERSION))
        {
            memcpy(&pendingManifest, &manifest, sizeof(MsgManifest));
            memcpy(pendingCoreMac, mac, 6);
            manifestPending = true;
        }
    }

    // --- MsgRelay (type 1): manifest dari peer untuk out-of-range node ---
    else if (type == 1 && len == sizeof(MsgRelay))
    {
        MsgRelay relay;
        memcpy(&relay, data, sizeof(MsgRelay));

        if (relay.ttl > 0)
        {
            relay.ttl--;
            memcpy(relay.relayerMac, selfMac, 6);
            uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            esp_now_send(broadcast, (uint8_t *)&relay, sizeof(MsgRelay));
            Serial.printf("[NODE] Relay MsgRelay TTL=%d\n", relay.ttl);
        }

        if (currentState == STATE_IDLE &&
            String(relay.version) != String(FIRMWARE_VERSION))
        {
            pendingManifest.type = 0;
            strlcpy(pendingManifest.version, relay.version,
                    sizeof(pendingManifest.version));
            pendingManifest.fileSize = relay.fileSize;
            pendingManifest.totalChunks = relay.totalChunks;
            memcpy(pendingCoreMac, relay.coreMac, 6);
            manifestPending = true;
            Serial.printf("[NODE] MsgRelay dari %s — OTA via Core %s\n",
                          macToString(mac).c_str(),
                          macToString(relay.coreMac).c_str());
        }
    }

    // --- MsgRegister (type 2): relay jika kita bisa reach Core ---
    else if (type == 2 && len == sizeof(MsgRegister))
    {
        MsgRegister reg;
        memcpy(&reg, data, sizeof(MsgRegister));

        if (memcmp(reg.mac, selfMac, 6) == 0)
            return;

        if (ota.coreMac[0] != 0 &&
            memcmp(reg.coreMac, ota.coreMac, 6) == 0)
        {
            addPeerIfNew(ota.coreMac);
            esp_now_send(ota.coreMac, data, len);
            Serial.printf("[NODE] Forward MsgRegister dari %s ke Core\n",
                          macToString(reg.mac).c_str());
        }

        if (reg.ttl > 0)
        {
            reg.ttl--;
            uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            esp_now_send(broadcast, (uint8_t *)&reg, sizeof(MsgRegister));
        }
    }

    // --- MsgChunk (type 5) ---
    else if (type == 5 &&
             (currentState == STATE_RECEIVING || currentState == STATE_P2P) &&
             len == sizeof(MsgChunk))
    {
        if (!chunkQueue)
            return;
        MsgChunk chunk;
        memcpy(&chunk, data, sizeof(MsgChunk));
        if (chunk.chunkIndex >= ota.total)
            return;
        if (!checkBit(chunk.chunkIndex))
        {
            if (memcmp(mac, ota.coreMac, 6) == 0)
                ota.fromCore++;
            else
                ota.fromPeer++;
            lastChunkReceivedMs = millis();
            xQueueSendFromISR(chunkQueue, &chunk, nullptr);
        }
    }

    // --- MsgHave (type 3) ---
    else if (type == 3 &&
             currentState == STATE_P2P &&
             len == sizeof(MsgHave))
    {
        if (!haveQueue)
            return;
        MsgHave have;
        memcpy(&have, data, sizeof(MsgHave));
        addPeerIfNew(have.mac);
        xQueueSendFromISR(haveQueue, &have, nullptr);
    }

    // --- MsgRequestP2P (type 4) ---
    else if (type == 4 && len == sizeof(MsgRequestP2P))
    {
        if (!replyQueue)
            return;
        MsgRequestP2P req;
        memcpy(&req, data, sizeof(MsgRequestP2P));
        PendingReply rep;
        memcpy(rep.mac, req.requesterMac, 6);
        rep.chunkIndex = req.chunkIndex;
        xQueueSendFromISR(replyQueue, &rep, nullptr);
    }

    // --- MsgRelayReport (type 8): forward ke Core jika bisa reach ---
    else if (type == 8 && len == sizeof(MsgRelayReport))
    {
        MsgRelayReport rr;
        memcpy(&rr, data, sizeof(MsgRelayReport));
        if (ota.coreMac[0] != 0 &&
            memcmp(rr.originalMac, selfMac, 6) != 0)
        {
            addPeerIfNew(ota.coreMac);
            esp_now_send(ota.coreMac, data, len);
        }
    }

    // --- MsgReboot (type 10) ---
    else if (type == 10 && len == sizeof(MsgReboot))
    {
        MsgReboot reboot;
        memcpy(&reboot, data, sizeof(MsgReboot));

        if (memcmp(reboot.targetMac, selfMac, 6) == 0)
        {
            Serial.println("[NODE] MsgReboot diterima — siap restart!");
            ota.rebootPermitted = true;
        }
        else if (reboot.ttl > 0)
        {
            reboot.ttl--;
            uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            esp_now_send(broadcast, (uint8_t *)&reboot, sizeof(MsgReboot));
            Serial.printf("[NODE] Relay MsgReboot TTL=%d\n", reboot.ttl);
        }
    }

    // --- MsgAbort (type 11) ---
    else if (type == 11 && len == sizeof(MsgAbort))
    {
        MsgAbort abort;
        memcpy(&abort, data, sizeof(MsgAbort));

        if (abort.ttl > 0)
        {
            abort.ttl--;
            uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            esp_now_send(broadcast, (uint8_t *)&abort, sizeof(MsgAbort));
            Serial.printf("[NODE] Relay MsgAbort TTL=%d\n", abort.ttl);
        }

        if (currentState == STATE_RECEIVING || currentState == STATE_P2P)
        {
            Serial.println("[NODE] MsgAbort diterima — batalkan OTA");
            ota.abortRequested = true;
        }
    }
}

// =============================================================================
// TASK: LED
// =============================================================================
void TaskLED(void *pvParameters)
{
    pinMode(LED_PIN, OUTPUT);
    for (;;)
    {
        digitalWrite(LED_PIN, HIGH);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        digitalWrite(LED_PIN, LOW);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// TASK: MAIN STATE MACHINE 
// =============================================================================
void TaskMain(void *pvParameters)
{
    WiFi.mode(WIFI_STA);
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    WiFi.macAddress(selfMac);

    esp_now_init();
    esp_now_register_recv_cb(OnDataRecv);

    uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    addPeerIfNew(broadcastAddr);

    // -------------------------------------------------------------------------
    // CEK BOOT PASCA-OTA
    // -------------------------------------------------------------------------

    if (preferences.begin("ota", false))
    {
        if (preferences.isKey("coreMac"))
        {
            uint8_t savedMac[6];
            preferences.getBytes("coreMac", savedMac, 6);
            preferences.end();

            Serial.println("[NODE] Boot pasca-OTA — kirim MsgReady");
            memcpy(ota.coreMac, savedMac, 6);
            reportReady();

            esp_ota_mark_app_valid_cancel_rollback();
            Serial.println("[NODE] Firmware ditandai valid");

            preferences.begin("ota", false);
            preferences.remove("coreMac");
            preferences.end();

            memset(ota.coreMac, 0, 6);
        }
        else
        {
            preferences.end();
        }
    }

    // Bersihkan session lama jika versi sudah cocok
    if (preferences.begin("otasess", false))
    {
        if (preferences.isKey("targetVer"))
        {
            String savedVer = preferences.getString("targetVer");
            preferences.end();
            if (savedVer == String(FIRMWARE_VERSION))
            {
                clearOTASession();
                Serial.println("[NVS] Session lama dihapus");
            }
        }
        else
        {
            preferences.end();
        }
    }

    // -------------------------------------------------------------------------
    // - SNAPSHOT 1: BASELINE IDLE
    // Diambil setelah seluruh inisialisasi selesai dan sistem stabil
    // -------------------------------------------------------------------------
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    takeCPUSnapshot(&snapIdle);
    snapP2PStart = snapIdle;
    Serial.println("[PROFILER] - Snapshot IDLE diambil — baseline siap");
    Serial.printf("[NODE] Firmware v%s — siap\n", FIRMWARE_VERSION);

    // -------------------------------------------------------------------------
    // MAIN LOOP STATE MACHINE
    // -------------------------------------------------------------------------
    for (;;)
    {
        switch (currentState)
        {

        // -----------------------------------------------------------------
        // STATE_IDLE
        // -----------------------------------------------------------------
        case STATE_IDLE:
        {
            // Cek interrupted session untuk recovery
            if (loadOTASession())
            {
                Serial.println("[NODE] Recovery session OTA...");

                ota.partition = esp_ota_get_next_update_partition(nullptr);
                if (ota.partition &&
                    esp_ota_begin(ota.partition, OTA_SIZE_UNKNOWN,
                                  &ota.handle) == ESP_OK)
                {
                    addPeerIfNew(ota.coreMac);
                    sendRegister();

                    takeCPUSnapshot(&snapOTAStart);
                    takeCPUSnapshot(&snapP2PStart);
                    Serial.println("[PROFILER] Snapshot OTA_START + P2P_START"
                                   " (recovery path)");

                    initOTATasks();
                    lastChunkReceivedMs = millis();
                    ota.receivingStartMs = millis();

                    xTaskCreatePinnedToCore(TaskP2PHandler, "TaskP2P",
                                            4096, nullptr, 1,
                                            &taskP2PHandle, 1);

                    currentState = STATE_P2P;
                    Serial.printf("[NODE] Recovery berhasil — %d/%d chunk\n",
                                  ota.received, ota.total);
                }
                else
                {
                    ota.reset();
                    clearOTASession();
                    Serial.println("[NODE] Recovery gagal — reset");
                }
                break;
            }

            if (!manifestPending)
                break;

            Serial.println("\n[STATE] STATE_IDLE -> inisialisasi OTA...");

            ota.fileSize = pendingManifest.fileSize;
            ota.total = pendingManifest.totalChunks;
            memcpy(ota.coreMac, pendingCoreMac, 6);
            strlcpy(ota.targetVersion, pendingManifest.version,
                    sizeof(ota.targetVersion));

            uint16_t expectedChunks =
                (ota.fileSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
            if (ota.total != expectedChunks || ota.total == 0)
            {
                Serial.println("[OTA] totalChunks tidak valid!");
                manifestPending = false;
                break;
            }

            if (!initOTA(false))
            {
                manifestPending = false;
                break;
            }

            saveOTASession();
            initOTATasks();
            sendRegister();

            // - SNAPSHOT 2: awal OTA (normal path)
            takeCPUSnapshot(&snapOTAStart);
            printCPUDelta(&snapIdle, &snapOTAStart,
                          "IDLE (baseline sebelum OTA)");
            Serial.println("[PROFILER] Snapshot OTA_START diambil");

            manifestPending = false;
            lastChunkReceivedMs = millis();
            ota.receivingStartMs = millis();
            currentState = STATE_RECEIVING;
            Serial.printf("[STATE] STATE_RECEIVING — %d chunks\n", ota.total);
            break;
        }

        // -----------------------------------------------------------------
        // STATE_RECEIVING
        // -----------------------------------------------------------------
        case STATE_RECEIVING:
        {
            if (ota.abortRequested)
            {
                Serial.println("[NODE] Abort oleh Core (RECEIVING)");
                cleanupOTATasks();
                ota.reset();
                clearOTASession();
                currentState = STATE_IDLE;
                break;
            }

            unsigned long seedingDuration = 5000 + ((unsigned long)ota.total * CHUNK_INTERVAL * 2);

            if (millis() - ota.receivingStartMs > seedingDuration)
            {
                if (ota.received < ota.total)
                {
                    // - SNAPSHOT 3: akhir seeding / awal P2P
                    takeCPUSnapshot(&snapP2PStart);
                    printCPUDelta(&snapOTAStart, &snapP2PStart,
                                  "FASE_SEEDING");
                    Serial.println("[PROFILER] Snapshot P2P_START diambil");

                    xTaskCreatePinnedToCore(TaskP2PHandler, "TaskP2P",
                                            4096, nullptr, 1,
                                            &taskP2PHandle, 1);
                    currentState = STATE_P2P;
                    Serial.printf("[STATE] STATE_P2P — %d/%d chunk\n",
                                  ota.received, ota.total);
                }
            }
            break;
        }

        // -----------------------------------------------------------------
        // STATE_P2P
        // -----------------------------------------------------------------
        case STATE_P2P:
        {
            if (ota.abortRequested)
            {
                Serial.println("[NODE] Abort oleh Core (P2P)");
                cleanupOTATasks();
                ota.reset();
                clearOTASession();
                currentState = STATE_IDLE;
            }
            break;
        }

        // -----------------------------------------------------------------
        // STATE_FINALIZE
        // -----------------------------------------------------------------
        case STATE_FINALIZE:
        {
            // - SNAPSHOT 4: semua chunk diterima / awal finalisasi
            takeCPUSnapshot(&snapFinalize);
            printCPUDelta(&snapP2PStart, &snapFinalize,
                          "FASE_P2P");
            Serial.println("[PROFILER] Snapshot FINALIZE diambil");

            if (taskP2PHandle)
            {
                vTaskDelete(taskP2PHandle);
                taskP2PHandle = nullptr;
            }

            Serial.println("\n[STATE] STATE_FINALIZE");

            if (esp_ota_end(ota.handle) != ESP_OK)
            {
                Serial.println("[OTA] esp_ota_end gagal");
                ota.finalizeRetries++;

                if (ota.finalizeRetries >= MAX_FINALIZE_RETRIES)
                {
                    Serial.println("[OTA] Melebihi batas retry");
                    cleanupOTATasks();
                    reportFail(FAIL_MAX_RETRY_EXCEEDED);
                    ota.reset();
                    clearOTASession();
                    currentState = STATE_IDLE;
                }
                else if (!resetOTAForRetry())
                {
                    cleanupOTATasks();
                    reportFail(FAIL_OTA_END_CHECKSUM);
                    ota.reset();
                    clearOTASession();
                    currentState = STATE_IDLE;
                }
                else
                {
                    // Reset snapP2PStart untuk retry
                    takeCPUSnapshot(&snapP2PStart);
                    currentState = STATE_P2P;
                }
                break;
            }

            if (esp_ota_set_boot_partition(ota.partition) != ESP_OK)
            {
                Serial.println("[OTA] set_boot_partition gagal");
                cleanupOTATasks();
                reportFail(FAIL_SET_BOOT_PARTITION);
                ota.reset();
                clearOTASession();
                currentState = STATE_IDLE;
                break;
            }

            reportSuccess();
            Serial.printf("[OTA] Sukses! Core:%d Peer:%d — tunggu MsgReboot\n",
                          ota.fromCore, ota.fromPeer);

            preferences.begin("ota", false);
            preferences.putBytes("coreMac", ota.coreMac, 6);
            preferences.end();

            clearOTASession();
            cleanupOTATasks();

            vTaskDelay(1000 / portTICK_PERIOD_MS);

            // - SNAPSHOT 5: setelah cleanup selesai
            takeCPUSnapshot(&snapPostOTA);
            printCPUDelta(&snapFinalize, &snapPostOTA,
                          "FINALISASI + CLEANUP");
            printCPUDelta(&snapIdle, &snapPostOTA,
                          "IDLE_VS_POST_OTA (apakah kembali ke baseline?)");
            printStackWatermarks();

            // Tunggu MsgReboot dari Core (staged reboot)
            ota.rebootWaitStart = millis();
            while (!ota.rebootPermitted)
            {
                if (millis() - ota.rebootWaitStart > REBOOT_WAIT_TIMEOUT_MS)
                {
                    Serial.println("[NODE] Timeout MsgReboot — restart mandiri");
                    break;
                }
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }

            vTaskDelay(500 / portTICK_PERIOD_MS);
            esp_restart();
            break;
        }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// SETUP
// =============================================================================
void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.printf("\n[NODE] Firmware v%s\n", FIRMWARE_VERSION);

    xTaskCreatePinnedToCore(TaskLED, "TaskLED", 2048, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(TaskMain, "TaskMain", 10240, nullptr, 1, nullptr, 1);
}

void loop() { vTaskDelete(NULL); }