#include "edge_settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { PAGE_RECORDS = 3 };
static EdgeSettingsRecord pages[2][PAGE_RECORDS];
static unsigned words_to_write = EDGE_SETTINGS_WORDS;
static unsigned program_count;
static bool erase_enabled = true;

static bool program(unsigned page, size_t index, const EdgeSettingsRecord record)
{
    ++program_count;
    for (unsigned word = 0; word < words_to_write; ++word) {
        assert(pages[page][index][word] == UINT32_MAX);
        pages[page][index][word] &= record[word];
    }
    return words_to_write == EDGE_SETTINGS_WORDS;
}

static bool erase(unsigned page)
{
    if (!erase_enabled) {
        return false;
    }
    memset(pages[page], 0xFF, sizeof pages[page]);
    return true;
}

int main(void)
{
    EdgeSettingsFlash flash = {
        .pages = {pages[0], pages[1]}, .capacity = PAGE_RECORDS, .program = program, .erase = erase
    };
    EdgeSettings store;
    EdgeThresholds first = {300, 3000}, second = {310, 3200}, third = {320, 3300};
    memset(pages, 0xFF, sizeof pages);
    assert(edge_settings_open(&store, flash) && !store.valid);
    assert(!edge_settings_save(&store, (EdgeThresholds){301, 3000}) && program_count == 0U);
    assert(edge_settings_save(&store, first) && store.sequence == 1U);
    assert(edge_settings_save(&store, first) && program_count == 1U);

    words_to_write = EDGE_SETTINGS_WORDS - 1U;
    assert(!edge_settings_save(&store, second));
    assert(edge_settings_open(&store, flash) && store.next[0] == 2U);
    assert(store.current.fan_on_tenths == 300U && store.current.close_level == 3000U);

    words_to_write = EDGE_SETTINGS_WORDS;
    assert(edge_settings_save(&store, second) && store.active == 0U);
    assert(edge_settings_save(&store, third) && store.active == 1U);
    pages[1][0][2] ^= 1U;
    assert(edge_settings_open(&store, flash) && store.active == 0U);
    assert(store.current.close_level == 3200U && store.next[1] == 0U);
    assert(edge_settings_save(&store, third) && store.active == 1U);
    erase_enabled = false;
    assert(!edge_settings_open(&store, flash) && store.active == 1U && store.current.close_level == 3300U);
    erase_enabled = true;
    assert(edge_settings_open(&store, flash) && store.next[0] == 0U);
    assert(store.current.fan_on_tenths == 320U);
    puts("Settings: invalid values, interrupted records, CRC, rollover and erase failure passed");
}
