#include "matter_controller.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_timer.h>
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
#include "value_cache.h"
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

// Requested subscription reporting intervals. The server may negotiate a
// smaller MaxInterval; the agreed value is logged by the CHIP ReadClient
// ("Subscription established ... MaxInterval = Ns").
static constexpr uint16_t kSubMinIntervalSec = 1;
static constexpr uint16_t kSubMaxIntervalSec = 30;

// Live attribute updates are coalesced and broadcast to WebSocket clients on
// this timer rather than per Matter report, bounding both frame count and
// client renders regardless of how chatty the meters are. Logging is unaffected
// (node_power_logger polls ValueCache directly).
static constexpr uint64_t kWsBroadcastPeriodUs = 1000000; // 1s
static esp_timer_handle_t s_ws_broadcast_timer = nullptr;

static constexpr uint32_t kDescriptorCluster = 0x001D;
static constexpr uint32_t kDescriptorDeviceTypeList = 0x0000;
static constexpr uint32_t kDescriptorPartsList = 0x0003;
static constexpr uint32_t kBasicInfoCluster = 0x0028;
static constexpr uint32_t kBasicInfoVendorName = 0x0002;
static constexpr uint32_t kBasicInfoProductName = 0x0004;
// Bridged Device Basic Information lives on each bridged endpoint and carries
// the human-readable name the bridge advertises for that child device.
static constexpr uint32_t kBridgedDeviceBasicInfoCluster = 0x0039;
static constexpr uint32_t kBridgedDeviceNodeLabel = 0x0005;

static void cache_attribute(uint64_t node_id,
                            const chip::app::ConcreteDataAttributePath &path,
                            chip::TLV::TLVReader *data);

static void on_ws_broadcast_timer(void *arg);

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

// Unpairing an *offline* device blocks until CHIP exhausts mDNS resolution and
// CASE retries, which can take ~45s or more. The waiter timeout below must stay
// comfortably above that, otherwise we abandon a request whose callback is still
// pending. The semaphore is persistent (never deleted) and the callback ignores
// abandoned requests, so a late callback is harmless either way -- but a timeout
// shorter than CHIP's own gives a needless "device can't be deleted" failure.
static constexpr uint32_t kRemoveNodeTimeoutMs = 90000;

// node_id sentinel meaning "no removal in flight" (real ids start at 1).
static constexpr uint64_t kNoRemoveInFlight = 0;

struct remove_node_ctx
{
    SemaphoreHandle_t done;       // created once, never deleted
    uint64_t          node_id;    // request the waiter is currently blocked on
    CHIP_ERROR        result;
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
    // Only signal the waiter if it is still blocked on *this* request. A callback
    // that fires after the waiter timed out (offline device, slow CASE failure)
    // must not touch its state -- the persistent semaphore makes a stray give a
    // no-op that the next request drains.
    if ((uint64_t)remoteNodeId == s_remove_ctx.node_id)
    {
        s_remove_ctx.result = status;
        xSemaphoreGive(s_remove_ctx.done);
    }
}

esp_err_t matter_controller_remove_node(uint64_t node_id)
{
    if (s_remove_ctx.done == nullptr)
    {
        s_remove_ctx.done = xSemaphoreCreateBinary();
        if (s_remove_ctx.done == nullptr)
            return ESP_ERR_NO_MEM;
    }
    // Drain any stale give left by a previous timed-out request's late callback.
    while (xSemaphoreTake(s_remove_ctx.done, 0) == pdTRUE)
    {
    }

    s_remove_ctx.node_id = node_id;
    s_remove_ctx.result = CHIP_NO_ERROR;

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    esp_err_t err = esp_matter::controller::matter_controller_client::get_instance()
                        .unpair((chip::NodeId)node_id, remove_node_cb);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "unpair failed: 0x%x", err);
        s_remove_ctx.node_id = kNoRemoveInFlight; // no callback will come
        return err;
    }

    if (xSemaphoreTake(s_remove_ctx.done, pdMS_TO_TICKS(kRemoveNodeTimeoutMs)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Remove node timed out");
        // Abandon: a later callback must not signal a freed/reused waiter. We
        // never delete the semaphore, so the late give is harmless.
        s_remove_ctx.node_id = kNoRemoveInFlight;
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = (s_remove_ctx.result == CHIP_NO_ERROR) ? ESP_OK : ESP_FAIL;
    s_remove_ctx.node_id = kNoRemoveInFlight;
    return result;
}

void matter_controller_forget_node(uint64_t node_id)
{
    ESP_LOGW(TAG, "Forgetting node 0x%llx locally (no RemoveFabric sent to device)",
             (unsigned long long)node_id);
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    node_list_remove(node_id);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
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
    else if (path.mClusterId == kBridgedDeviceBasicInfoCluster &&
             path.mAttributeId == kBridgedDeviceNodeLabel)
    {
        // Per-endpoint: each bridged child carries its own NodeLabel.
        chip::CharSpan str;
        if (data->Get(str) == CHIP_NO_ERROR)
        {
            // Don't depend on the Descriptor read having created the endpoint first.
            device_manager_add_endpoint(node_id, path.mEndpointId);
            device_manager_set_endpoint_label(node_id, path.mEndpointId, str.data(), str.size());
        }
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
    attr_paths.Alloc(4);
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
    // BridgedDeviceBasicInformation NodeLabel on all endpoints (wildcard);
    // present only on bridged endpoints, which is exactly where we want it.
    attr_paths[3] = chip::app::AttributePathParams(chip::kInvalidEndpointId, kBridgedDeviceBasicInfoCluster, kBridgedDeviceNodeLabel);

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
    home_energy_manager::controller::pairing_command::get_instance().pairing_on_network(node_id, payload.setUpPINCode);
    //home_energy_manager::controller::pairing_command::get_instance().pairing_code(node_id, onboarding_payload);
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
    //
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

    if (!s_ws_broadcast_timer) {
        esp_timer_create_args_t ws_args = {
            .callback = on_ws_broadcast_timer,
            .arg      = nullptr,
            .name     = "ws_attr_batch",
        };
        err = esp_timer_create(&ws_args, &s_ws_broadcast_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ws broadcast timer create failed: 0x%x", err);
            return err;
        }
        err = esp_timer_start_periodic(s_ws_broadcast_timer, kWsBroadcastPeriodUs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ws broadcast timer start failed: 0x%x", err);
            return err;
        }
    }

    ESP_LOGI(TAG, "Matter controller started");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Subscriptions
// ---------------------------------------------------------------------------

// Drop a node id onto the subscription queue; the worker task picks it up and
// establishes (or re-establishes) the subscription. Safe to call from the CHIP
// event-loop thread (the subscription callbacks below) and from app threads.
static void subscribe_enqueue(uint64_t node_id);

void node_subscription_established_cb(uint64_t remote_node_id, uint32_t subscription_id)
{
    ESP_LOGI(TAG, "Successfully subscribed, node 0x%016llX, subscription id 0x%08X", remote_node_id, subscription_id);

    // Flag on the device manager that this node is subscribed, so the UI can reflect that.
    //
    device_manager_mark_subscribed(remote_node_id);
}

void node_subscription_terminated_cb(uint64_t remote_node_id, uint32_t subscription_id)
{
    ESP_LOGI(TAG, "Subscription terminated, node 0x%016llX, subscription id 0x%08X", remote_node_id, subscription_id);

    // The subscription is gone; reflect that and queue a re-subscribe attempt.
    //
    device_manager_mark_unsubscribed(remote_node_id);
    subscribe_enqueue(remote_node_id);
}

void node_subscribe_failed_cb(void *ctx, const chip::ScopedNodeId &node_id, chip::ChipError err)
{
    uint64_t remote_node_id = node_id.GetNodeId();
    ESP_LOGE(TAG, "Failed to subscribe to node 0x%016llX: %" CHIP_ERROR_FORMAT " (context: %p)",
             (unsigned long long)remote_node_id, err.Format(), ctx);

    // Flag the subscription failure so the UI can reflect that, then queue a retry.
    //
    device_manager_mark_unsubscribed(remote_node_id);
    subscribe_enqueue(remote_node_id);
}

static void on_attribute_data_cb(uint64_t node_id,
                                 const chip::app::ConcreteDataAttributePath &path,
                                 chip::TLV::TLVReader *data,
                                 const chip::app::StatusIB &status)
{
    using namespace chip::Protocols::InteractionModel;

    if (!data || status.mStatus != Status::Success)
        return;

    // Cache + broadcast the attributes the UI consumes: ElectricalPowerMeasurement
    // (voltage/current/power) and the Power Source battery state of charge.
    //
    if (path.mClusterId == ElectricalPowerMeasurement::Id || path.mClusterId == PowerSource::Id) {
        cache_attribute(node_id, path, data);
    }

    return;
}

// Persist the latest value of a subscribed attribute into the ValueCache.
// Deliberately cluster/attribute-generic: the cache and the UI key purely off
// cluster+attribute ids, so adding a new attribute path to the subscription is
// enough to surface it. WebSocket clients are updated by on_ws_broadcast_timer,
// which coalesces the whole cache into one `attribute_batch` frame; node_power_logger
// polls the cache on its own timer for per-minute averages. Neither hangs off the
// report cadence here.
static void cache_attribute(uint64_t node_id,
                            const chip::app::ConcreteDataAttributePath &path,
                            chip::TLV::TLVReader *data)
{
    ESP_LOGI(TAG, "Received attribute update for node 0x%016llX, endpoint 0x%04X, cluster 0x%04X, attribute 0x%04X", node_id, path.mEndpointId, path.mClusterId, path.mAttributeId);

    // Nullable attributes (e.g. BatPercentRemaining when SoC is unknown) report
    // as NULL; nothing to cache, so skip.
    if (data->GetType() == chip::TLV::kTLVType_Null) {
        return;
    }

    // TLVReader::Get(int64_t&) only accepts signed-integer element types; an
    // unsigned-encoded attribute (e.g. BatPercentRemaining, a uint8) would
    // return CHIP_ERROR_WRONG_TLV_TYPE and be silently dropped. Branch on the
    // TLV type and read unsigned values via the uint64_t overload. All attributes
    // we cache fit comfortably in int64_t, so the cast is safe.
    //
    int64_t raw_value = 0;
    switch (data->GetType()) {
    case chip::TLV::kTLVType_SignedInteger:
        if (data->Get(raw_value) != CHIP_NO_ERROR) {
            return;
        }
        break;
    case chip::TLV::kTLVType_UnsignedInteger: {
        uint64_t u = 0;
        if (data->Get(u) != CHIP_NO_ERROR) {
            return;
        }
        raw_value = static_cast<int64_t>(u);
        break;
    }
    default:
        // Not an integer we cache; ignore the update.
        return;
    }

    ESP_LOGI(TAG, "Caching value %lld for node 0x%016llX, endpoint 0x%04X, cluster 0x%04X, attribute 0x%04X", raw_value, node_id, path.mEndpointId, path.mClusterId, path.mAttributeId);
    
    ValueCache::instance().put(node_id, path.mEndpointId, path.mClusterId, path.mAttributeId, raw_value);
}

// Coalesce the whole ValueCache into a single `attribute_batch` WebSocket frame.
// Runs on s_ws_broadcast_timer so the UI sees a bounded update rate (one frame per
// period) instead of one frame per Matter report. Skips the broadcast when there is
// nothing valid to send.
static void on_ws_broadcast_timer(void *arg)
{
    std::vector<ValueCacheEntry> entries = ValueCache::instance().snapshot();

    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "type", "attribute_batch");
    cJSON *data = cJSON_AddArrayToObject(root, "data");

    size_t valid = 0;
    for (const ValueCacheEntry &e : entries) {
        if (!e.valid) continue;
        cJSON *item = cJSON_CreateObject();
        if (!item) continue;
        cJSON_AddNumberToObject(item, "nodeId", (double)e.node_id);
        cJSON_AddNumberToObject(item, "endpointId", e.endpoint_id);
        cJSON_AddNumberToObject(item, "clusterId", e.cluster_id);
        cJSON_AddNumberToObject(item, "attributeId", e.attribute_id);
        cJSON_AddNumberToObject(item, "value", (double)e.value);
        cJSON_AddItemToArray(data, item);
        valid++;
    }

    if (valid > 0) {
        char *json = cJSON_PrintUnformatted(root);
        if (json) {
            ws_server_broadcast(json, strlen(json));
            cJSON_free(json);
        }
    }

    cJSON_Delete(root);
}

void matter_controller_seed_value_cache(void)
{
    static constexpr size_t kMaxSensors = 32;
    uint64_t node_ids[kMaxSensors];
    uint16_t endpoint_ids[kMaxSensors];
    size_t count = device_manager_get_electrical_sensor_endpoints(node_ids, endpoint_ids, kMaxSensors);

    // Pre-populate one (invalid) entry per attribute we subscribe to, so the
    // cache shape mirrors the loaded device structure before any report arrives.
    //
    for (size_t i = 0; i < count; i++)
    {
        ValueCache::instance().seed(node_ids[i], endpoint_ids[i], ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::Voltage::Id);
        ValueCache::instance().seed(node_ids[i], endpoint_ids[i], ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::ActiveCurrent::Id);
        ValueCache::instance().seed(node_ids[i], endpoint_ids[i], ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::ActivePower::Id);
    }

    ESP_LOGI(TAG, "Seeded value cache for %u electrical sensor endpoint(s)", (unsigned)count);
}

// ---------------------------------------------------------------------------
// Subscription queue
//
// Subscriptions are driven through a FreeRTOS queue of node ids. matter_controller_subscribe
// enqueues the nodes to subscribe to; a single worker task dequeues each one and establishes
// the subscription. When a subscription fails or is terminated, the callbacks above drop the
// node back onto the queue so the worker re-establishes it. Per the chosen policy, retries are
// re-queued immediately and naturally paced by the CASE-session setup time.
// ---------------------------------------------------------------------------

static QueueHandle_t s_subscribe_queue = nullptr;
static TaskHandle_t  s_subscribe_task  = nullptr;
static constexpr size_t kSubscribeQueueLen = kMaxNodes;

// Establish (or re-establish) a subscription to one node. Runs on the CHIP event-loop thread
// via ScheduleWork. The endpoint is wildcarded, so one subscription per node covers every
// endpoint on that node exposing ElectricalPowerMeasurement.
static void establish_subscription(uint64_t node_id)
{
    ESP_LOGI(TAG, "Subscribing to ElectricalPowerMeasurement on node 0x%016llX (all endpoints), requested MaxInterval = %us", (unsigned long long)node_id, (unsigned)kSubMaxIntervalSec);

    auto *args = new uint64_t(node_id);

    chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t arg)
                                                  {
        auto *args = reinterpret_cast<uint64_t *>(arg);

        ScopedMemoryBufferWithSize<AttributePathParams> attr_paths;
        attr_paths.Alloc(4);

        // Endpoint left as wildcard (kInvalidEndpointId): subscribe to
        // these attributes on every endpoint of the node that exposes
        // the relevant cluster.
        //
        attr_paths[0] = AttributePathParams(ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::Voltage::Id);
        attr_paths[1] = AttributePathParams(ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::ActiveCurrent::Id);
        attr_paths[2] = AttributePathParams(ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::ActivePower::Id);
        // Battery state of charge. Only battery power sources expose this optional
        // attribute, so wildcarding the endpoint is harmless on other endpoints.
        attr_paths[3] = AttributePathParams(PowerSource::Id, PowerSource::Attributes::BatPercentRemaining::Id);

        ScopedMemoryBufferWithSize<EventPathParams> event_paths;
        event_paths.Alloc(0);

        // This might be an ICD device, so we would need to change the MinInterval to zero
        //
        auto *cmd = chip::Platform::New<esp_matter::controller::subscribe_command>(*args,
            std::move(attr_paths),
            std::move(event_paths),
            kSubMinIntervalSec, // MinInterval
            kSubMaxIntervalSec, // MaxInterval (requested ceiling)
            false, // <--- Keep Subscriptions
            on_attribute_data_cb,
            nullptr,
            node_subscription_established_cb,
            node_subscription_terminated_cb,
            node_subscribe_failed_cb,
            false);

        delete args;

        cmd->send_command();
    }, reinterpret_cast<intptr_t>(args));
}

static void subscribe_enqueue(uint64_t node_id)
{
    if (!s_subscribe_queue)
    {
        ESP_LOGW(TAG, "Subscribe queue not ready, dropping node 0x%016llX", (unsigned long long)node_id);
        return;
    }
    if (xQueueSend(s_subscribe_queue, &node_id, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "Subscribe queue full, dropping node 0x%016llX", (unsigned long long)node_id);
    }
}

static void subscribe_task(void *)
{
    uint64_t node_id = 0;
    for (;;)
    {
        if (xQueueReceive(s_subscribe_queue, &node_id, portMAX_DELAY) == pdTRUE)
        {
            establish_subscription(node_id);
        }
    }
}

esp_err_t matter_controller_subscribe(void)
{
    if (!s_subscribe_queue)
    {
        s_subscribe_queue = xQueueCreate(kSubscribeQueueLen, sizeof(uint64_t));
        if (!s_subscribe_queue)
        {
            ESP_LOGE(TAG, "Failed to create subscribe queue");
            return ESP_ERR_NO_MEM;
        }
        if (xTaskCreate(subscribe_task, "subscribe", 4096, nullptr, 5, &s_subscribe_task) != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create subscribe task");
            vQueueDelete(s_subscribe_queue);
            s_subscribe_queue = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }

    static constexpr size_t kMaxSensors = 32;
    uint64_t node_ids[kMaxSensors];
    uint16_t endpoint_ids[kMaxSensors];
    size_t count = device_manager_get_electrical_sensor_endpoints(node_ids, endpoint_ids, kMaxSensors);

    // A Matter subscription rides a single CASE session to one node, so we
    // subscribe per node (not per endpoint). The endpoint is wildcarded in the
    // attribute path, so one subscription per node covers every endpoint on
    // that node exposing ElectricalPowerMeasurement.
    //
    uint64_t unique_nodes[kMaxSensors];
    size_t node_count = 0;
    for (size_t i = 0; i < count; i++)
    {
        bool seen = false;
        for (size_t j = 0; j < node_count; j++)
        {
            if (unique_nodes[j] == node_ids[i]) { seen = true; break; }
        }
        if (!seen)
        {
            unique_nodes[node_count++] = node_ids[i];
        }
    }

    if (node_count > 0)
    {
        ESP_LOGI(TAG, "Queuing subscription to ElectricalPowerMeasurement on %u node(s)", (unsigned)node_count);
        for (size_t i = 0; i < node_count; i++)
        {
            subscribe_enqueue(unique_nodes[i]);
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
