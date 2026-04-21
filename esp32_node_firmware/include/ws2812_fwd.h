#pragma once

// =============================================================================
// ws2812_fwd.h  —  Forward declarations for ws2812.h public API
//
// Include this instead of ws2812.h when you only need to call ws2812PostEvent()
// or ws2812PublishState() (e.g. from mqtt_client.h or rfid.h).
//
// ws2812.h defines the LedEvent struct and LedEventType enum, so this header
// must repeat the minimum type information needed for the function signatures.
// It does NOT redefine the full LedEvent — ws2812.h must still be included
// once (before any call site) in the translation unit.
//
// In practice ws2812.h is included before mqtt_client.h in esp32_firmware.ino,
// so these forward declarations are a belt-and-braces guard against future
// reordering. Any reorder that breaks compilation will now give a clear
// "undefined type LedEvent" error rather than a mysterious link failure.
// =============================================================================

// Declared in ws2812.h — included first in esp32_firmware.ino
struct LedEvent;

void ws2812PublishState();
void ws2812PostEvent(const LedEvent& e);
