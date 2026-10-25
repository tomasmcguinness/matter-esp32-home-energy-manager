#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Live state of the Matter appliance devices linked to appliance nodes.
//
// An appliance node is metered by its settings.nodeId/endpointId (an Electrical
// Sensor). It may also carry settings.device = {nodeId, endpointId, deviceType,
// label}: a commissioned Dishwasher (0x0075) or Laundry Washer (0x0073) endpoint,
// e.g. a bridged appliance. The link adds what a meter can't see: the operational
// state, the selected program (the Dishwasher / Laundry Washer Mode) and the
// phase. The meter stays the source of power.
//
// Programs are keyed by a per-appliance catalogue the HEM owns, not by the
// device's mode value: a bridge may renumber its modes when the appliance's list
// of available programs changes. A label seen for the first time is appended;
// its index is the program id, stable for the life of the graph node.
//   catalogue : /sdcard/programs-<graphId>              (JSON array of labels)
//   state log : /sdcard/state-<graphId>-YYYY-MM-DD      (appliance_state_record_t per minute)

#ifdef __cplusplus
extern "C" {
#endif

#define APPLIANCE_STATE_MAX_LINKS   8
#define APPLIANCE_PROGRAM_MAX       16
#define APPLIANCE_LABEL_LEN         32
#define APPLIANCE_UNKNOWN           0xFF  // op state, program id or phase not known

// Matter OperationalStateEnum values the HEM interprets.
#define APPLIANCE_OP_STOPPED        0x00
#define APPLIANCE_OP_RUNNING        0x01
#define APPLIANCE_OP_PAUSED         0x02
#define APPLIANCE_OP_ERROR          0x03

#define APPLIANCE_STATE_FLAG_REACHABLE 0x01 // subscribed and the bridge reports the appliance reachable

// One per-minute record in the state log, appended by appliance_state_log_minute().
typedef struct {
    uint32_t unix_minute;
    uint8_t  op_state;   // OperationalStateEnum, APPLIANCE_UNKNOWN if not reachable
    uint8_t  program_id; // catalogue id, APPLIANCE_UNKNOWN if none
    uint8_t  phase;      // index into the device's PhaseList, APPLIANCE_UNKNOWN if null
    uint8_t  flags;      // APPLIANCE_STATE_FLAG_*
} appliance_state_record_t;

typedef struct {
    uint64_t node_id;
    uint16_t endpoint_id;
    uint32_t device_type;
    bool     reachable;
    uint8_t  op_state;                         // APPLIANCE_UNKNOWN until reported
    uint8_t  program_id;                       // catalogue id of the current mode
    char     program[APPLIANCE_LABEL_LEN];     // "" if unknown
    uint8_t  phase;                            // APPLIANCE_UNKNOWN if null
    char     phase_name[APPLIANCE_LABEL_LEN];  // "" if unknown
    int32_t  countdown_s;                      // -1 if null
    uint32_t last_report_unix;                 // 0 if nothing reported yet
} appliance_live_t;

// Re-read the links from the topology graph. With resubscribe, nodes whose set of
// linked endpoints changed get a fresh subscription, so newly linked devices start
// reporting without a restart. Call at init (resubscribe false) and after every
// graph edit.
void appliance_state_refresh_links(bool resubscribe);

// The Matter nodes that carry at least one linked appliance endpoint.
size_t appliance_state_linked_nodes(uint64_t *out, size_t max);

// The linked endpoints on one node and the Mode cluster each one is read through
// (DishwasherMode or LaundryWasherMode; 0 when the device type has none).
size_t appliance_state_linked_endpoints(uint64_t node_id, uint16_t *endpoints,
                                        uint32_t *mode_clusters, size_t max);

// Live state of the device linked to an appliance node. Returns false if the
// node has no link.
bool appliance_state_get(const char *graph_id, appliance_live_t *out);

// Label of a catalogue program id. Returns false if the id is not in the catalogue.
bool appliance_state_program_name(const char *graph_id, uint8_t program_id, char *buf, size_t len);

// Append the current state of every linked appliance to its state log. Called once
// a minute by the node power logger's flush.
void appliance_state_log_minute(void);

#ifdef __cplusplus
}

#include <app/ConcreteAttributePath.h>
#include <lib/core/TLVReader.h>

// Feed one Operational State, Dishwasher Mode, Laundry Washer Mode or Bridged
// Device Basic Information report. Reports for endpoints that aren't linked are
// ignored. Runs on the CHIP thread.
void appliance_state_on_attribute(uint64_t node_id, const chip::app::ConcreteDataAttributePath &path,
                                  chip::TLV::TLVReader *data);
#endif
