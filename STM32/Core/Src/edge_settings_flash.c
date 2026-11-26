#include "edge_settings.h"
#include "stm32f4xx_hal.h"

extern const uint8_t __edge_settings_a[], __edge_settings_b[], __edge_settings_size[];

static const uint8_t *const sector_start[] = {__edge_settings_a, __edge_settings_b};

static bool program_record(unsigned page, size_t index, const EdgeSettingsRecord record)
{
    uint32_t address = (uint32_t)(uintptr_t)sector_start[page] + index * sizeof(EdgeSettingsRecord);
    if (HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
    HAL_StatusTypeDef status = HAL_OK;
    /* A record is visible to the boot scan only after its last word is written. */
    for (unsigned word = 0; word < EDGE_SETTINGS_WORDS && status == HAL_OK; ++word) {
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address + word * sizeof(uint32_t),
                                   record[word]);
    }
    HAL_FLASH_Lock();
    return status == HAL_OK;
}

static bool erase_sector(unsigned page)
{
    if (HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }
    FLASH_EraseInitTypeDef operation = {
        .TypeErase = FLASH_TYPEERASE_SECTORS, .VoltageRange = FLASH_VOLTAGE_RANGE_3,
        .Sector = page == 0U ? FLASH_SECTOR_6 : FLASH_SECTOR_7, .NbSectors = 1
    };
    uint32_t failed_sector;
    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&operation, &failed_sector);
    HAL_FLASH_Lock();
    return status == HAL_OK;
}

EdgeSettingsFlash edge_settings_flash(void)
{
    return (EdgeSettingsFlash){
        .pages = {(const volatile EdgeSettingsRecord *)sector_start[0],
                  (const volatile EdgeSettingsRecord *)sector_start[1]},
        .capacity = (uintptr_t)__edge_settings_size / sizeof(EdgeSettingsRecord),
        .program = program_record, .erase = erase_sector
    };
}
