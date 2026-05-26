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
#include <esp_matter_controller_subscribe_command.h>

#include "managers/device_manager.h"
#include "managers/node_manager.h"
#include "ws_server.h"
#include "power_logger.h"
#include "cJSON.h"

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

#include "commands/pairing_command.h"

using namespace chip;
using namespace chip::app::Clusters;

static const char *TAG = "matter_controller";

static constexpr char kNodeIdCounterKey[] = "HEM_NodeIdCnt";
static constexpr char kNodeListKey[] = "HEM_NodeList";
static constexpr size_t kMaxNodes = 32;
static constexpr uint64_t kFirstDeviceNodeId = 1;

static constexpr uint32_t kDescriptorCluster = 0x001D;
static constexpr uint32_t kDescriptorDeviceTypeList = 0x0000;
static constexpr uint32_t kDescriptorPartsList = 0x0003;
static constexpr uint32_t kBasicInfoCluster = 0x0028;
static constexpr uint32_t kBasicInfoVendorName = 0x0002;
static constexpr uint32_t kBasicInfoProductName = 0x0004;

void processElectralPowerMeasurementUpdate(uint64_t node_id,
                                           const chip::app::ConcreteDataAttributePath &path,
                                           chip::TLV::TLVReader *data);

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
            if (data->EnterContainer(outer) != CHIP_NO_ERROR)
                return;
            while (data->Next() == CHIP_NO_ERROR)
            {
                uint16_t ep_id = 0;
                if (data->Get(ep_id) == CHIP_NO_ERROR)
                {
                    device_manager_add_endpoint(node_id, ep_id);
                    device_manager_add_endpoint_part(node_id, path.mEndpointId, ep_id);
                }
            }
            data->ExitContainer(outer);
        }
        else if (path.mAttributeId == kDescriptorDeviceTypeList)
        {
            device_manager_add_endpoint(node_id, path.mEndpointId);
            chip::TLV::TLVType list_type;
            if (data->EnterContainer(list_type) != CHIP_NO_ERROR)
                return;
            while (data->Next() == CHIP_NO_ERROR)
            {
                chip::TLV::TLVType struct_type;
                if (data->EnterContainer(struct_type) != CHIP_NO_ERROR)
                    continue;
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
        if (data->Get(str) != CHIP_NO_ERROR)
            return;
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
    device_manager_resolve_parents(node_id);
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
        node_id,
        std::move(attr_paths),
        std::move(event_paths),
        on_interrogation_attr,
        on_interrogation_done,
        nullptr);
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

esp_err_t matter_controller_commission_on_network(const char *onboarding_payload, uint64_t *node_id_out)
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

    home_energy_manager::controller::pairing_command_callbacks_t callbacks = {
        .commissioning_success_callback = on_commissioning_success_callback,
        .commissioning_failure_callback = on_commissioning_failure_callback};

    home_energy_manager::controller::pairing_command::get_instance().set_callbacks(callbacks);

    ESP_LOGI(TAG, "Attempting to commission node %llu", node_id);
    ESP_LOGI(TAG, "SetupCode %u", payload.setUpPINCode);
    if (payload.discriminator.IsShortDiscriminator())
    {
        ESP_LOGI(TAG, "Discriminator: %u (short)", payload.discriminator.GetShortValue());
    }
    else
    {
        ESP_LOGI(TAG, "Discriminator: %u (long)", payload.discriminator.GetLongValue());
    }

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    //home_energy_manager::controller::pairing_command::get_instance().pairing_on_network(node_id, payload.setUpPINCode);
    home_energy_manager::controller::pairing_command::get_instance().pairing_code(node_id, onboarding_payload);
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
        if (node_id_out)
            *node_id_out = (uint64_t)node_id;
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

    ESP_LOGI(TAG, "Matter controller started");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Subscriptions
// ---------------------------------------------------------------------------

void node_subscription_established_cb(uint64_t remote_node_id, uint32_t subscription_id)
{
    ESP_LOGI(TAG, "Successfully subscribed, node 0x%016llX, subscription id 0x%08X", remote_node_id, subscription_id);
}

void node_subscription_terminated_cb(uint64_t remote_node_id, uint32_t subscription_id)
{
    ESP_LOGI(TAG, "Subscription terminated, node 0x%016llX, subscription id 0x%08X", remote_node_id, subscription_id);
}

void node_subscribe_failed_cb(void *ctx, const chip::ScopedNodeId &node_id, int err)
{
    ESP_LOGE(TAG, "Failed to subscribe (context: %p)", ctx);
}

static void on_attribute_data_cb(uint64_t node_id,
                                 const chip::app::ConcreteDataAttributePath &path,
                                 chip::TLV::TLVReader *data,
                                 const chip::app::StatusIB &status)
{
    using namespace chip::Protocols::InteractionModel;

    if (!data || status.mStatus != Status::Success)
        return;

    // Process ElectrialPowerMeasurement updates.
    if (path.mClusterId == ElectricalPowerMeasurement::Id) {
        processElectralPowerMeasurementUpdate(node_id, path, data);
    }
     
    return;
}

// Grid sensor identity, loaded once from the node manager when subscribing.
static uint64_t s_grid_node_id     = 0;
static uint16_t s_grid_endpoint_id = 0;

static void load_grid_sensor_identity(void)
{
    char *json = node_manager_get_all_json();
    if (!json) return;

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return;

    cJSON *nodes = cJSON_GetObjectItemCaseSensitive(root, "nodes");
    cJSON *n;
    cJSON_ArrayForEach(n, nodes)
    {
        cJSON *id_j = cJSON_GetObjectItemCaseSensitive(n, "id");
        if (!cJSON_IsString(id_j) || strcmp(id_j->valuestring, "grid_meter") != 0)
            continue;
        cJSON *settings = cJSON_GetObjectItemCaseSensitive(n, "settings");
        cJSON *nid_j    = cJSON_GetObjectItemCaseSensitive(settings, "nodeId");
        cJSON *eid_j    = cJSON_GetObjectItemCaseSensitive(settings, "endpointId");
        if (cJSON_IsNumber(nid_j) && cJSON_IsNumber(eid_j))
        {
            s_grid_node_id     = (uint64_t)nid_j->valuedouble;
            s_grid_endpoint_id = (uint16_t)eid_j->valuedouble;
            ESP_LOGI(TAG, "Grid sensor: node 0x%llx EP %u", (unsigned long long)s_grid_node_id, s_grid_endpoint_id);
        }
        break;
    }
    cJSON_Delete(root);
}

void processElectralPowerMeasurementUpdate(uint64_t node_id,
                                           const chip::app::ConcreteDataAttributePath &path,
                                           chip::TLV::TLVReader *data)
{
    ESP_LOGI(TAG, "Received attribute update for node 0x%016llX, cluster 0x%04X, attribute 0x%04X", node_id, path.mClusterId, path.mAttributeId);

    // Ignore anything that isn't from the ElectrialPowerMeasurement cluster.
    //
    if (path.mClusterId != ElectricalPowerMeasurement::Id) {
        return;
    }

    // if (path.mAttributeId != ElectricalPowerMeasurement::Attributes::ActivePower::Id) {
    //     return;
    // }

    if (data->GetType() == chip::TLV::kTLVType_Null) {
        return;
    }

    // We need to decided how to "AI" this data at this point.
    // At this point, all we know is that an Electrial Power Measurement cluster has sent some data.
    //
    int64_t raw_value = 0;

    if (data->Get(raw_value) != CHIP_NO_ERROR) {
        return;
    }

    if (node_id == s_grid_node_id &&
        path.mEndpointId == s_grid_endpoint_id &&
        path.mAttributeId == ElectricalPowerMeasurement::Attributes::ActivePower::Id)
    {
        ESP_LOGI(TAG, "Recording grid power value");
        power_logger_sample((int32_t)raw_value);
    }

    ESP_LOGI(TAG, "Sending 'attribute_update' for node 0x%016llX, cluster 0x%04X, attribute 0x%04X", node_id, path.mClusterId, path.mAttributeId);

    char json[128];

    snprintf(json, sizeof(json), "{\"type\":\"attribute_update\",\"data\":{\"nodeId\":%llu,\"endpointId\":%u, \"clusterId\":%u, \"attributeId\":%u, \"value\":%lld}}", (unsigned long long)node_id, (unsigned)path.mEndpointId, path.mClusterId, path.mAttributeId, raw_value);

    ws_server_broadcast(json, strlen(json));
}

esp_err_t matter_controller_subscribe(void)
{
    load_grid_sensor_identity();

    static constexpr size_t kMaxSensors = 32;
    uint64_t node_ids[kMaxSensors];
    uint16_t endpoint_ids[kMaxSensors];
    size_t count = device_manager_get_electrical_sensor_endpoints(node_ids, endpoint_ids, kMaxSensors);

    if (count > 0)
    {
        ESP_LOGI(TAG, "Subscribing to Active Power on %u electrical sensor endpoint(s)", (unsigned)count);

        for (size_t i = 0; i < count; i++)
        {
            uint64_t node_id = node_ids[i];
            uint16_t endpoint_id = endpoint_ids[i];

            auto *args = new std::tuple<uint64_t, uint16_t>(node_id, endpoint_id);

            chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t arg)
                                                          {
                auto *args = reinterpret_cast<std::tuple<uint64_t, uint16_t> *>(arg);

                ScopedMemoryBufferWithSize<AttributePathParams> attr_paths;
                attr_paths.Alloc(3);

                attr_paths[0] = AttributePathParams(ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::Voltage::Id);
                attr_paths[1] = AttributePathParams(ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::ActiveCurrent::Id);
                attr_paths[2] = AttributePathParams(ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::ActivePower::Id);
                
                ScopedMemoryBufferWithSize<EventPathParams> event_paths;
                event_paths.Alloc(0);

                // This might be an ICD device??
                auto *cmd = chip::Platform::New<esp_matter::controller::subscribe_command>(std::get<0>(*args),
                    std::move(attr_paths), 
                    std::move(event_paths), 
                    1, // MinInterval 1 second 
                    30, // MaxInterval 30 seconds
                    false, // <--- Keep Subscriptions
                    on_attribute_data_cb,
                    nullptr,
                    node_subscription_established_cb,
                    node_subscription_terminated_cb,
                    nullptr,
                    false);
        
                delete args;
                
                cmd->send_command(); 
            }, reinterpret_cast<intptr_t>(args));
        }
    }
    else
    {
        ESP_LOGI(TAG, "No electrical sensor endpoints found to subscribe to");
    }

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Factory reset
// ---------------------------------------------------------------------------

esp_err_t matter_factory_reset(void)
{
    device_manager_clear();
    node_manager_clear();

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    chip::Server::GetInstance().ScheduleFactoryReset();
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    return ESP_OK;
}
