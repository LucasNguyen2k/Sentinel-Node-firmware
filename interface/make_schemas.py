#!/usr/bin/env python3
"""Generate the JSON Schemas of the Edge HAL interface (MQTT channel sentinel/v1 and its "file ready"
messages) into interface/schemas/sentinel-v1/. Every schema is self-contained (common fields copied in),
so any JSON Schema 2020-12 validator can use a file on its own. index.json maps topics to schemas.

    python3 interface/make_schemas.py
"""
import copy
import json
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "schemas", "sentinel-v1")
VERSION = "1.0"

TS = {"type": "string", "pattern": r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$"}
TS_OR_NULL = {"anyOf": [TS, {"type": "null"}]}
NUM_OR_NULL = {"type": ["number", "null"]}
INT_OR_NULL = {"type": ["integer", "null"]}
BOOL = {"type": "boolean"}
DEVICE_ID = {"type": "string", "pattern": r"^SN-[0-9A-F]{12}$"}
EVENT_ID = {"type": "string", "pattern": r"^[EM]\d+_\d+$"}          # the node's own event id; unique with device_id
EVENT_ID_OR_NULL = {"anyOf": [EVENT_ID, {"type": "null"}]}
COMMON = {
    "device_id": DEVICE_ID,
    "dataset_id": {"type": "string"},
    "timestamp": TS,
    "seq": {"type": "integer", "minimum": 1},
    "boot_id": {"type": "string", "pattern": r"^b\d+$"},
    "schema_version": {"type": "string", "pattern": r"^1\.\d+$"},
}
COMMON_REQ = list(COMMON)


def schema(name, title, dataset, props, required, extra=None):
    s = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": "https://eyezense.local/schemas/sentinel-v1/%s.schema.json" % name,
        "title": title,
        "type": "object",
        "properties": dict(copy.deepcopy(COMMON), **props),
        "required": COMMON_REQ + required,
        "additionalProperties": True,          # minor versions may add optional fields
    }
    if dataset is not None:
        s["properties"]["dataset_id"] = {"enum": dataset} if isinstance(dataset, list) else {"const": dataset}
    if extra:
        s.update(extra)
    return name, s


def path_for(ext):
    return {"type": "string", "pattern": r"^SN-[0-9A-F]{12}/[A-Z]+-\d{2}/\d{8}/\d{8}_\d{6}_\d{6}\.%s$" % ext}


FILE_COMMON = {"size": {"type": "integer", "minimum": 1}, "sha256": {"type": "string", "pattern": "^[0-9a-f]{64}$"}}
WINDOW = {"recorded_from": TS, "recorded_to": TS}
TRIGGER = {"enum": ["event", "command"]}

SCHEMAS = dict([
    schema("node-birth", "Node birth (NODE-01..04), retained", "NODE", {
        "board_name": {"type": "string"},
        "serial_number": {"type": "string"},
        "ip": {"type": "string"},
        "firmware_version": {"type": "string"},
        "mac_address": {"type": "string", "pattern": r"^([0-9A-F]{2}:){5}[0-9A-F]{2}$"},
        "capabilities": {"type": "object", "required": ["chemical", "acoustic_1", "acoustic_2_ultrasonic", "image"],
                         "properties": {k: BOOL for k in ("chemical", "acoustic_1", "acoustic_2_ultrasonic", "image")}},
        "chemical_adc": {"type": "object", "required": ["converter", "bits", "vref_v", "divider", "resolution_mv"],
                         "properties": {"converter": {"type": "string"}, "bits": {"type": "integer", "minimum": 8, "maximum": 24},
                                        "vref_v": {"type": "number"}, "divider": {"type": "number"},
                                        "circuit_supply_v": NUM_OR_NULL, "resolution_mv": {"type": "number"}}},
        "config": {"type": "object"},
        "storage": {"type": "object", "properties": {
            "flash_mb": INT_OR_NULL, "filesystem": BOOL, "offline_records_minutes": INT_OR_NULL,
            "event_log_entries": INT_OR_NULL, "keeps_event_pictures": BOOL, "keeps_sound_clips": BOOL}},
    }, ["board_name", "serial_number", "firmware_version", "mac_address", "capabilities", "chemical_adc", "config"]),
    schema("heartbeat", "Heartbeat", "HEARTBEAT", {
        "link": {"type": "object", "required": ["online"],
                 "properties": {"online": BOOL, "rssi_dbm": INT_OR_NULL, "wifi_reconnects": INT_OR_NULL}},
        "last_seen": TS_OR_NULL,
        "uptime_s": INT_OR_NULL,
        "health": {"type": "object"},
    }, ["link", "last_seen", "health"]),
    schema("data-CHEM-01", "Chemical baseline reading (CHEM-01)", "CHEM-01", {
        "sensor_values": {"type": "string", "pattern": r"^\d{13},(-?\d+(\.\d+)?)?,(-?\d+(\.\d+)?)?,(-?\d+(\.\d+)?)?$"},
        "fields": {"type": "object", "required": ["time_ms", "temp_c", "rh_pct", "mos_mv"],
                   "properties": {"time_ms": {"type": "integer"}, "temp_c": NUM_OR_NULL, "rh_pct": NUM_OR_NULL, "mos_mv": NUM_OR_NULL}},
        "quality": {"type": "object",
                    "required": ["warmed_up", "calibrated", "sensor_ok", "temp_rh_ok", "time_synced", "maintenance",
                                 "simulated", "mos_resolution_mv"],
                    "properties": dict({k: BOOL for k in ("warmed_up", "calibrated", "sensor_ok", "temp_rh_ok",
                                                          "time_synced", "maintenance", "simulated")},
                                       mos_resolution_mv={"type": "number"})},
        "gas_ppm": NUM_OR_NULL,
    }, ["sensor_values", "fields", "quality"]),
    schema("file-CHEM-02", "Chemical event data file ready (CHEM-02)", "CHEM-02", dict(FILE_COMMON, **WINDOW, **{
        "path": path_for("csv"), "content_type": {"const": "text/csv"}, "rows": {"type": "integer", "minimum": 0},
        "event_id": EVENT_ID, "part": {"type": "integer", "minimum": 1}, "late": BOOL}),
        ["path", "content_type", "size", "sha256", "rows", "event_id", "part", "recorded_from", "recorded_to"]),
    schema("file-ACO-01", "Acoustic baseline file ready (ACO-01, ACO-04)", ["ACO-01", "ACO-04"], dict(FILE_COMMON, **WINDOW, **{
        "path": path_for("wav"), "content_type": {"const": "audio/wav"},
        "time_accuracy_ms": {"type": "integer", "minimum": 0}, "gaps_ms": {"type": "integer", "minimum": 1}}),
        ["path", "content_type", "size", "sha256", "recorded_from", "recorded_to", "time_accuracy_ms"],
        {"not": {"anyOf": [{"required": ["event_id"]}, {"required": ["part"]}]}}),
    schema("file-ACO-02", "Acoustic event file ready (ACO-02, ACO-05)", ["ACO-02", "ACO-05"], dict(FILE_COMMON, **WINDOW, **{
        "path": path_for("wav"), "content_type": {"const": "audio/wav"},
        "event_id": EVENT_ID_OR_NULL, "part": {"type": "integer", "minimum": 1}, "trigger": TRIGGER,
        "command_id": {"type": ["string", "null"]}, "time_accuracy_ms": {"type": "integer", "minimum": 0},
        "recorded_by": {"enum": ["edge", "node"]}, "gaps_ms": {"type": "integer", "minimum": 1}, "late": BOOL}),
        ["path", "content_type", "size", "sha256", "event_id", "part", "trigger", "command_id",
         "recorded_from", "recorded_to", "time_accuracy_ms", "recorded_by"]),
    schema("file-IMG-01", "Image baseline file ready (IMG-01)", "IMG-01", dict(FILE_COMMON, **{
        "path": path_for("jpg"), "content_type": {"const": "image/jpeg"},
        "width": INT_OR_NULL, "height": INT_OR_NULL}),
        ["path", "content_type", "size", "sha256", "width", "height"],
        {"not": {"anyOf": [{"required": ["event_id"]}, {"required": ["part"]}]}}),
    schema("file-IMG-02", "Image event file ready (IMG-02)", "IMG-02", dict(FILE_COMMON, **{
        "path": path_for("jpg"), "content_type": {"const": "image/jpeg"},
        "event_id": EVENT_ID_OR_NULL, "part": {"type": "integer", "minimum": 1}, "trigger": TRIGGER,
        "command_id": {"type": ["string", "null"]}, "width": INT_OR_NULL, "height": INT_OR_NULL, "late": BOOL}),
        ["path", "content_type", "size", "sha256", "event_id", "part", "trigger", "command_id", "width", "height"]),
    schema("event-CHEM-03", "Node-triggered event notification (CHEM-03)", "CHEM-03", {
        "event_id": EVENT_ID, "notification": {"enum": ["start", "record", "end"]},
        "state": {"enum": ["SUSPECT", "CONFIRMED", "NORMAL"]},
        "triggered_by": {"type": "object", "required": ["chemical", "acoustic_1", "acoustic_2_ultrasonic"],
                         "properties": {k: BOOL for k in ("chemical", "acoustic_1", "acoustic_2_ultrasonic")}},
        "mos_mv": NUM_OR_NULL, "maintenance": BOOL, "parts_total": {"type": "integer", "minimum": 0},
        "late": BOOL, "source": {"enum": ["live", "node_event_log"]}},
        ["event_id", "notification", "state", "triggered_by", "maintenance"],
        {"if": {"properties": {"notification": {"const": "end"}}}, "then": {"required": ["parts_total"]}}),
])
for ds, files in (("ACO-03", "ACO-02"), ("IMG-03", "IMG-02")):
    name, s = schema("event-" + ds, "Evidence notification (%s, for %s files)" % (ds, files),
                     [ds, "ACO-06"] if ds == "ACO-03" else ds, {
        "event_id": EVENT_ID_OR_NULL, "command_id": {"type": ["string", "null"]},
        "notification": {"enum": ["start", "record", "end"]},
        "part": {"type": "integer", "minimum": 1}, "path": path_for("wav" if ds == "ACO-03" else "jpg"),
        "size": {"type": "integer", "minimum": 1}, "parts_total": {"type": "integer", "minimum": 0}},
        ["event_id", "notification"],
        {"allOf": [{"if": {"properties": {"notification": {"const": "record"}}}, "then": {"required": ["part", "path", "size"]}},
                   {"if": {"properties": {"notification": {"const": "end"}}}, "then": {"required": ["parts_total"]}}]})
    SCHEMAS[name] = s

CMD = {"command_id": {"type": "string", "minLength": 1}}
for cname, props, req in (
        ("record_audio", {"duration_s": {"type": "number", "exclusiveMinimum": 0, "maximum": 600}, "event_id": EVENT_ID_OR_NULL}, ["duration_s"]),
        ("capture_image", {"count": {"type": "integer", "minimum": 1, "maximum": 10}, "event_id": EVENT_ID_OR_NULL}, []),
        ("stop_recording", {"target_command_id": {"type": "string"}}, ["target_command_id"]),
        ("config_set", {"key": {"type": "string"}, "value": {"type": ["string", "number", "integer"]}}, ["key", "value"])):
    SCHEMAS["cmd-" + cname] = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": "https://eyezense.local/schemas/sentinel-v1/cmd-%s.schema.json" % cname,
        "title": "Command %s (node-gateway -> Edge HAL)" % cname, "type": "object",
        "properties": dict(CMD, **props), "required": ["command_id"] + req, "additionalProperties": True}
SCHEMAS["cmd-result"] = schema("cmd-result", "Command result", "CMD", {
    "command_id": {"type": "string"}, "command": {"type": "string"},
    "state": {"enum": ["accepted", "done", "refused", "expired"]},
    "files": {"type": "array", "items": {"type": "string"}}, "detail": {"type": ["string", "null"]}},
    ["command_id", "command", "state", "files", "detail"])[1]

INDEX = {
    "schema_version": VERSION,
    "topics": [
        {"topic": "sentinel/v1/+/birth", "schema": "node-birth"},
        {"topic": "sentinel/v1/+/heartbeat", "schema": "heartbeat"},
        {"topic": "sentinel/v1/+/data/CHEM-01", "schema": "data-CHEM-01"},
        {"topic": "sentinel/v1/+/file/CHEM-02", "schema": "file-CHEM-02"},
        {"topic": "sentinel/v1/+/file/ACO-01", "schema": "file-ACO-01"},
        {"topic": "sentinel/v1/+/file/ACO-04", "schema": "file-ACO-01"},
        {"topic": "sentinel/v1/+/file/ACO-02", "schema": "file-ACO-02"},
        {"topic": "sentinel/v1/+/file/ACO-05", "schema": "file-ACO-02"},
        {"topic": "sentinel/v1/+/file/IMG-01", "schema": "file-IMG-01"},
        {"topic": "sentinel/v1/+/file/IMG-02", "schema": "file-IMG-02"},
        {"topic": "sentinel/v1/+/event/CHEM-03", "schema": "event-CHEM-03"},
        {"topic": "sentinel/v1/+/event/ACO-03", "schema": "event-ACO-03"},
        {"topic": "sentinel/v1/+/event/ACO-06", "schema": "event-ACO-03"},
        {"topic": "sentinel/v1/+/event/IMG-03", "schema": "event-IMG-03"},
        {"topic": "sentinel/v1/+/cmd/record_audio", "schema": "cmd-record_audio"},
        {"topic": "sentinel/v1/+/cmd/capture_image", "schema": "cmd-capture_image"},
        {"topic": "sentinel/v1/+/cmd/stop_recording", "schema": "cmd-stop_recording"},
        {"topic": "sentinel/v1/+/cmd/config_set", "schema": "cmd-config_set"},
        {"topic": "sentinel/v1/+/cmd_result", "schema": "cmd-result"},
    ],
}

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for name, s in sorted(SCHEMAS.items()):
        with open(os.path.join(OUT, name + ".schema.json"), "w") as f:
            json.dump(s, f, indent=2)
            f.write("\n")
    with open(os.path.join(OUT, "index.json"), "w") as f:
        json.dump(INDEX, f, indent=2)
        f.write("\n")
    print("wrote %d schemas and index.json to %s" % (len(SCHEMAS), OUT))
