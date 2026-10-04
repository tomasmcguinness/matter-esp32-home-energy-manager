#pragma once

#include <esp_err.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// How long the device stays open for commissioning. 900 s is the spec maximum for an
// enhanced window, which leaves time to find the device in another ecosystem's app.
#define COMMISSIONING_WINDOW_TIMEOUT_S 900

typedef struct
{
    char manual_code[24]; // 11 digits (or 21 with VID/PID), NUL-terminated
    char qr_code[64];     // "MT:..."
    uint16_t timeout_s;
} commissioning_window_result_t;

// C entry point for the web server. Same contract as
// home_energy_manager::controller::open_commissioning_window; on ESP_FAIL the CHIP error is
// written to `error` as text.
esp_err_t commissioning_window_open(uint64_t node_id, commissioning_window_result_t *out,
                                    char *error, size_t error_len);

#ifdef __cplusplus
}

#include <lib/core/CHIPError.h>

namespace home_energy_manager
{
    namespace controller
    {
        // Opens an enhanced commissioning window on an already-commissioned node, with a random
        // passcode and discriminator, and blocks until the device answers or the wait times out.
        //
        // Must be called WITHOUT the CHIP stack lock held -- it takes the lock itself and then
        // waits on a callback that runs on the Matter task.
        //
        // Returns ESP_OK and fills `out` on success. ESP_ERR_INVALID_STATE means another request
        // is still running, ESP_ERR_TIMEOUT means the device never answered, and ESP_FAIL means
        // the device or the session refused, with the CHIP error in `chip_err`.
        esp_err_t open_commissioning_window(uint64_t node_id, commissioning_window_result_t *out, CHIP_ERROR *chip_err);

    } // namespace controller
} // namespace home_energy_manager
#endif
