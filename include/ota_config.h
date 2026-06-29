#pragma once

// =============================================================================
// KONFIGURASI FIRMWARE
// =============================================================================
#define FIRMWARE_VERSION "16.1.3"
#define WIFI_CHANNEL 1
#define LED_PIN 2
#define CHUNK_SIZE 200

// Queue sizes
#define CHUNK_QUEUE_SIZE 40
#define REQUEST_QUEUE_SIZE 100
#define REPLY_QUEUE_SIZE 60
#define HAVE_QUEUE_SIZE 40

// P2P parameters
#define CHUNK_INTERVAL 5          // ms: estimasi interval seeding Core
#define MSGHAVE_BASE_MS 200       // ms: base interval broadcast MsgHave
#define MSGHAVE_JITTER_MS 300     // ms: random jitter MsgHave
#define P2P_BATCH_REQUEST 20      // chunk yang direquest per MsgHave
#define FALLBACK_INTERVAL_MS 2000 // ms: interval fallback ke Core

// OTA parameters
#define MAX_FINALIZE_RETRIES 3        // maksimum percobaan ulang finalize
#define REBOOT_WAIT_TIMEOUT_MS 300000 // ms: 5 menit timeout tunggu MsgReboot
#define BITMASK_SAVE_INTERVAL 100     // simpan bitmask ke NVS setiap N chunk