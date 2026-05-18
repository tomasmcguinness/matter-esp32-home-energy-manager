#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t unix_minute; // unix timestamp of the minute's start
    int32_t  power_mw;    // average power over the minute, in milliwatts
} power_record_t;

esp_err_t  power_logger_init(void);
void       power_logger_sample(int32_t power_mw);
char      *power_logger_day_json(const char *date_str);    // caller must free
esp_err_t  power_logger_rollup_hourly(const char *date_str);
char      *power_logger_hourly_json(const char *date_str); // caller must free

#ifdef __cplusplus
}
#endif
