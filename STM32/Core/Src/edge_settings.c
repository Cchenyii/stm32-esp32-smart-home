#include "edge_settings.h"
#include "edge_home.h"

#define RECORD_COMMITTED UINT32_C(0x45444731)

static uint16_t record_crc(uint32_t sequence, uint32_t packed)
{
    uint8_t bytes[8];
    edge_put32(bytes, sequence);
    edge_put32(bytes + 4, packed);
    return edge_crc16(bytes, sizeof bytes);
}

static bool valid_thresholds(EdgeThresholds value)
{
    return value.fan_on_tenths >= EDGE_MIN_FAN_ON && value.fan_on_tenths <= EDGE_MAX_FAN_ON &&
           value.fan_on_tenths % EDGE_FAN_STEP == 0U &&
           value.close_level >= EDGE_MIN_CLOSE_LEVEL && value.close_level <= EDGE_MAX_CLOSE_LEVEL &&
           value.close_level % EDGE_LIGHT_STEP == 0U;
}

static bool read_record(const volatile uint32_t *record, EdgeThresholds *value)
{
    uint32_t packed = record[1];
    *value = (EdgeThresholds){(uint16_t)packed, (uint16_t)(packed >> 16)};
    return record[3] == RECORD_COMMITTED && record[2] == record_crc(record[0], packed) &&
           valid_thresholds(*value);
}

bool edge_settings_open(EdgeSettings *store, EdgeSettingsFlash flash)
{
    *store = (EdgeSettings){.flash = flash};
    for (unsigned page = 0; page < 2; ++page) {
        for (size_t index = 0; index < flash.capacity; ++index) {
            const volatile uint32_t *record = flash.pages[page][index];
            if ((record[0] & record[1] & record[2] & record[3]) == UINT32_MAX) {
                continue;
            }
            store->next[page] = index + 1;
            EdgeThresholds value;
            uint32_t sequence = record[0];
            uint32_t distance = sequence - store->sequence;
            if (read_record(record, &value) &&
                (!store->valid || (distance != 0U && distance < UINT32_C(0x80000000)))) {
                store->current = value;
                store->sequence = sequence;
                store->active = page;
                store->valid = true;
            }
        }
    }

    unsigned spare = store->active ^ 1U;
    bool both_full = store->next[0] == flash.capacity && store->next[1] == flash.capacity;
    /* An entire sector erase stalls this Flash bank. Do it only at startup. */
    if ((store->valid || both_full) && store->next[spare] != 0U) {
        if (!flash.erase(spare)) {
            return false;
        }
        store->next[spare] = 0;
    }
    return true;
}

bool edge_settings_save(EdgeSettings *store, EdgeThresholds thresholds)
{
    if (!valid_thresholds(thresholds)) {
        return false;
    }
    if (store->valid && thresholds.fan_on_tenths == store->current.fan_on_tenths &&
        thresholds.close_level == store->current.close_level) {
        return true;
    }

    unsigned page = store->active;
    if (store->next[page] == store->flash.capacity) {
        page ^= 1U;
    }
    if (store->next[page] == store->flash.capacity) {
        return false;
    }
    size_t index = store->next[page]++;
    uint32_t sequence = store->sequence + 1U;
    uint32_t packed = ((uint32_t)thresholds.close_level << 16) | thresholds.fan_on_tenths;
    EdgeSettingsRecord record = {sequence, packed, record_crc(sequence, packed), RECORD_COMMITTED};
    if (!store->flash.program(page, index, record)) {
        return false;
    }
    for (unsigned word = 0; word < EDGE_SETTINGS_WORDS; ++word) {
        if (store->flash.pages[page][index][word] != record[word]) {
            return false;
        }
    }
    store->current = thresholds;
    store->sequence = sequence;
    store->active = page;
    store->valid = true;
    return true;
}
