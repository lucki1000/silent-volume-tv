#!/usr/bin/env python3
"""Compile the actual HTTP worker header against fake Arduino/FreeRTOS APIs.

Checks requests and volume-key policy without real Wi-Fi or TV side effects.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BRIDGE = (ROOT / "BLEHIDBridge/BLEHIDBridge.ino").read_text()


def extract(signature):
    start = BRIDGE.index(signature)
    end = BRIDGE.index("{", start) + 1
    depth = 1
    while depth:
        depth += (BRIDGE[end] == "{") - (BRIDGE[end] == "}")
        end += 1
    return BRIDGE[start:end]


STUBS = r'''
#pragma once
#include <cassert>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>
using String = std::string;
uint32_t fakeMillis = 1000;
uint32_t millis() { return fakeMillis; }
struct SerialStub {
    std::string log;
    void println(const char* text) { log += std::string(text) + "\n"; }
    template<class... Args> void printf(const char* fmt, Args... args) {
        char buffer[1024]; std::snprintf(buffer, sizeof(buffer), fmt, args...); log += buffer;
    }
} Serial;
struct IPAddress {
    bool fromString(const char* value) { return std::string(value) == "192.168.1.50"; }
    String toString() { return "192.168.1.60"; }
};
constexpr int WL_CONNECTED = 3, WIFI_STA = 1;
struct WiFiStub {
    int state = 0, reconnects = 0, begins = 0;
    int status() { return state; }
    IPAddress localIP() { return {}; }
    void mode(int mode) { assert(mode == WIFI_STA); }
    void setAutoReconnect(bool value) { assert(value); }
    void begin(const char*, const char*) { ++begins; }
    void reconnect() { ++reconnects; }
} WiFi;
struct WiFiClient {};
constexpr int HTTPC_DISABLE_FOLLOW_REDIRECTS = 0;
int response = 204, posts = 0, ends = 0;
bool httpBeginOk = true;
String requestHost, requestPath, requestBody;
uint16_t requestPort = 0;
std::map<String, String> headers;
struct HTTPClient {
    void setConnectTimeout(int32_t value) { assert(value == 1000); }
    void setTimeout(uint16_t value) { assert(value == 1500); }
    void setReuse(bool value) { assert(!value); }
    void setFollowRedirects(int value) { assert(value == HTTPC_DISABLE_FOLLOW_REDIRECTS); }
    bool begin(WiFiClient&, String host, uint16_t port, String path, bool https) {
        assert(!https); requestHost = host; requestPort = port; requestPath = path;
        headers.clear(); return httpBeginOk;
    }
    void addHeader(const String& name, const String& value) { headers[name] = value; }
    int POST(String body) { ++posts; requestBody = body; return response; }
    void end() { ++ends; }
};
struct Queue { size_t capacity, itemSize; std::deque<std::vector<uint8_t>> items; };
using QueueHandle_t = Queue*;
constexpr int pdTRUE = 1, pdPASS = 1;
bool queueFailure = false, taskFailure = false;
int deletedQueues = 0;
QueueHandle_t xQueueCreate(size_t capacity, size_t itemSize) {
    return queueFailure ? nullptr : new Queue{capacity, itemSize, {}};
}
void vQueueDelete(QueueHandle_t queue) { ++deletedQueues; delete queue; }
int xQueueSend(QueueHandle_t queue, const void* item, uint32_t wait) {
    assert(wait == 0); // The Arduino loop must never wait for this worker.
    if (queue->items.size() >= queue->capacity) return 0;
    const auto* bytes = static_cast<const uint8_t*>(item);
    queue->items.emplace_back(bytes, bytes + queue->itemSize); return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue, void* item, uint32_t) {
    if (queue->items.empty()) return 0;
    memcpy(item, queue->items.front().data(), queue->itemSize); queue->items.pop_front(); return pdTRUE;
}
void xQueueReset(QueueHandle_t queue) { queue->items.clear(); }
size_t uxQueueMessagesWaiting(QueueHandle_t queue) { return queue->items.size(); }
uint32_t pdMS_TO_TICKS(uint32_t value) { return value; }
int xTaskCreate(void (*fn)(void*), const char*, uint32_t stack, void*, unsigned priority, void*) {
    assert(fn && stack >= 6144 && priority == 1); return taskFailure ? 0 : pdPASS;
}
namespace bridgeconfig {
char WIFI_SSID[64] = "test-wifi", WIFI_PASSWORD[64] = "test-password-do-not-log";
char TV_IP[64] = "192.168.1.50", API_TOKEN[64] = "test-token-do-not-log";
uint16_t TV_HTTP_PORT = 8090;
uint32_t VOLUME_REPEAT_MS = 200;
constexpr uint32_t VOLUME_MAX_AGE_MS = 2000, WIFI_RETRY_MS = 10000;
constexpr int32_t HTTP_CONNECT_TIMEOUT_MS = 1000;
constexpr uint16_t HTTP_READ_TIMEOUT_MS = 1500;
}
'''

CHECKS = r'''
using namespace volumehttp;
Action take() {
    Action action{}; assert(xQueueReceive(actions, &action, 0) == pdTRUE); return action;
}
int main() {
    assert(validConfig());
    bridgeconfig::API_TOKEN[0] = '\0';
    begin(); assert(!enabled && !actions);
    onKey(0xE9, true, false); assert(posts == 0);
    strcpy(bridgeconfig::API_TOKEN, "bad\r\ntoken"); assert(!validConfig());
    strcpy(bridgeconfig::API_TOKEN, "test-token-do-not-log");
    strcpy(bridgeconfig::TV_IP, "http://192.168.1.50"); assert(!validConfig());
    strcpy(bridgeconfig::TV_IP, "192.168.1.50");
    queueFailure = true; begin(); assert(!enabled && !actions);
    queueFailure = false; taskFailure = true; begin(); assert(!enabled && !actions && deletedQueues == 1);
    taskFailure = false; begin(); assert(enabled && actions);
    uint32_t lastAttempt = millis();

    // Offline presses do not create a backlog or wait for network recovery.
    const auto beforeOffline = dropped.load();
    onKey(0xE9, true, false); assert(actions->items.empty() && dropped == beforeOffline + 1);
    fakeMillis += 10000; maintainWifi(lastAttempt); assert(WiFi.reconnects == 1 && !wifiReady);
    WiFi.state = WL_CONNECTED; maintainWifi(lastAttempt); assert(wifiReady);
    const auto generation = wifiSession.load();

    // A quick tap still sends its initial press, even after its release.
    onKey(0xE9, true, false); onKey(0xE9, false, false);
    processAction(take());
    assert(posts == 1 && succeeded == 1 && failed == 0 && ends == 1);
    assert(requestHost == "192.168.1.50" && requestPort == 8090 && requestPath == "/volume/up");
    assert(headers["Authorization"] == "Bearer test-token-do-not-log");
    assert(headers["Content-Length"] == "0" && requestBody.empty());
    onKey(0xEA, true, false); processAction(take());
    assert(posts == 2 && requestPath == "/volume/down");
    onKey(0xEA, false, false); assert(actions->items.empty());
    onKey(0x42, true, false); assert(actions->items.empty());

    // Throttle only held reports: every independent press remains immediate.
    onKey(0xE9, true, false); processAction(take());
    fakeMillis += 199; onKey(0xE9, true, true); assert(actions->items.empty());
    fakeMillis += 1; onKey(0xE9, true, true); assert(actions->items.size() == 1);
    processAction(take()); assert(posts == 4);
    onKey(0xE9, false, false); onKey(0xE9, true, false); assert(actions->items.size() == 1);
    processAction(take()); assert(posts == 5);
    bridgeconfig::VOLUME_REPEAT_MS = 0;
    fakeMillis += 1000; onKey(0xE9, true, true); assert(actions->items.empty());
    bridgeconfig::VOLUME_REPEAT_MS = 200;

    // Queued held repeats are canceled on release or the next press generation.
    onKey(0xE9, true, true); Action repeat = take();
    onKey(0xE9, false, false); processAction(repeat); assert(posts == 5);
    onKey(0xE9, true, false); processAction(take()); assert(posts == 6);
    processAction(repeat); assert(posts == 6);

    // Stale requests expire, including across millis() wraparound.
    onKey(0xEA, true, false); Action expired = take();
    fakeMillis += 2000; processAction(expired); assert(posts == 6);
    fakeMillis = 0xFFFFFFF0; onKey(0xEA, true, false); Action wrap = take();
    fakeMillis = 0x20; processAction(wrap); assert(posts == 7);

    // Never retry an HTTP rejection or ambiguous transport timeout.
    response = 401; onKey(0xE9, true, false); processAction(take());
    assert(posts == 8 && failed == 1 && lastCode == 401 && actions->items.empty());
    response = -11; onKey(0xEA, true, false); processAction(take());
    assert(posts == 9 && failed == 2 && lastCode == -11 && actions->items.empty());
    httpBeginOk = false; onKey(0xEA, true, false); processAction(take());
    assert(posts == 9 && failed == 3 && lastCode == 0 && ends == 10);
    httpBeginOk = true;

    // Drop pending requests on Wi-Fi loss; generations reject an old dequeued request too.
    onKey(0xE9, true, false); Action oldWifi = take();
    onKey(0xEA, true, false); assert(actions->items.size() == 1);
    WiFi.state = 0; maintainWifi(lastAttempt); assert(!wifiReady && actions->items.empty());
    processAction(oldWifi); assert(posts == 9);
    WiFi.state = WL_CONNECTED; maintainWifi(lastAttempt);
    assert(wifiSession > generation); processAction(oldWifi); assert(posts == 9);

    // Saturation is explicit and never blocks the BLE loop.
    const auto oldDrops = dropped.load();
    for (int i = 0; i < 9; ++i) onKey(0xE9, true, false);
    assert(actions->items.size() == 8 && dropped == oldDrops + 1);

    // Integrate real HID filtering and the sketch's actual volume hooks.
    xQueueReset(actions); response = 204;
    const uint8_t map[] = {
        0x05,0x0C,0x09,0x01,0xA1,0x01,0x85,0x03,0x19,0x00,0x2A,0x9C,0x02,
        0x15,0x00,0x26,0x9C,0x02,0x95,0x02,0x75,0x10,0x81,0x00,0xC0
    };
    assert(descriptor.parse(map, sizeof(map)));
    uint8_t packet[4] = {0xE9, 0, 0x42, 0}, filtered[4], mask = 0;
    assert(descriptor.filter(0, packet, 4, filtered, mask));
    assert(mask == 1 && filtered[0] == 0 && filtered[1] == 0 && filtered[2] == 0x42);
    updateVolume(0, mask); processAction(take());
    assert(posts == 10 && requestPath == "/volume/up");
    const uint8_t shortPacket[2] = {0xEA, 0};
    assert(descriptor.normalizeInput(0, shortPacket, 2, packet));
    assert(descriptor.filter(0, packet, 4, filtered, mask));
    assert(mask == 2 && filtered[0] == 0 && filtered[1] == 0 && filtered[2] == 0 && filtered[3] == 0);
    updateVolume(0, mask); processAction(take());
    assert(posts == 11 && requestPath == "/volume/down");
    clearVolume(); assert(actions->items.empty() && volumeState == 0);
    printStatus();
    assert(Serial.log.find("test-password-do-not-log") == std::string::npos);
    assert(Serial.log.find("test-token-do-not-log") == std::string::npos);
    vQueueDelete(actions); actions = nullptr;
}
'''

with tempfile.TemporaryDirectory(prefix="esp32-volume-http-") as directory:
    directory = Path(directory)
    (directory / "stubs.h").write_text(STUBS)
    for name in ("Arduino.h", "WiFi.h", "HTTPClient.h", "BridgeConfig.h",
                 "freertos/FreeRTOS.h", "freertos/queue.h", "freertos/task.h"):
        header = directory / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "stubs.h"\n')
    (directory / "VolumeHttp.h").write_text((ROOT / "BLEHIDBridge/VolumeHttp.h").read_text())
    code = '#include "VolumeHttp.h"\n'
    code += '#include "' + str(ROOT / "BLEHIDBridge/HidReportFilter.h") + '"\n'
    code += '''
hidbridge::Descriptor descriptor;
struct Binding { uint8_t volume = 0; };
std::array<Binding, hidbridge::MAX_REPORTS> bindings;
uint8_t volumeState = 0;
'''
    for signature in ("static void onVolume(", "static void updateVolume(", "static void clearVolume("):
        code += extract(signature) + "\n"
    (directory / "test.cpp").write_text(code + CHECKS)
    binary = directory / "test"
    subprocess.run(["g++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I", str(directory), str(directory / "test.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("HTTP volume tests passed: endpoints, bearer header, empty POST, repeats, expiry, Wi-Fi loss and no retries.")
