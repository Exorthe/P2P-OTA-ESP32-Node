#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define CPU_PROFILER_MAX_TASKS 20

// =============================================================================
// STRUCT SNAPSHOT
// =============================================================================
struct CPUSnapshot
{
    TaskStatus_t tasks[CPU_PROFILER_MAX_TASKS];
    UBaseType_t taskCount;
    uint32_t totalRunTime;
    unsigned long wallTime;
};

// =============================================================================
// AMBIL SNAPSHOT
// =============================================================================
inline void takeCPUSnapshot(CPUSnapshot *snap)
{
    snap->taskCount = uxTaskGetSystemState(
        snap->tasks,
        CPU_PROFILER_MAX_TASKS,
        &snap->totalRunTime);
    snap->wallTime = millis();
}

// =============================================================================
// CETAK DELTA ANTARA DUA SNAPSHOT
// =============================================================================
inline void printCPUDelta(const CPUSnapshot *before,
                          const CPUSnapshot *after,
                          const char *label)
{
    uint32_t totalDelta = after->totalRunTime - before->totalRunTime;
    unsigned long durationMs = after->wallTime - before->wallTime;

    if (totalDelta == 0)
    {
        Serial.printf("[PROFILER] %s: tidak ada aktivitas CPU\n", label);
        return;
    }

    Serial.printf("\n");
    Serial.printf("================================================\n");
    Serial.printf("CPU PROFILER : %s\n", label);
    Serial.printf("Durasi       : %lu ms\n", durationMs);
    Serial.printf("------------------------------------------------\n");
    Serial.printf("%-20s %12s %5s %5s\n",
                  "Task", "Cycles", "%", "Core");
    Serial.printf("------------------------------------------------\n");

    for (UBaseType_t i = 0; i < after->taskCount; i++)
    {
        uint32_t beforeTime = 0;
        bool isNewTask = true;

        for (UBaseType_t j = 0; j < before->taskCount; j++)
        {
            if (before->tasks[j].xHandle ==
                after->tasks[i].xHandle)
            {
                beforeTime = before->tasks[j].ulRunTimeCounter;
                isNewTask = false;
                break;
            }
        }

        uint32_t delta = after->tasks[i].ulRunTimeCounter - beforeTime;
        uint32_t pct = (uint32_t)((uint64_t)delta * 100ULL / totalDelta);

        Serial.printf("%-18s%s %12u %4u%% %5d\n",
                      after->tasks[i].pcTaskName,
                      isNewTask ? "*" : " ",
                      delta,
                      pct,
                      after->tasks[i].xCoreID == tskNO_AFFINITY
                          ? -1
                          : (int)after->tasks[i].xCoreID);
    }

    Serial.printf("------------------------------------------------\n");
    Serial.printf("* = task baru sejak snapshot before\n");
    Serial.printf("Total cycles : %u\n", totalDelta);
    Serial.printf("================================================\n\n");
}

// =============================================================================
// CETAK STACK HIGH WATER MARK
// =============================================================================
inline void printStackWatermarks()
{
    TaskStatus_t tasks[CPU_PROFILER_MAX_TASKS];
    UBaseType_t count = uxTaskGetSystemState(tasks, CPU_PROFILER_MAX_TASKS, nullptr);

    Serial.printf("\n");
    Serial.printf("================================================\n");
    Serial.printf("STACK HIGH WATER MARK\n");
    Serial.printf("(nilai kecil = mendekati overflow)\n");
    Serial.printf("------------------------------------------------\n");
    Serial.printf("%-20s %10s %5s\n", "Task", "Min Sisa", "Core");
    Serial.printf("------------------------------------------------\n");

    for (UBaseType_t i = 0; i < count; i++)
    {
        const char *warn =
            tasks[i].usStackHighWaterMark < 512 ? " !" : "";
        Serial.printf("%-20s %10u %5d%s\n",
                      tasks[i].pcTaskName,
                      tasks[i].usStackHighWaterMark,
                      tasks[i].xCoreID == tskNO_AFFINITY
                          ? -1
                          : (int)tasks[i].xCoreID,
                      warn);
    }

    Serial.printf("------------------------------------------------\n");
    Serial.printf("! = sisa stack < 512 byte\n");
    Serial.printf("================================================\n\n");
}