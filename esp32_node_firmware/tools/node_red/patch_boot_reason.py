"""Make boot_history_tpl Reason column descriptive (#user request 2026-04-25)."""
import json, urllib.request, urllib.error

ADMIN = "http://127.0.0.1:1880"

NEW_TPL = '''<template>
  <div style="font-family:system-ui;font-size:12px;padding:6px">
    <div style="font-size:11px;color:#95a5a6;margin-bottom:6px">
      {{ msg && msg.payload ? msg.payload.length : 0 }} abnormal reboot(s) tracked.
      Filter: any boot_reason != "poweron". Max 20 entries.
    </div>
    <table v-if="msg && msg.payload && msg.payload.length"
           style="width:100%;border-collapse:collapse">
      <thead>
        <tr style="background:#2c3e50;color:#ecf0f1">
          <th style="text-align:left;padding:4px 8px">When</th>
          <th style="text-align:left;padding:4px 8px">Device</th>
          <th style="text-align:left;padding:4px 8px">Reason</th>
          <th style="text-align:left;padding:4px 8px">Firmware</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="e in msg.payload" :key="e.ts_ms"
            style="border-bottom:1px solid #3a3a3a">
          <td style="padding:3px 8px">{{ e.ts }}</td>
          <td style="padding:3px 8px">{{ e.device }}</td>
          <td style="padding:3px 8px"
              :title="e.reason"
              :style="{
                fontWeight: 600,
                color: ['task_wdt','int_wdt','wdt','other_wdt','panic','brownout'].includes(e.reason) ? '#ff6b6b'
                     : ['software','external','sw_reset'].includes(e.reason) ? '#f1c40f'
                     : '#2ecc71'
              }">
            <span v-if="e.reason === 'task_wdt'">Task watchdog timeout — a FreeRTOS task was unresponsive</span>
            <span v-else-if="e.reason === 'int_wdt'">Interrupt watchdog timeout — ISR exceeded budget</span>
            <span v-else-if="e.reason === 'other_wdt' || e.reason === 'wdt'">Watchdog reset — task or RTC WDT fired</span>
            <span v-else-if="e.reason === 'panic'">Firmware panic / crash</span>
            <span v-else-if="e.reason === 'brownout'">Brownout — supply voltage dropped below ~2.43 V</span>
            <span v-else-if="e.reason === 'software' || e.reason === 'sw_reset'">Clean restart (esp_restart from firmware)</span>
            <span v-else-if="e.reason === 'external' || e.reason === 'ext'">External reset — RTS pin / hardware EN button</span>
            <span v-else-if="e.reason === 'poweron'">Power-on</span>
            <span v-else-if="e.reason === 'deepsleep'">Wake from deep sleep</span>
            <span v-else>{{ e.reason }}</span>
          </td>
          <td style="padding:3px 8px">{{ e.firmware }}</td>
        </tr>
      </tbody>
    </table>
    <div v-else
         style="color:#7f8c8d;font-style:italic;padding:14px;text-align:center">
      No abnormal reboots recorded. Clean soak so far.
    </div>
  </div>
</template>'''

req = urllib.request.Request(ADMIN + "/flows")
flows = json.loads(urllib.request.urlopen(req).read())
patched = 0
for n in flows:
    if n.get("id") == "boot_history_tpl":
        n["format"] = NEW_TPL
        patched += 1
print(f"Patched {patched} nodes")
body = json.dumps(flows).encode()
req = urllib.request.Request(ADMIN + "/flows", data=body, method="POST",
    headers={"Content-Type":"application/json","Node-RED-Deployment-Type":"nodes"})
try:
    resp = urllib.request.urlopen(req); print("Deploy:", resp.status)
except urllib.error.HTTPError as e:
    print("Deploy failed:", e.status, e.read().decode())
