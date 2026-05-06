#include "matter_controller.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_console.h>
#include <esp_matter_controller_credentials_issuer.h>
#include <esp_matter_controller_pairing_command.h>
#include <esp_matter_controller_read_command.h>

#include "managers/device_manager.h"

#include <app/server/Dnssd.h>
#include <controller/CHIPDeviceController.h>
#include <controller/DevicePairingDelegate.h>
#include <controller/OperationalCredentialsDelegate.h>
#include <credentials/CHIPCert.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/core/CASEAuthTag.h>
#include <lib/core/TLV.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/Span.h>
#include <platform/CHIPDeviceLayer.h>
#include <protocols/secure_channel/PASESession.h>
#include <setup_payload/ManualSetupPayloadParser.h>
#include <setup_payload/QRCodeSetupPayloadParser.h>
#include <setup_payload/SetupPayload.h>

static const char *TAG = "matter_controller";

static constexpr char kNodeIdCounterKey[] = "HEM_NodeIdCnt";
static constexpr char kNodeListKey[] = "HEM_NodeList";
static constexpr size_t kMaxNodes = 32;
static constexpr uint64_t kFirstDeviceNodeId = 1;

static constexpr uint32_t kDescriptorCluster        = 0x001D;
static constexpr uint32_t kDescriptorDeviceTypeList  = 0x0000;
static constexpr uint32_t kDescriptorPartsList       = 0x0003;
static constexpr uint32_t kBasicInfoCluster          = 0x0028;
static constexpr uint32_t kBasicInfoVendorName       = 0x0002;
static constexpr uint32_t kBasicInfoProductName      = 0x0004;

uint64_t matter_controller_allocate_node_id(void)
{
    uint64_t node_id = kFirstDeviceNodeId;
    size_t read_size = sizeof(node_id);
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get(kNodeIdCounterKey, &node_id, sizeof(node_id), &read_size);
    uint64_t next = node_id + 1;
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put(kNodeIdCounterKey, &next, sizeof(next));
    return node_id;
}

// ---------------------------------------------------------------------------
// Node list (persisted in NVS as a blob of uint64_t values)
// ---------------------------------------------------------------------------

static void node_list_add(uint64_t node_id)
{
    uint64_t list[kMaxNodes] = {};
    size_t read_size = sizeof(list);
    size_t count = 0;

    if (chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get(
            kNodeListKey, list, sizeof(list), &read_size) == CHIP_NO_ERROR)
    {
        count = read_size / sizeof(uint64_t);
    }

    if (count >= kMaxNodes)
    {
        ESP_LOGW(TAG, "Node list full, cannot add node 0x%llx", (unsigned long long)node_id);
        return;
    }
    list[count++] = node_id;
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put(
        kNodeListKey, list, count * sizeof(uint64_t));
}

static void node_list_remove(uint64_t node_id)
{
    uint64_t list[kMaxNodes] = {};
    size_t read_size = sizeof(list);

    if (chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get(
            kNodeListKey, list, sizeof(list), &read_size) != CHIP_NO_ERROR)
    {
        return;
    }
    size_t count = read_size / sizeof(uint64_t);
    size_t w = 0;
    for (size_t i = 0; i < count; i++)
    {
        if (list[i] != node_id)
            list[w++] = list[i];
    }
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put(
        kNodeListKey, list, w * sizeof(uint64_t));
}

// ---------------------------------------------------------------------------
// Blocking unpair
// ---------------------------------------------------------------------------

struct remove_node_ctx
{
    SemaphoreHandle_t done;
    CHIP_ERROR result;
};

static remove_node_ctx s_remove_ctx;

static void remove_node_cb(chip::NodeId remoteNodeId, CHIP_ERROR status)
{
    ESP_LOGI(TAG, "RemoveFabric complete for node 0x%llx: %" CHIP_ERROR_FORMAT,
             (unsigned long long)remoteNodeId, status.Format());
    if (status == CHIP_NO_ERROR)
    {
        node_list_remove(remoteNodeId);
    }
    s_remove_ctx.result = status;
    xSemaphoreGive(s_remove_ctx.done);
}

esp_err_t matter_controller_remove_node(uint64_t node_id)
{
    s_remove_ctx.done = xSemaphoreCreateBinary();
    s_remove_ctx.result = CHIP_NO_ERROR;
    if (!s_remove_ctx.done)
        return ESP_ERR_NO_MEM;

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    esp_err_t err = esp_matter::controller::matter_controller_client::get_instance()
                        .unpair((chip::NodeId)node_id, remove_node_cb);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "unpair failed: 0x%x", err);
        vSemaphoreDelete(s_remove_ctx.done);
        return err;
    }

    if (xSemaphoreTake(s_remove_ctx.done, pdMS_TO_TICKS(30000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Remove node timed out");
        vSemaphoreDelete(s_remove_ctx.done);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = (s_remove_ctx.result == CHIP_NO_ERROR) ? ESP_OK : ESP_FAIL;
    vSemaphoreDelete(s_remove_ctx.done);
    return result;
}

// ---------------------------------------------------------------------------
// Post-commissioning interrogation
// ---------------------------------------------------------------------------

static void on_interrogation_attr(uint64_t node_id,
                                  const chip::app::ConcreteDataAttributePath &path,
                                  chip::TLV::TLVReader *data,
                                  const chip::app::StatusIB &status)
{
    using namespace chip::Protocols::InteractionModel;
    if (!data || status.mStatus != Status::Success)
        return;

    if (path.mClusterId == kDescriptorCluster)
    {
        if (path.mAttributeId == kDescriptorPartsList)
        {
            chip::TLV::TLVType outer;
            if (data->EnterContainer(outer) != CHIP_NO_ERROR) return;
            while (data->Next() == CHIP_NO_ERROR)
            {
                uint16_t ep_id = 0;
                if (data->Get(ep_id) == CHIP_NO_ERROR)
                    device_manager_add_endpoint(node_id, ep_id);
            }
            data->ExitContainer(outer);
        }
        else if (path.mAttributeId == kDescriptorDeviceTypeList)
        {
            device_manager_add_endpoint(node_id, path.mEndpointId);
            chip::TLV::TLVType list_type;
            if (data->EnterContainer(list_type) != CHIP_NO_ERROR) return;
            while (data->Next() == CHIP_NO_ERROR)
            {
                chip::TLV::TLVType struct_type;
                if (data->EnterContainer(struct_type) != CHIP_NO_ERROR) continue;
                uint32_t device_type = 0;
                while (data->Next() == CHIP_NO_ERROR)
                {
                    if (chip::TLV::TagNumFromTag(data->GetTag()) == 0)
                        data->Get(device_type);
                }
                data->ExitContainer(struct_type);
                if (device_type != 0)
                    device_manager_add_device_type(node_id, path.mEndpointId, device_type);
            }
            data->ExitContainer(list_type);
        }
    }
    else if (path.mClusterId == kBasicInfoCluster)
    {
        chip::CharSpan str;
        if (data->Get(str) != CHIP_NO_ERROR) return;
        if (path.mAttributeId == kBasicInfoVendorName)
            device_manager_set_vendor_name(node_id, str.data(), str.size());
        else if (path.mAttributeId == kBasicInfoProductName)
            device_manager_set_product_name(node_id, str.data(), str.size());
    }
}

static void on_interrogation_done(uint64_t node_id,
                                  const chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> &,
                                  const chip::Platform::ScopedMemoryBufferWithSize<chip::app::EventPathParams> &)
{
    ESP_LOGI(TAG, "Interrogation complete for node 0x%llx", (unsigned long long)node_id);
    device_manager_log_structure(node_id);
    device_manager_persist();
}

static void interrogate_node(uint64_t node_id)
{
    device_manager_add_device(node_id);

    chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> attr_paths;
    chip::Platform::ScopedMemoryBufferWithSize<chip::app::EventPathParams> event_paths;
    attr_paths.Alloc(3);
    if (!attr_paths.Get())
    {
        ESP_LOGE(TAG, "Failed to allocate attribute paths for interrogation");
        return;
    }

    // Descriptor cluster on all endpoints (wildcard), all attributes
    attr_paths[0] = chip::app::AttributePathParams(chip::kInvalidEndpointId, kDescriptorCluster, chip::kInvalidAttributeId);
    // BasicInformation VendorName and ProductName from endpoint 0
    attr_paths[1] = chip::app::AttributePathParams(0, kBasicInfoCluster, kBasicInfoVendorName);
    attr_paths[2] = chip::app::AttributePathParams(0, kBasicInfoCluster, kBasicInfoProductName);

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    auto *cmd = new esp_matter::controller::read_command(
        node_id, std::move(attr_paths), std::move(event_paths),
        on_interrogation_attr, on_interrogation_done, nullptr);
    if (cmd)
        cmd->send_command();
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
}

// ---------------------------------------------------------------------------
// Blocking on-network commissioning
// ---------------------------------------------------------------------------

struct commission_ctx
{
    SemaphoreHandle_t done;
    CHIP_ERROR result;
};

static commission_ctx s_commission_ctx;

static void on_commissioning_success_callback(ScopedNodeId peer_id)
{
    ESP_LOGI(TAG, "Commissioning succeeded for node 0x%llx", (unsigned long long)peer_id.GetNodeId());
    s_commission_ctx.result = CHIP_NO_ERROR;
    xSemaphoreGive(s_commission_ctx.done);
}

static void on_commissioning_failure_callback(ScopedNodeId peer_id,
                                              CHIP_ERROR error,
                                              chip::Controller::CommissioningStage stage,
                                              std::optional<chip::Credentials::AttestationVerificationResult> additional_err_info)
{
    ESP_LOGE(TAG, "Commissioning failed for node 0x%llx: %" CHIP_ERROR_FORMAT, (unsigned long long)peer_id.GetNodeId(), error.Format());
    s_commission_ctx.result = error;
    xSemaphoreGive(s_commission_ctx.done);
}

esp_err_t matter_controller_commission_on_network(const char *onboarding_payload)
{
    chip::SetupPayload payload;
    CHIP_ERROR parse_err;

    if (strncmp(onboarding_payload, "MT:", 3) == 0)
    {
        parse_err = chip::QRCodeSetupPayloadParser(onboarding_payload).populatePayload(payload);
    }
    else
    {
        parse_err = chip::ManualSetupPayloadParser(onboarding_payload).populatePayload(payload);
    }

    if (parse_err != CHIP_NO_ERROR)
    {
        ESP_LOGE(TAG, "Failed to parse onboarding payload: %" CHIP_ERROR_FORMAT, parse_err.Format());
        return ESP_ERR_INVALID_ARG;
    }

    s_commission_ctx.done = xSemaphoreCreateBinary();
    s_commission_ctx.result = CHIP_NO_ERROR;
    if (!s_commission_ctx.done)
    {
        return ESP_ERR_NO_MEM;
    }

    chip::NodeId node_id = matter_controller_allocate_node_id();

    esp_matter::controller::pairing_command_callbacks_t callbacks = {
        .commissioning_success_callback = on_commissioning_success_callback,
        .commissioning_failure_callback = on_commissioning_failure_callback};

    esp_matter::controller::pairing_command::get_instance().set_callbacks(callbacks);

    ESP_LOGI(TAG, "Attempting to commission node %llu", node_id);
    ESP_LOGI(TAG, "SetupCode %u", payload.setUpPINCode);
    ESP_LOGI(TAG, "Discriminator: %u", payload.discriminator.GetLongValue());

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    esp_matter::controller::pairing_code(node_id, onboarding_payload);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (xSemaphoreTake(s_commission_ctx.done, pdMS_TO_TICKS(60000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Commissioning timed out");
        vSemaphoreDelete(s_commission_ctx.done);
        return ESP_ERR_TIMEOUT;
    }

    CHIP_ERROR result = s_commission_ctx.result;
    vSemaphoreDelete(s_commission_ctx.done);

    if (result == CHIP_NO_ERROR)
    {
        node_list_add(node_id);
        interrogate_node(node_id);
        return ESP_OK;
    }

    return ESP_FAIL;
}

// ---------------------------------------------------------------------------
// Public re-interrogation
// ---------------------------------------------------------------------------

esp_err_t matter_controller_interrogate_node(uint64_t node_id)
{
    // Clear stale endpoints so removed endpoints don't persist after re-interview
    device_manager_clear_device_endpoints(node_id);
    interrogate_node(node_id);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

static void app_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t)
{
    if (event->Type == chip::DeviceLayer::DeviceEventType::kCommissioningComplete)
    {
        ESP_LOGI(TAG, "kCommissioningComplete");
    }
}

esp_err_t matter_controller_start(void)
{
#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::controller_register_commands();
    esp_matter::console::init();
#endif

    // esp_matter::controller::set_custom_credentials_issuer(&s_credentials_issuer);

    esp_err_t err = esp_matter::start(app_event_cb);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_matter::start failed: 0x%x", err);
        return err;
    }

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    err = esp_matter::controller::matter_controller_client::get_instance().init(112233, 1, 5580);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Controller init failed: 0x%x", err);
        chip::DeviceLayer::PlatformMgr().UnlockChipStack();
        return err;
    }

    err = esp_matter::controller::matter_controller_client::get_instance().setup_commissioner();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Commissioner setup failed: 0x%x", err);
        chip::DeviceLayer::PlatformMgr().UnlockChipStack();
        return err;
    }

    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    ESP_LOGI(TAG, "Matter commissioner started");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Factory reset
// ---------------------------------------------------------------------------

esp_err_t matter_factory_reset(void)
{
    device_manager_clear();

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    chip::Server::GetInstance().ScheduleFactoryReset();
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    return ESP_OK;
}
