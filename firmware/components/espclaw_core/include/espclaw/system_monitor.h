#ifndef ESPCLAW_SYSTEM_MONITOR_H
#define ESPCLAW_SYSTEM_MONITOR_H

#include <stdbool.h>
#include <stddef.h>

#include "espclaw/board_profile.h"

#define ESPCLAW_SYSTEM_MONITOR_MAX_CORES 2

typedef struct {
    bool available;
    unsigned int cpu_cores;
    bool dual_core;
    unsigned int cpu_mhz;
    size_t flash_chip_size_bytes;
    size_t app_partition_size_bytes;
    size_t app_image_size_bytes;
    size_t workspace_total_bytes;
    size_t workspace_used_bytes;
    size_t ram_total_bytes;
    size_t ram_free_bytes;
    size_t ram_min_free_bytes;
    size_t ram_largest_free_block_bytes;
    unsigned int cpu_load_percent[ESPCLAW_SYSTEM_MONITOR_MAX_CORES];
} espclaw_system_monitor_snapshot_t;

int espclaw_system_monitor_init(const espclaw_board_profile_t *profile);
int espclaw_system_monitor_snapshot(
    const espclaw_board_profile_t *profile,
    size_t workspace_total_bytes,
    size_t workspace_used_bytes,
    espclaw_system_monitor_snapshot_t *snapshot
);

#endif
