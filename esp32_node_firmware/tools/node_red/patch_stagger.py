"""One-shot Node-RED patch: add 5-min stagger to fleet OTA/restart buttons (#45)."""
import json, urllib.request, urllib.error

ADMIN = "http://127.0.0.1:1880"

CMD_FN = '''try {
    var base = flow.get("topic_base") || "Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/";
    var devices = flow.get("hb_devices") || {};
    var uuids = Object.keys(devices);
    if (uuids.length === 0) {
        node.warn("No known devices yet \u2014 waiting for first heartbeat");
        return null;
    }
    // Sort by friendly name so the stagger order is deterministic (Alpha, Bravo, ...).
    uuids.sort(function(a, b) {
        var na = (devices[a] && devices[a].name) || a;
        var nb = (devices[b] && devices[b].name) || b;
        return na.localeCompare(nb);
    });
    // 5-min stagger for OTA (prevents AP-association contention and validation
    // cascade; observed v0.4.06). 2-s stagger for the cheaper "check" button.
    var stepMs = (msg.payload === "ota") ? 300000 : 2000;
    uuids.forEach(function(uuid, i) {
        setTimeout(function() {
            node.send({ topic: base + uuid + "/cmd/ota_check", payload: "" });
        }, i * stepMs);
    });
    var label = (msg.payload === "ota") ? "OTA" : "Check";
    node.status({fill:"blue", shape:"dot",
                 text: label + ": " + uuids.length + " devices, " + (stepMs/1000) + "s apart"});
    return null;
} catch(e) {
    node.error("Publish to All Devices: " + e.message, msg);
    return null;
}'''

RESTART_FN = '''try {
    var base    = flow.get("topic_base") || "Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/";
    var devices = flow.get("hb_devices") || {};
    var uuids   = Object.keys(devices);
    if (uuids.length === 0) {
        node.warn("No known devices \u2014 waiting for first heartbeat");
        return null;
    }
    uuids.forEach(function(u) {
        if (devices[u] && devices[u].status === "Connected") {
            devices[u].status = "Restarting";
        }
    });
    flow.set("hb_devices", devices);
    global.set("hb_devices", devices);
    var sorted = Object.values(devices).sort(function(a,b){ return a.name.localeCompare(b.name); });
    // Same sort key as the status table so the visible "Restarting" order matches the wire order.
    uuids.sort(function(a, b) {
        var na = (devices[a] && devices[a].name) || a;
        var nb = (devices[b] && devices[b].name) || b;
        return na.localeCompare(nb);
    });
    var stepMs = 300000;  // 5 min — same rationale as OTA stagger (#45)
    uuids.forEach(function(uuid, i) {
        setTimeout(function() {
            node.send([{ topic: base + uuid + "/cmd/restart", payload: "" }, null]);
        }, i * stepMs);
    });
    node.send([null, { payload: sorted }]);
    node.status({fill:"red", shape:"dot",
                 text:"Restart: " + uuids.length + " devices, 300s apart"});
    return null;
} catch(e) {
    node.error("Restart All Devices: " + e.message, msg);
    return null;
}'''

req = urllib.request.Request(ADMIN + "/flows")
flows = json.loads(urllib.request.urlopen(req).read())

patched = 0
for n in flows:
    if n.get("id") == "hb_cmd_fn":
        n["func"] = CMD_FN; patched += 1
    elif n.get("id") == "hb_restart_fn":
        n["func"] = RESTART_FN; patched += 1

print(f"Patched {patched} nodes")

body = json.dumps(flows).encode()
req = urllib.request.Request(
    ADMIN + "/flows",
    data=body,
    method="POST",
    headers={
        "Content-Type": "application/json",
        "Node-RED-Deployment-Type": "nodes",
    },
)
try:
    resp = urllib.request.urlopen(req)
    print("Deploy:", resp.status, resp.read().decode())
except urllib.error.HTTPError as e:
    print("Deploy failed:", e.status, e.read().decode())
