#include "subscription_manager.h"
#include "device_manager.h"
#include "matter_controller.h"
#include "tariff.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_subscribe_command.h>

#include <app/icd/client/DefaultICDClientStorage.h>
#include <platform/CHIPDeviceLayer.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;

static const char *TAG = "subscription_manager";

#define SUBSCRIPTION_QUEUE_DEPTH   16
#define SUBSCRIPTION_TASK_STACK    4096
#define SUBSCRIPTION_TASK_PRIORITY 5

// Minimum interval between subscription attempts. A node that keeps failing re-queues itself
// immediately, so without this the worker would spin on an unreachable device.
#define SUBSCRIBE_PACING_MS 1000

// How often to look for nodes that ought to have a subscription but don't. The sweep runs on this
// schedule whether or not the queue is busy: a device that keeps failing re-queues itself after
// every attempt, which would otherwise keep the queue from ever going idle long enough to sweep.
#define SWEEP_INTERVAL_MS 60000

// Subscription attempts allowed in flight at once. Each holds a CASE session-setup slot until it
// resolves; the controller has CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_CASE_CLIENTS (16) of those and
// CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_DEVICES (8) operational session setups, shared with reads, ICD
// check-ins, interrogation and commissioning. Four leaves most of both for everything else.
#define MAX_ATTEMPTS_IN_FLIGHT 4

// An attempt that has not reported back after this long gives up its slot anyway. Normally every
// attempt ends in one of the callbacks that call subscription_attempt_finished(), but one path does
// not: if the session connects and esp-matter then fails to send the subscribe request, it deletes
// the command without calling anything.
#define ATTEMPT_TIMEOUT_MS 90000

// How often the worker rechecks for a free slot while all of them are in use.
#define SLOT_POLL_MS 250

// Requested subscription reporting intervals. The server may negotiate a smaller MaxInterval.
#define SUBSCRIBE_MIN_INTERVAL 1
#define SUBSCRIBE_MAX_INTERVAL 30

// Upper bound on nodes we track subscription state for.
#define MAX_NODES 32

// ---------------------------------------------------------------------------
// Per-node subscription state
//
// The heating monitor keeps these flags on its node manager; the HEM has no equivalent node object,
// so they live here. Written from the CHIP task (callbacks) and the worker task, so only touched
// under s_state_mutex.
// ---------------------------------------------------------------------------

struct node_state_t {
    uint64_t node_id;          // 0 when unused
    bool     has_subscription;
    bool     is_subscription_pending;
    uint32_t subscription_id;
    bool     is_icd;           // learned on the CHIP task; see refresh_icd_flag()
};

static SemaphoreHandle_t s_state_mutex = NULL;
static node_state_t      s_nodes[MAX_NODES];

// Caller holds s_state_mutex.
static node_state_t *find_node_locked(uint64_t node_id, bool create)
{
    for (auto &n : s_nodes)
        if (n.node_id == node_id) return &n;
    if (!create) return nullptr;
    for (auto &n : s_nodes)
    {
        if (n.node_id == 0)
        {
            n = node_state_t{};
            n.node_id = node_id;
            return &n;
        }
    }
    return nullptr;
}

static void mark_node_has_subscription(uint64_t node_id, uint32_t subscription_id)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    node_state_t *node = find_node_locked(node_id, true);
    if (node)
    {
        node->has_subscription        = true;
        node->is_subscription_pending = false;
        node->subscription_id         = subscription_id;
    }
    xSemaphoreGive(s_state_mutex);

    // Mirror onto the device manager so the UI can show it.
    device_manager_mark_subscribed(node_id);
}

// Clears the node's subscription, but only if subscription_id is the one we hold (or 0, meaning
// "whatever it is"): a late termination of a replaced subscription must not clear the new one.
// Returns true when the state was cleared and a new subscription should be established.
static bool mark_node_has_no_subscription(uint64_t node_id, uint32_t subscription_id)
{
    bool create_new_subscription = false;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    node_state_t *node = find_node_locked(node_id, false);
    if (node && (node->subscription_id == subscription_id || subscription_id == 0))
    {
        node->has_subscription        = false;
        node->is_subscription_pending = false;
        node->subscription_id         = 0;
        create_new_subscription       = true;
    }
    xSemaphoreGive(s_state_mutex);

    if (create_new_subscription) device_manager_mark_unsubscribed(node_id);
    return create_new_subscription;
}

static bool node_is_icd(uint64_t node_id)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    node_state_t *node = find_node_locked(node_id, false);
    bool icd = node && node->is_icd;
    xSemaphoreGive(s_state_mutex);
    return icd;
}

// True if the node was commissioned as an ICD and registered us as its Check-In client. Must run on
// the CHIP task: the ICD client storage is not locked.
static bool is_registered_icd(uint64_t node_id)
{
    auto &storage = esp_matter::controller::matter_controller_client::get_instance().get_icd_client_storage();
    auto *iter = storage.IterateICDClientInfo();
    if (!iter) return false;
    DefaultICDClientStorage::ICDClientInfoIteratorWrapper wrapper(iter);
    ICDClientInfo info;
    while (iter->Next(info))
    {
        if (info.peer_node.GetNodeId() == node_id) return true;
    }
    return false;
}

// Cache whether the node is an ICD, so the worker and sweep (not on the CHIP task) can use it.
// Runs on the CHIP task.
static void refresh_icd_flag(uint64_t node_id)
{
    bool icd = is_registered_icd(node_id);
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    node_state_t *node = find_node_locked(node_id, true);
    if (node) node->is_icd = icd;
    xSemaphoreGive(s_state_mutex);
}

// ---------------------------------------------------------------------------
// Which nodes the HEM needs, and what from them
// ---------------------------------------------------------------------------

// Nodes exposing an Electrical Sensor endpoint, plus the tariff source. Returns the count.
static size_t wanted_nodes(uint64_t *out, size_t max)
{
    uint64_t node_ids[MAX_NODES];
    uint16_t endpoint_ids[MAX_NODES];
    size_t count = device_manager_get_electrical_sensor_endpoints(node_ids, endpoint_ids, MAX_NODES);

    size_t n = 0;
    auto add = [&](uint64_t id) {
        for (size_t j = 0; j < n; j++) if (out[j] == id) return;
        if (n < max) out[n++] = id;
    };
    for (size_t i = 0; i < count; i++) add(node_ids[i]);

    uint64_t tariff_node = 0;
    if (tariff_get_source(&tariff_node, nullptr)) add(tariff_node);
    return n;
}

static bool node_is_wanted(uint64_t node_id)
{
    uint64_t nodes[MAX_NODES];
    size_t n = wanted_nodes(nodes, MAX_NODES);
    for (size_t i = 0; i < n; i++) if (nodes[i] == node_id) return true;
    return false;
}

// ---------------------------------------------------------------------------
// In-flight attempt slots
// ---------------------------------------------------------------------------

// Attempts in flight, keyed by node. The worker task claims slots and the CHIP task frees them from
// the subscription callbacks, so the table is only touched under s_slots_mutex.
struct attempt_slot_t {
    uint64_t   node_id; // 0 when the slot is free
    TickType_t started;
};

static SemaphoreHandle_t s_slots_mutex = NULL;
static attempt_slot_t    s_slots[MAX_ATTEMPTS_IN_FLIGHT];
static QueueHandle_t     s_queue = NULL;

// Frees any slot held longer than ATTEMPT_TIMEOUT_MS. Caller holds s_slots_mutex.
static void reclaim_expired_slots_locked(void)
{
    TickType_t now = xTaskGetTickCount();

    for (size_t i = 0; i < MAX_ATTEMPTS_IN_FLIGHT; i++)
    {
        if (s_slots[i].node_id != 0 && (now - s_slots[i].started) > pdMS_TO_TICKS(ATTEMPT_TIMEOUT_MS))
        {
            ESP_LOGW(TAG, "Subscription attempt for node 0x%016llX never reported back; releasing its slot", s_slots[i].node_id);

            // Nothing will retry a node still marked pending, so clear that too and let the sweep
            // pick it up again.
            mark_node_has_no_subscription(s_slots[i].node_id, 0);

            s_slots[i].node_id = 0;
        }
    }
}

// Blocks until a slot is free, then claims it for node_id.
static void claim_slot(uint64_t node_id)
{
    bool logged = false;

    while (true)
    {
        xSemaphoreTake(s_slots_mutex, portMAX_DELAY);

        reclaim_expired_slots_locked();

        for (size_t i = 0; i < MAX_ATTEMPTS_IN_FLIGHT; i++)
        {
            if (s_slots[i].node_id == 0)
            {
                s_slots[i].node_id = node_id;
                s_slots[i].started = xTaskGetTickCount();
                xSemaphoreGive(s_slots_mutex);
                return;
            }
        }

        xSemaphoreGive(s_slots_mutex);

        if (!logged)
        {
            ESP_LOGI(TAG, "%d subscription attempts in flight; node 0x%016llX waits for one to finish", MAX_ATTEMPTS_IN_FLIGHT, node_id);
            logged = true;
        }

        vTaskDelay(pdMS_TO_TICKS(SLOT_POLL_MS));
    }
}

// Report that a subscription attempt for a node has resolved -- established, failed to connect, or
// ended before it was established -- so its in-flight slot can go to the next node. Safe to call more
// than once, or for a node with no attempt in flight: only the first call for an attempt frees a slot.
static void subscription_attempt_finished(uint64_t node_id)
{
    if (!s_slots_mutex || node_id == 0)
    {
        return;
    }

    xSemaphoreTake(s_slots_mutex, portMAX_DELAY);

    for (size_t i = 0; i < MAX_ATTEMPTS_IN_FLIGHT; i++)
    {
        if (s_slots[i].node_id == node_id)
        {
            s_slots[i].node_id = 0;
            break;
        }
    }

    xSemaphoreGive(s_slots_mutex);
}

// ---------------------------------------------------------------------------
// Subscription callbacks (CHIP task)
// ---------------------------------------------------------------------------

static void node_subscription_established_cb(uint64_t remote_node_id, uint32_t subscription_id)
{
    ESP_LOGI(TAG, "Successfully subscribed, node 0x%016llX, subscription id 0x%08" PRIX32, remote_node_id, subscription_id);

    mark_node_has_subscription(remote_node_id, subscription_id);

    // The attempt has resolved, so its in-flight slot can go to the next queued node.
    subscription_attempt_finished(remote_node_id);
}

static void node_subscription_terminated_cb(uint64_t remote_node_id, uint32_t subscription_id)
{
    ESP_LOGI(TAG, "Subscription terminated, node 0x%016llX, subscription id 0x%08" PRIX32, remote_node_id, subscription_id);

    bool create_new_subscription = mark_node_has_no_subscription(remote_node_id, subscription_id);

    // Also how an attempt ends that connected but never got as far as establishing -- esp-matter
    // reports that as a termination with subscription id 0. A no-op after an established one.
    subscription_attempt_finished(remote_node_id);

    // The node might still have an active subscription, so only establish another if necessary.
    if (create_new_subscription)
    {
        enqueue_subscription(remote_node_id);
    }
}

static void node_subscribe_failed_cb(void *ctx, const ScopedNodeId &peer_id, CHIP_ERROR error)
{
    // ctx is the subscribe_command, which esp-matter deletes as soon as we return. The node we
    // failed to reach is identified by peer_id.
    uint64_t node_id = peer_id.GetNodeId();

    ESP_LOGE(TAG, "Failed to subscribe to node 0x%016llX: %s", node_id, error.AsString());

    // Clear both has_subscription and is_subscription_pending, otherwise the node looks like it
    // still has an attempt in flight and nothing will ever retry it.
    mark_node_has_no_subscription(node_id, 0);

    subscription_attempt_finished(node_id);

    // A sleepy device is not worth chasing - it will get a subscription when it next checks in.
    refresh_icd_flag(node_id);
    if (!node_is_icd(node_id))
    {
        enqueue_subscription(node_id);
    }
}

static void on_icd_checkin_callback(const ICDClientInfo &clientInfo)
{
    // A check-in from an ICD device means it has no subscriptions.
    uint64_t node_id = clientInfo.peer_node.GetNodeId();

    ESP_LOGI(TAG, "ICD Check-In from node 0x%016llX", node_id);

    if (!node_is_wanted(node_id))
    {
        return;
    }

    refresh_icd_flag(node_id);

    // Treat it as unsubscribed whatever we believed, since the device says it has no subscription.
    // The node may be unknown to the state table yet, in which case there is nothing to clear.
    mark_node_has_no_subscription(node_id, 0);

    ESP_LOGI(TAG, "Queueing subscription after ICD Check-In");
    enqueue_subscription(node_id);
}

// ---------------------------------------------------------------------------
// Sending (CHIP task)
// ---------------------------------------------------------------------------

/**
 * Runs on the CHIP event loop, so it is safe to touch the controller from here.
 */
static void send_subscription(intptr_t arg)
{
    uint64_t node_id = (uint64_t)arg;

    refresh_icd_flag(node_id);

    // What we subscribe to is decided by what the HEM needs of the node. Every wanted node gets the
    // power measurement and battery paths (harmless where absent, since the endpoint is wildcard);
    // the tariff source also gets the Commodity Tariff and Commodity Price clusters, and the
    // PriceChange event, on its tariff endpoint. A device lacking one of the clusters just answers
    // that path with an unsupported status.
    uint64_t tariff_node     = 0;
    uint16_t tariff_endpoint = 0;
    bool wants_tariff = tariff_get_source(&tariff_node, &tariff_endpoint) && tariff_node == node_id;

    ScopedMemoryBufferWithSize<AttributePathParams> attr_paths;
    attr_paths.Alloc(4 + (wants_tariff ? 2 : 0));

    if (!attr_paths.Get())
    {
        ESP_LOGE(TAG, "Failed to alloc memory for attribute paths");
        mark_node_has_no_subscription(node_id, 0);
        subscription_attempt_finished(node_id);
        return;
    }

    size_t path_index = 0;

    // Endpoint left wildcard: a node can expose several electrical sensor endpoints (a multi-channel
    // meter, an inverter's PV strings and battery), and one subscription per node covers them all.
    attr_paths[path_index++] = AttributePathParams(ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::Voltage::Id);
    attr_paths[path_index++] = AttributePathParams(ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::ActiveCurrent::Id);
    attr_paths[path_index++] = AttributePathParams(ElectricalPowerMeasurement::Id, ElectricalPowerMeasurement::Attributes::ActivePower::Id);

    // Battery state of charge. Only battery power sources expose it; an unsupported attribute on a
    // wildcard path is skipped rather than reported as an error.
    attr_paths[path_index++] = AttributePathParams(PowerSource::Id, PowerSource::Attributes::BatPercentRemaining::Id);

    if (wants_tariff)
    {
        // One cluster-wide path keeps the per-subscription path count low.
        attr_paths[path_index++] = AttributePathParams(tariff_endpoint, CommodityTariff::Id);
        attr_paths[path_index++] = AttributePathParams(tariff_endpoint, CommodityPrice::Id);
    }

    // PriceChange events buffered by the device while we were away are replayed on resubscribe,
    // which fills gaps in the recorded price history.
    ScopedMemoryBufferWithSize<EventPathParams> event_paths;
    event_paths.Alloc(wants_tariff ? 1 : 0);
    if (wants_tariff)
    {
        if (!event_paths.Get())
        {
            ESP_LOGE(TAG, "Failed to alloc memory for event paths");
            mark_node_has_no_subscription(node_id, 0);
            subscription_attempt_finished(node_id);
            return;
        }
        event_paths[0] = EventPathParams(tariff_endpoint, CommodityPrice::Id, CommodityPrice::Events::PriceChange::Id);
    }

    ESP_LOGI(TAG, "Subscribing to node 0x%016llX (%u path(s): electrical battery%s)", node_id,
             (unsigned)path_index, wants_tariff ? " tariff price" : "");

    auto *cmd = Platform::New<esp_matter::controller::subscribe_command>(node_id,
                                                                         std::move(attr_paths),
                                                                         std::move(event_paths),
                                                                         SUBSCRIBE_MIN_INTERVAL,
                                                                         SUBSCRIBE_MAX_INTERVAL,
                                                                         false, // auto resubscribe
                                                                         matter_controller_attribute_data_cb,
                                                                         matter_controller_event_data_cb,
                                                                         node_subscription_established_cb,
                                                                         node_subscription_terminated_cb,
                                                                         node_subscribe_failed_cb,
                                                                         false); // keep subscription

    if (!cmd)
    {
        ESP_LOGE(TAG, "Failed to alloc memory for subscribe_command");
        mark_node_has_no_subscription(node_id, 0);
        subscription_attempt_finished(node_id);
        return;
    }

    esp_err_t err = cmd->send_command();

    if (err != ESP_OK)
    {
        // send_command() deletes the command on failure without calling any of the callbacks, so
        // nothing else would free the slot or clear the pending flag. The sweep picks it up again.
        ESP_LOGE(TAG, "Failed to send subscribe command: %s", esp_err_to_name(err));
        mark_node_has_no_subscription(node_id, 0);
        subscription_attempt_finished(node_id);
    }
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

/**
 * Look for nodes that should have a subscription but don't, and queue them.
 *
 * ICD (sleepy) nodes are skipped once known: they are only worth subscribing to when they check
 * in, which on_icd_checkin_callback handles.
 */
static void sweep_for_unsubscribed_nodes(void)
{
    uint64_t nodes[MAX_NODES];
    size_t n = wanted_nodes(nodes, MAX_NODES);

    ESP_LOGI(TAG, "Sweep: %u node(s) need a subscription (electrical sensors + tariff source)", (unsigned)n);

    for (size_t i = 0; i < n; i++)
    {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        node_state_t *node = find_node_locked(nodes[i], true);
        bool needs = node && !node->is_icd && !node->has_subscription && !node->is_subscription_pending;
        xSemaphoreGive(s_state_mutex);

        if (needs)
        {
            ESP_LOGI(TAG, "Sweep found node 0x%016llX without a subscription", nodes[i]);
            enqueue_subscription(nodes[i]);
        }
    }
}

static void subscription_task(void *arg)
{
    ESP_LOGI(TAG, "Subscription worker started");

    TickType_t last_sweep = xTaskGetTickCount();

    while (true)
    {
        TickType_t since_sweep = xTaskGetTickCount() - last_sweep;
        if (since_sweep >= pdMS_TO_TICKS(SWEEP_INTERVAL_MS))
        {
            sweep_for_unsubscribed_nodes();
            last_sweep  = xTaskGetTickCount();
            since_sweep = 0;
        }

        uint64_t node_id = 0;

        if (xQueueReceive(s_queue, &node_id, pdMS_TO_TICKS(SWEEP_INTERVAL_MS) - since_sweep) == pdTRUE)
        {
            // State may have moved on while the request sat in the queue.
            if (!node_is_wanted(node_id))
            {
                ESP_LOGW(TAG, "Dropping queued subscription for node 0x%016llX; it is no longer needed", node_id);
                mark_node_has_no_subscription(node_id, 0);
                continue;
            }

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            node_state_t *node = find_node_locked(node_id, false);
            bool subscribed = node && node->has_subscription;
            xSemaphoreGive(s_state_mutex);

            if (subscribed)
            {
                ESP_LOGI(TAG, "Node 0x%016llX already subscribed; skipping", node_id);
                continue;
            }

            // Waits here while MAX_ATTEMPTS_IN_FLIGHT attempts are outstanding. The slot is freed by
            // subscription_attempt_finished(), from the subscription callbacks above.
            claim_slot(node_id);

            if (DeviceLayer::PlatformMgr().ScheduleWork(send_subscription, (intptr_t)node_id) != CHIP_NO_ERROR)
            {
                // send_subscription will never run, so nothing else would free the slot or clear
                // the pending flag. The sweep picks the node up again.
                ESP_LOGE(TAG, "Failed to schedule subscription for node 0x%016llX", node_id);

                mark_node_has_no_subscription(node_id, 0);
                subscription_attempt_finished(node_id);
            }

            vTaskDelay(pdMS_TO_TICKS(SUBSCRIBE_PACING_MS));
        }
        // A timeout just means the next sweep is due; the top of the loop runs it.
    }
}

esp_err_t enqueue_subscription(uint64_t node_id)
{
    if (!s_queue)
    {
        ESP_LOGE(TAG, "Subscription manager not initialised");
        return ESP_ERR_INVALID_STATE;
    }

    // Marking it pending is also what stops the sweep queueing the same node again.
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    node_state_t *node = find_node_locked(node_id, true);
    bool already_pending = node && node->is_subscription_pending;
    if (node) node->is_subscription_pending = true;
    xSemaphoreGive(s_state_mutex);

    if (!node)
    {
        ESP_LOGW(TAG, "Node table full; cannot track node 0x%016llX", node_id);
        return ESP_ERR_NO_MEM;
    }
    if (already_pending)
    {
        ESP_LOGI(TAG, "Subscription for node 0x%016llX already queued or in flight", node_id);
        return ESP_OK;
    }

    if (xQueueSend(s_queue, &node_id, 0) != pdTRUE)
    {
        // Clear the flag again, otherwise the node would look permanently in-flight and the sweep
        // would never pick it back up.
        mark_node_has_no_subscription(node_id, 0);

        ESP_LOGW(TAG, "Subscription queue full; dropped node 0x%016llX (the sweep will retry)", node_id);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Queued subscription for node 0x%016llX", node_id);

    return ESP_OK;
}

esp_err_t subscription_manager_start(void)
{
    if (s_queue)
    {
        return ESP_OK;
    }

    s_state_mutex = xSemaphoreCreateMutex();
    s_slots_mutex = xSemaphoreCreateMutex();

    if (!s_state_mutex || !s_slots_mutex)
    {
        ESP_LOGE(TAG, "Failed to create subscription mutexes");
        return ESP_ERR_NO_MEM;
    }

    s_queue = xQueueCreate(SUBSCRIPTION_QUEUE_DEPTH, sizeof(uint64_t));

    if (!s_queue)
    {
        ESP_LOGE(TAG, "Failed to create subscription queue");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(subscription_task, "matter_sub", SUBSCRIPTION_TASK_STACK, NULL, SUBSCRIPTION_TASK_PRIORITY, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create subscription task");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    DeviceLayer::PlatformMgr().LockChipStack();
    esp_matter::controller::matter_controller_client::get_instance().set_icd_client_callback(on_icd_checkin_callback, nullptr);
    DeviceLayer::PlatformMgr().UnlockChipStack();

    // Queue everything we need now rather than waiting a full sweep interval.
    sweep_for_unsubscribed_nodes();

    return ESP_OK;
}
