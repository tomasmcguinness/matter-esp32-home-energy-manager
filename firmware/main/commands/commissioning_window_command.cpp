#include "commissioning_window_command.h"

#include <esp_log.h>
#include <esp_matter_controller_client.h>

#include <controller/CommissioningWindowOpener.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/core/NodeId.h>
#include <lib/support/Span.h>
#include <platform/PlatformManager.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

#define TAG "commissioning_window"

// Covers establishing CASE to a (possibly sleepy) Thread device, reading its VID and PID, and the
// OpenCommissioningWindow invoke itself.
#define OPEN_WINDOW_WAIT_MS (30 * 1000)

namespace home_energy_manager
{
    namespace controller
    {
        namespace
        {
            // One opener per request, deleted in its own callback. CommissioningWindowOpener has no
            // way to reset itself if a request is abandoned, so a shared instance would be stuck
            // rejecting every later request with CHIP_ERROR_INCORRECT_STATE after one timed out.
            // Same ownership scheme as CHIP's AutoCommissioningWindowOpener.
            struct open_request_t
            {
                explicit open_request_t(chip::Controller::DeviceController *controller)
                    : opener(controller), callback(on_complete, this)
                {
                }

                static void on_complete(void *context, chip::NodeId node_id, CHIP_ERROR status, chip::SetupPayload payload);

                chip::Controller::CommissioningWindowOpener opener;
                chip::Callback::Callback<chip::Controller::OnOpenCommissioningWindow> callback;
            };

            SemaphoreHandle_t s_lock = NULL; // serialises requests
            SemaphoreHandle_t s_done = NULL; // given by on_complete
            std::atomic<bool> s_waiting{false};
            CHIP_ERROR s_status = CHIP_NO_ERROR;
            commissioning_window_result_t s_result;

            bool ensure_initialised()
            {
                // Only ever called from the httpd task, so this doesn't need to be race-free.
                if (s_lock == NULL)
                {
                    s_lock = xSemaphoreCreateMutex();
                }

                if (s_done == NULL)
                {
                    s_done = xSemaphoreCreateBinary();
                }

                return s_lock != NULL && s_done != NULL;
            }

            // Called on the Matter task. A result for a request the caller already gave up on is
            // dropped, so it can't satisfy the next one.
            void open_request_t::on_complete(void *context, chip::NodeId node_id, CHIP_ERROR status, chip::SetupPayload payload)
            {
                auto *request = static_cast<open_request_t *>(context);

                if (s_waiting.exchange(false))
                {
                    if (status == CHIP_NO_ERROR)
                    {
                        chip::MutableCharSpan manual_code(s_result.manual_code, sizeof(s_result.manual_code) - 1);
                        CHIP_ERROR err = chip::ManualSetupPayloadGenerator(payload).payloadDecimalStringRepresentation(manual_code);

                        if (err == CHIP_NO_ERROR)
                        {
                            s_result.manual_code[manual_code.size()] = '\0';

                            chip::MutableCharSpan qr_code(s_result.qr_code, sizeof(s_result.qr_code) - 1);
                            err = chip::QRCodeBasicSetupPayloadGenerator(payload).payloadBase38Representation(qr_code);

                            if (err == CHIP_NO_ERROR)
                            {
                                s_result.qr_code[qr_code.size()] = '\0';
                            }
                        }

                        status = err;
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Failed to open commissioning window on node 0x%016llX: %s", node_id, chip::ErrorStr(status));
                    }

                    s_status = status;
                    xSemaphoreGive(s_done);
                }
                else
                {
                    ESP_LOGW(TAG, "Late commissioning window result for node 0x%016llX ignored", node_id);
                }

                // The opener doesn't touch itself after invoking the callback.
                delete request;
            }
        } // namespace

        esp_err_t open_commissioning_window(uint64_t node_id, commissioning_window_result_t *out, CHIP_ERROR *chip_err)
        {
            *chip_err = CHIP_NO_ERROR;

            if (!chip::IsOperationalNodeId(node_id))
            {
                return ESP_ERR_INVALID_ARG;
            }

            if (!ensure_initialised())
            {
                return ESP_ERR_NO_MEM;
            }

            if (xSemaphoreTake(s_lock, 0) != pdTRUE)
            {
                return ESP_ERR_INVALID_STATE;
            }

            uint16_t discriminator = 0;

            if (chip::Crypto::DRBG_get_bytes(reinterpret_cast<uint8_t *>(&discriminator), sizeof(discriminator)) != CHIP_NO_ERROR)
            {
                xSemaphoreGive(s_lock);
                return ESP_FAIL;
            }

            discriminator &= 0x0FFF; // discriminators are 12 bits

            auto *commissioner = esp_matter::controller::matter_controller_client::get_instance().get_commissioner();
            auto *request = new (std::nothrow) open_request_t(commissioner);

            if (request == nullptr)
            {
                xSemaphoreGive(s_lock);
                return ESP_ERR_NO_MEM;
            }

            memset(&s_result, 0, sizeof(s_result));
            s_result.timeout_s = COMMISSIONING_WINDOW_TIMEOUT_S;

            // Arm the wait before starting so a callback can't land before we're listening, and
            // drain anything a previously abandoned request left behind.
            xSemaphoreTake(s_done, 0);
            s_waiting = true;

            ESP_LOGI(TAG, "Opening commissioning window on node 0x%016llX for %u s", node_id, COMMISSIONING_WINDOW_TIMEOUT_S);

            chip::SetupPayload payload;

            chip::DeviceLayer::PlatformMgr().LockChipStack();
            CHIP_ERROR err = request->opener.OpenCommissioningWindow(node_id,
                                                                     chip::System::Clock::Seconds16(COMMISSIONING_WINDOW_TIMEOUT_S),
                                                                     chip::Crypto::kSpake2p_Min_PBKDF_Iterations,
                                                                     discriminator,
                                                                     chip::NullOptional, // random passcode
                                                                     chip::NullOptional, // random salt
                                                                     &request->callback,
                                                                     payload,
                                                                     true); // read VID/PID so the QR code carries them
            chip::DeviceLayer::PlatformMgr().UnlockChipStack();

            if (err != CHIP_NO_ERROR)
            {
                // A synchronous failure doesn't invoke the callback, so the request is still ours.
                s_waiting = false;
                delete request;
                xSemaphoreGive(s_lock);

                ESP_LOGE(TAG, "Failed to start opening commissioning window: %s", chip::ErrorStr(err));
                *chip_err = err;
                return ESP_FAIL;
            }

            bool signalled = xSemaphoreTake(s_done, pdMS_TO_TICKS(OPEN_WINDOW_WAIT_MS)) == pdTRUE;

            if (!signalled && !s_waiting.exchange(false))
            {
                // The callback claimed the result just as we timed out and is filling it in now.
                // Take it, rather than letting it land on the next request.
                xSemaphoreTake(s_done, portMAX_DELAY);
                signalled = true;
            }

            if (!signalled)
            {
                // Leave the request alive: its callback will still arrive, find nobody waiting, and
                // delete it.
                xSemaphoreGive(s_lock);

                ESP_LOGE(TAG, "Timed out opening commissioning window on node 0x%016llX", node_id);
                return ESP_ERR_TIMEOUT;
            }

            CHIP_ERROR status = s_status;
            *out = s_result;

            xSemaphoreGive(s_lock);

            if (status != CHIP_NO_ERROR)
            {
                *chip_err = status;
                return ESP_FAIL;
            }

            ESP_LOGI(TAG, "Commissioning window open on node 0x%016llX, manual code %s", node_id, out->manual_code);

            return ESP_OK;
        }

    } // namespace controller
} // namespace home_energy_manager

extern "C" esp_err_t commissioning_window_open(uint64_t node_id, commissioning_window_result_t *out,
                                               char *error, size_t error_len)
{
    CHIP_ERROR chip_err = CHIP_NO_ERROR;
    esp_err_t err = home_energy_manager::controller::open_commissioning_window(node_id, out, &chip_err);

    if (error != nullptr && error_len > 0)
    {
        error[0] = '\0';
        if (chip_err != CHIP_NO_ERROR)
        {
            snprintf(error, error_len, "%s", chip::ErrorStr(chip_err));
        }
    }

    return err;
}
