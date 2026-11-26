#ifndef EDGE_SETTINGS_H
#define EDGE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t fan_on_tenths;
    uint16_t close_level;
} EdgeThresholds;

enum { EDGE_SETTINGS_WORDS = 4 };
typedef uint32_t EdgeSettingsRecord[EDGE_SETTINGS_WORDS];

typedef struct {
    const volatile EdgeSettingsRecord *pages[2];
    size_t capacity;
    bool (*program)(unsigned page, size_t index, const EdgeSettingsRecord record);
    bool (*erase)(unsigned page);
} EdgeSettingsFlash;

typedef struct {
    EdgeSettingsFlash flash;
    size_t next[2];
    EdgeThresholds current;
    uint32_t sequence;
    unsigned active;
    bool valid;
} EdgeSettings;

/* Scans both sectors and, if necessary, prepares a spare before workers start. */
bool edge_settings_open(EdgeSettings *store, EdgeSettingsFlash flash);
bool edge_settings_save(EdgeSettings *store, EdgeThresholds thresholds);
EdgeSettingsFlash edge_settings_flash(void);

#endif
