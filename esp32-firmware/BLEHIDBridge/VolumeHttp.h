#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>
#include <cstring>
#include "BridgeConfig.h"

namespace volumehttp {
struct Action {
    uint16_t usage;
    bool repeat;
    uint32_t queuedAt, keySession, wifiSession;
};
static QueueHandle_t actions = nullptr;
static std::atomic<bool> enabled{false}, wifiReady{false};
static std::atomic<uint32_t> wifiSession{0};
static std::atomic<bool> held[2];
static std::atomic<uint32_t> keySession[2];
static uint32_t lastQueuedAt[2] = {}; // Owned by the Arduino loop / onKey().
static std::atomic<uint32_t> succeeded{0}, failed{0}, dropped{0};
static std::atomic<int> lastCode{0};

static bool validConfig() {
    IPAddress address;
    return bridgeconfig::WIFI_SSID[0] && bridgeconfig::API_TOKEN[0] &&
           !strchr(bridgeconfig::API_TOKEN, '\r') && !strchr(bridgeconfig::API_TOKEN, '\n') &&
           address.fromString(bridgeconfig::TV_IP) && bridgeconfig::TV_HTTP_PORT != 0;
}

static void drop(uint16_t usage, const char* reason) {
    dropped.fetch_add(1);
    Serial.printf("HTTP VOL%s skipped: %s\n", usage == 0xE9 ? "+" : "-", reason);
}

// Called only by onVolume() in the Arduino loop. Never waits for network I/O.
static void onKey(uint16_t usage, bool pressed, bool repeat) {
    if (usage != 0xE9 && usage != 0xEA) return;
    const unsigned index = usage == 0xE9 ? 0 : 1;
    if (!pressed) { held[index].store(false); return; }
    const uint32_t now = millis();
    if (!repeat) {
        keySession[index].fetch_add(1);
        held[index].store(true);
    } else if (!held[index].load() || !bridgeconfig::VOLUME_REPEAT_MS ||
               uint32_t(now - lastQueuedAt[index]) < bridgeconfig::VOLUME_REPEAT_MS) {
        return;
    }
    lastQueuedAt[index] = now;
    if (!enabled.load()) { drop(usage, "network action not configured/started"); return; }
    if (!wifiReady.load() || WiFi.status() != WL_CONNECTED) { drop(usage, "Wi-Fi disconnected"); return; }
    const Action action{usage, repeat, now, keySession[index].load(), wifiSession.load()};
    if (xQueueSend(actions, &action, 0) != pdTRUE) drop(usage, "network queue full");
}

// Executed only by the HTTP worker, separate from loop() and BLE callbacks.
static void processAction(const Action& action) {
    const unsigned index = action.usage == 0xE9 ? 0 : 1;
    if (uint32_t(millis() - action.queuedAt) >= bridgeconfig::VOLUME_MAX_AGE_MS) {
        drop(action.usage, "expired queued request"); return;
    }
    if (action.repeat && (!held[index].load() || action.keySession != keySession[index].load())) {
        drop(action.usage, "held key already released/changed"); return;
    }
    if (!wifiReady.load() || WiFi.status() != WL_CONNECTED || action.wifiSession != wifiSession.load()) {
        drop(action.usage, "Wi-Fi disconnected before POST"); return;
    }
    const char* path = action.usage == 0xE9 ? "/volume/up" : "/volume/down";
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(bridgeconfig::HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(bridgeconfig::HTTP_READ_TIMEOUT_MS);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(client, String(bridgeconfig::TV_IP), bridgeconfig::TV_HTTP_PORT, String(path), false)) {
        lastCode.store(0);
        failed.fetch_add(1);
        Serial.printf("HTTP POST %s: could not initialize request\n", path);
        http.end();
        return;
    }
    http.addHeader("Authorization", String("Bearer ") + bridgeconfig::API_TOKEN);
    http.addHeader("Content-Length", "0");
    const uint32_t started = millis();
    const int code = http.POST(String("")); // POST with an empty body, like the supplied curl commands.
    lastCode.store(code);
    if (code >= 200 && code < 300) succeeded.fetch_add(1);
    else failed.fetch_add(1);
    if (code > 0)
        Serial.printf("HTTP POST %s -> %d (%lu ms)%s\n", path, code,
                      static_cast<unsigned long>(uint32_t(millis() - started)),
                      code >= 200 && code < 300 ? "" : " failed; not retried");
    else
        Serial.printf("HTTP POST %s transport error=%d (%lu ms); not retried\n", path, code,
                      static_cast<unsigned long>(uint32_t(millis() - started)));
    // Do not print headers/token or potentially sensitive response bodies.
    http.end();
}

static void maintainWifi(uint32_t& lastAttempt) {
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (wifiReady.exchange(connected) != connected) {
        wifiSession.fetch_add(1);
        if (connected) Serial.printf("Wi-Fi connected: ESP32 IP=%s\n", WiFi.localIP().toString().c_str());
        else {
            xQueueReset(actions); // No queued requests are replayed on recovery.
            Serial.println("Wi-Fi disconnected; BLE remains active.");
        }
    }
    const uint32_t now = millis();
    if (!connected && uint32_t(now - lastAttempt) >= bridgeconfig::WIFI_RETRY_MS) {
        lastAttempt = now;
        Serial.println("Wi-Fi reconnect attempt (nonblocking).");
        WiFi.reconnect();
    }
}

static void worker(void*) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(bridgeconfig::WIFI_SSID, bridgeconfig::WIFI_PASSWORD);
    Serial.println("Wi-Fi connecting for HTTP volume control; BLE remains active.");
    uint32_t lastAttempt = millis();
    for (;;) {
        maintainWifi(lastAttempt);
        Action action{};
        if (xQueueReceive(actions, &action, pdMS_TO_TICKS(200)) == pdTRUE) processAction(action);
    }
}

static void begin() {
    if (!validConfig()) {
        Serial.println("HTTP volume disabled: fill WIFI_SSID, TV_IP and API_TOKEN in BridgeConfig.h.");
        return;
    }
    actions = xQueueCreate(8, sizeof(Action));
    if (!actions) { Serial.println("HTTP volume queue allocation failed; BLE continues."); return; }
    if (xTaskCreate(worker, "volume-http", 6144, nullptr, 1, nullptr) != pdPASS) {
        vQueueDelete(actions); actions = nullptr;
        Serial.println("HTTP volume worker allocation failed; BLE continues."); return;
    }
    enabled.store(true);
}

static void printStatus() {
    Serial.printf("HTTP volume: enabled=%s Wi-Fi=%s queued=%u ok=%lu failed=%lu dropped=%lu last-code=%d\n",
                  enabled.load() ? "yes" : "no", wifiReady.load() ? "connected" : "disconnected",
                  actions ? unsigned(uxQueueMessagesWaiting(actions)) : 0,
                  static_cast<unsigned long>(succeeded.load()), static_cast<unsigned long>(failed.load()),
                  static_cast<unsigned long>(dropped.load()), lastCode.load());
}
} // namespace volumehttp
