"""Phase 2: extend OTA stagger with canary cancel-on-failure (#35)."""
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
    // 5-min stagger for OTA: prevents AP-association contention AND gives each
    // device's post-OTA validation window time to complete (#35, #45).
    // 2-s for the cheap "check" button (no download → no AP contention).
    var isOta  = (msg.payload === "ota");
    var stepMs = isOta ? 300000 : 2000;
    var label  = isOta ? "OTA" : "Check";
    var chainStartMs = Date.now();
    var triggered = [];   // closure-shared: each setTimeout sees prior pushes

    flow.set("hb_ota_chain_started_ms", isOta ? chainStartMs : 0);
    flow.set("hb_ota_chain_total",      uuids.length);
    flow.set("hb_ota_chain_done",       0);

    uuids.forEach(function(uuid, i) {
        setTimeout(function() {
            // Canary gate: before triggering device i+1, check if any device
            // already triggered has rebooted abnormally since the chain started.
            // boot_history_fn populates this list with deduped entries.
            if (isOta && i > 0) {
                var hist = flow.get("boot_history") || [];
                var bad = hist.find(function(e) {
                    return triggered.indexOf(e.uuid) !== -1 &&
                           (e.ts_ms || 0) > chainStartMs &&
                           ["task_wdt","int_wdt","panic","brownout"].indexOf(e.reason) !== -1;
                });
                if (bad) {
                    node.warn("OTA chain ABORTED at step " + (i+1) + "/" + uuids.length +
                              ": " + bad.device + " rebooted with " + bad.reason);
                    node.status({fill:"red", shape:"ring",
                                 text:"OTA aborted: " + bad.device + " " + bad.reason});
                    flow.set("hb_ota_chain_started_ms", 0);
                    return;
                }
            }
            node.send({ topic: base + uuid + "/cmd/ota_check", payload: "" });
            triggered.push(uuid);
            flow.set("hb_ota_chain_done", triggered.length);
            var devName = (devices[uuid] && devices[uuid].name) || uuid.slice(0,8);
            node.status({fill:"blue", shape:"dot",
                         text: label + ": " + triggered.length + "/" + uuids.length +
                               " (" + devName + ")"});
        }, i * stepMs);
    });
    node.status({fill:"blue", shape:"ring",
                 text: label + ": queued " + uuids.length + " devices, " + (stepMs/1000) + "s apart"});
    return null;
} catch(e) {
    node.error("Publish to All Devices: " + e.message, msg);
    return null;
}'''

req = urllib.request.Request(ADMIN + "/flows")
flows = json.loads(urllib.request.urlopen(req).read())

patched = 0
for n in flows:
    if n.get("id") == "hb_cmd_fn":
        n["func"] = CMD_FN; patched += 1

print(f"Patched {patched} nodes")
body = json.dumps(flows).encode()
req = urllib.request.Request(ADMIN + "/flows", data=body, method="POST",
    headers={"Content-Type":"application/json","Node-RED-Deployment-Type":"nodes"})
try:
    resp = urllib.request.urlopen(req)
    print("Deploy:", resp.status)
except urllib.error.HTTPError as e:
    print("Deploy failed:", e.status, e.read().decode())
