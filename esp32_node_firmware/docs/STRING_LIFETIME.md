# String / pointer lifetime convention

> Codebase-wide convention for passing C-string pointers to library APIs that
> may store the pointer instead of copying it. Written 2026-04-23 as part of
> the v0.4.02 audit — to stop the v0.1.7 → v0.3.30 → v0.3.31 use-after-free
> shape from recurring.

## The shape of the bug

A function takes `const char*`. The caller passes `someString.c_str()`. The
called function stores the pointer in a member variable. By the next time
the library uses that pointer, `someString` has gone out of scope and its
heap buffer has been freed (or — worse — overwritten by another String).
The next call dereferences freed memory, occasionally producing the symptom
the user actually sees (corrupted MQTT client ID, dropped LWT, garbled topic).

This has bitten this codebase three times that we have explicit records of:

- **v0.1.7** — `AsyncMqttClient::setClientId(String("ESP-" + mac).c_str())` in
  `mqttBegin()`. The temporary `String` was destroyed at the semicolon. Every
  subsequent reconnect dereferenced freed memory.
- **v0.3.30** — `_mqttClient.setWill(localStringInOnConnect.c_str(), ...)`.
  The local `String` was destroyed when `onMqttConnect` returned. Same shape.
- **v0.3.31** — confirmed v0.3.30 fix held across reboot + reconnect cycles.

## The convention

Any C-string pointer passed to a library function whose contract is "stores
the pointer" — instead of "copies the bytes" — must:

1. Come from a **module-static** `String` or `char[]` (not a local variable,
   not a function-local `String`, not a temporary, not the return value of
   another function).
2. Be **labelled** at its declaration with a `// LIFETIME: <api>` comment so
   it is grep-able. Example from `include/mqtt_client.h`:

   ```cpp
   // LIFETIME: the next four globals are passed by .c_str() / raw-pointer
   // to AsyncMqttClient setters that STORE the pointer without copying.
   // They MUST remain alive (module-static) for the lifetime of _mqttClient.
   static String _mqttClientId;     // LIFETIME: setClientId()
   static String _mqttHost;         // LIFETIME: setServer()
   static String _mqttWillTopic;    // LIFETIME: setWill()
   ```

3. Stay at module scope for as long as the library that holds the pointer
   stays alive. For our codebase that means "for the lifetime of the
   firmware" — the network stack is never torn down outside of OTA / reboot.

## Known dangerous APIs (audit list)

These functions are documented or known to STORE the pointer, not copy. If
you call any of these, the source string MUST be module-static and labelled
with `LIFETIME:`.

| Library | Function | Notes |
|---|---|---|
| AsyncMqttClient | `setClientId(const char*)` | Stored in `_clientId` |
| AsyncMqttClient | `setCredentials(const char*, const char*)` | Both stored |
| AsyncMqttClient | `setServer(const char*, uint16_t)` | Host stored |
| AsyncMqttClient | `setWill(const char*, ..., const char* payload, ...)` | Topic + payload stored |
| esp_https_server | `httpd_register_uri_handler(... uri ...)` | URI string stored |
| esp_http_client | `esp_http_client_config_t.url` | Pointer stored — config struct must outlive the client OR the config call must copy URL into a static |
| esp_now | `esp_now_add_peer(esp_now_peer_info_t*)` | Struct copied; safe |

This is **not exhaustive**. When adding a new library or new setter call,
read the library source — `_member = arg;` (without strdup / std::string copy)
is the smoking gun.

## Known safe APIs (no LIFETIME annotation needed)

These COPY the bytes; you can pass a temporary `String` or local buffer:

- `mqttClient.publish(topic, qos, retain, payload)` — payload is copied into
  the AsyncTCP send buffer immediately.
- `Preferences::putString(key, value)` — copied into NVS.
- `Serial.printf(...)`, `LOG_*` macros — formatted into a stack buffer.
- `String(...)` constructor / concatenation — heap-managed.
- `WiFi.begin(ssid, password)` — internally `strncpy`'d.

## Compile-time guard

`include/lib_api_assert.h` (added in v0.4.02) `static_assert`s the function
signatures of the dangerous APIs above. If a library bump silently changes
`setClientId(const char*)` to `setClientId(String)` or to `setClientId(const
std::string&)`, the build fails immediately. The assert error message points
back to this doc.

## Audit checklist for new code

When you add a `.c_str()` call:

1. Is the called function in the **dangerous list** above? If no, ignore.
2. Is the source `String` module-static? If yes, ignore.
3. If no — promote the `String` to module scope, label with `LIFETIME:`,
   and update the dangerous-APIs table above if this is a new API.

When you add a new third-party library that takes `const char*`:

1. Read its source. Does it copy or store?
2. If stores — add the function to the dangerous-APIs table above.
3. Add a `static_assert` for its signature in `lib_api_assert.h`.
