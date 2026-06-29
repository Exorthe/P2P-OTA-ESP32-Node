#pragma once
#include "ota_context.h"
#include "ota_protocol.h"

// =============================================================================
// FORWARD DECLARATION TASK OTA
// =============================================================================
void TaskWriteFlash(void *pvParameters);
void TaskRequest(void *pvParameters);
void TaskReplyChunk(void *pvParameters);
void TaskProcessHave(void *pvParameters);
void TaskP2PHandler(void *pvParameters);

// =============================================================================
// MANAJEMEN TASK OTA
// =============================================================================

// Buat semua queue dan task OTA saat sesi dimulai
void initOTATasks();

// Hapus semua task dan queue OTA saat sesi selesai/gagal/abort
void cleanupOTATasks();

// Inisialisasi partisi OTA
// recovery=true: jangan erase partisi, gunakan data yang sudah ada di flash
bool initOTA(bool recovery = false);

// Reset OTA untuk retry finalize — erase ulang dan begin ulang
bool resetOTAForRetry();