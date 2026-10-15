/*
 * ESP32-WROOM-32D BLE HID remote, using NimBLE-Arduino 2.x.
 * Board: ESP32 Dev Module. Serial Monitor: 115200 baud.
 * Send single characters: u d l r o h b. CR/LF are ignored.
 * Pair "ESP32 TV Remote" in Android TV's Remotes & Accessories menu.
 */
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <atomic>

static constexpr char DEVICE_NAME[] = "ESP32 TV Remote";
// Advertise a recognized input-device subtype for TV accessory filters.
// This changes discovery metadata only; reports still use Consumer page 0x0C.
static constexpr uint16_t DEVICE_APPEARANCE = HID_KEYBOARD;
static constexpr uint8_t REPORT_ID = 1;
static constexpr uint32_t PRESS_MS = 80;
static constexpr uint32_t GAP_MS = 80;

// Consumer page (0x0C), one 16-bit array entry containing a usage ID.
// A value of zero means no key is pressed. HOME/BACK require 16 bits.
static uint8_t reportMap[] = {
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, REPORT_ID,   //   Report ID (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0x24, 0x02,  //   Logical Maximum (0x0224)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0x24, 0x02,  //   Usage Maximum (0x0224)
    0x75, 0x10,        //   Report Size (16 bits)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data, Array, Absolute)
    0xC0               // End Collection
};

static NimBLECharacteristic* inputReport = nullptr;
// BLE callbacks run on another task; use atomics for shared state.
static std::atomic<uint16_t> connectionHandle{BLE_HS_CONN_HANDLE_NONE};
static std::atomic<bool> encrypted{false};
static std::atomic<bool> subscribed{false};

static bool isReady() {
    return connectionHandle.load() != BLE_HS_CONN_HANDLE_NONE &&
           encrypted.load() && subscribed.load();
}

static void printStatus() {
    Serial.printf("Advertising=%s, connected=%s, encrypted=%s, notifications=%s, ready=%s\n",
                  NimBLEDevice::getAdvertising()->isAdvertising() ? "yes" : "no",
                  connectionHandle.load() != BLE_HS_CONN_HANDLE_NONE ? "yes" : "no",
                  encrypted.load() ? "yes" : "no",
                  subscribed.load() ? "yes" : "no",
                  isReady() ? "yes" : "no");
}

static void restartAdvertising() {
    if (connectionHandle.load() != BLE_HS_CONN_HANDLE_NONE) {
        Serial.println("Already connected. Disconnect the other host before pairing the TV.");
        printStatus();
        return;
    }
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (!advertising->stop() || !advertising->start(0)) {
        Serial.println("Advertising restart failed; reset the ESP32.");
    }
    printStatus();
}

class RemoteServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
        // This test remote serves one TV at a time.
        if (connectionHandle.load() != BLE_HS_CONN_HANDLE_NONE) {
            server->disconnect(info.getConnHandle());
            return;
        }
        encrypted.store(info.isEncrypted());
        subscribed.store(false);
        connectionHandle.store(info.getConnHandle());
        Serial.printf("Host %s connected; waiting for encryption and HID subscription.\n",
                      info.getAddress().toString().c_str());
        if (!info.isEncrypted() && !NimBLEDevice::startSecurity(info.getConnHandle())) {
            Serial.println("Could not initiate security; the TV may initiate pairing.");
        }
    }

    void onDisconnect(NimBLEServer* server, NimBLEConnInfo& info, int reason) override {
        if (info.getConnHandle() != connectionHandle.load()) {
            return;
        }
        connectionHandle.store(BLE_HS_CONN_HANDLE_NONE);
        encrypted.store(false);
        subscribed.store(false);
        Serial.printf("Disconnected (reason %d); advertising again.\n", reason);
        if (!server->startAdvertising()) {
            Serial.println("Advertising restart failed; reset the ESP32.");
        }
    }

    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        if (info.getConnHandle() != connectionHandle.load()) {
            return;
        }
        encrypted.store(info.isEncrypted());
        Serial.printf("Security complete: encrypted=%s, bonded=%s\n",
                      info.isEncrypted() ? "yes" : "no",
                      info.isBonded() ? "yes" : "no");
        if (!info.isEncrypted()) {
            NimBLEDevice::getServer()->disconnect(info.getConnHandle());
            return;
        }
        printStatus();
    }
};

class InputCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& info,
                     uint16_t value) override {
        if (info.getConnHandle() == connectionHandle.load()) {
            subscribed.store((value & 0x0001) != 0); // Notifications, not indications.
            encrypted.store(info.isEncrypted());
            printStatus();
        }
    }
};

static RemoteServerCallbacks serverCallbacks;
static InputCallbacks inputCallbacks;

static bool sendReport(uint16_t usage, uint16_t handle) {
    // BLE HID's Report Reference descriptor identifies report 1.
    // Do NOT prepend the Report ID to the characteristic payload.
    const uint8_t report[2] = {
        static_cast<uint8_t>(usage & 0xFF),
        static_cast<uint8_t>(usage >> 8)
    };
    inputReport->setValue(report, sizeof(report));
    if (!isReady() || connectionHandle.load() != handle) {
        return false;
    }
    return inputReport->notify(report, sizeof(report), handle);
}

static void tapKey(uint16_t usage, const char* name) {
    if (!isReady()) {
        Serial.println("Not ready. Pair the TV and wait for ready=yes; use s for status.");
        return;
    }
    const uint16_t handle = connectionHandle.load();
    const bool pressed = sendReport(usage, handle);
    delay(PRESS_MS);
    // Always restore the stored report to zero, even if the link drops.
    const bool released = sendReport(0, handle);
    Serial.printf("%s (0x%04X): press=%s, release=%s\n", name,
                  static_cast<unsigned int>(usage),
                  pressed ? "queued" : "failed", released ? "queued" : "failed");
    if (pressed && !released && isReady()) {
        Serial.println("Release failed; send x to retry releasing all keys.");
    }
    delay(GAP_MS); // Separate repeated taps, including repeated OK presses.
}

static void printHelp() {
    Serial.println("Commands: u=UP d=DOWN l=LEFT r=RIGHT o=OK h=HOME b=BACK");
    Serial.println("          s=status a=restart advertising ?=help x=release all keys");
    Serial.println("Send single characters. Uppercase also works; CR/LF are ignored.");
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\nESP32 Android TV Consumer Control remote (NimBLE-Arduino 2.x)");

    NimBLEDevice::init(DEVICE_NAME);
    Serial.printf("BLE address: %s\n", NimBLEDevice::getAddress().toString().c_str());
    // Bonding persists across resets. Just Works pairing requires no PIN.
    NimBLEDevice::setSecurityAuth(true, false, true); // Bonding, MITM, Secure Connections.
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(&serverCallbacks, false); // Static callback: do not delete.
    server->advertiseOnDisconnect(false); // Restart explicitly in onDisconnect.

    NimBLEHIDDevice* hid = new NimBLEHIDDevice(server);
    hid->setManufacturer("ESP32 DIY");
    // Prototype USB-source VID/PID placeholders; replace for a shipped product.
    hid->setPnp(0x02, 0x1234, 0x0001, 0x0100);
    hid->setHidInfo(0x00, 0x02); // Country: none; normally connectable.
    hid->setReportMap(reportMap, sizeof(reportMap));
    hid->setBatteryLevel(100); // USB-powered test device; fixed battery percentage.

    inputReport = hid->getInputReport(REPORT_ID);
    inputReport->setCallbacks(&inputCallbacks);
    const uint8_t released[2] = {0, 0};
    inputReport->setValue(released, sizeof(released));

    // Older 2.x needs startServices(); newer 2.x retains it as a no-op
    // (possibly with a deprecation warning) and starts services with the server.
    hid->startServices();
    server->start(); // Returns void in older 2.x; bool in newer releases.
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    // Keep all discovery fields in the primary packet, including the name.
    // 3 bytes flags + 4 bytes HID UUID + 4 bytes appearance + 17 bytes name = 28.
    NimBLEAdvertisementData advertisement;
    bool configured = advertisement.setFlags(0x06); // General discoverable, BLE only.
    configured = advertisement.addServiceUUID(NimBLEUUID(uint16_t(0x1812))) && configured;
    configured = advertisement.setAppearance(DEVICE_APPEARANCE) && configured;
    configured = advertisement.setName(DEVICE_NAME) && configured;
    configured = advertising->setConnectableMode(BLE_GAP_CONN_MODE_UND) && configured;
    configured = advertising->setDiscoverableMode(BLE_GAP_DISC_MODE_GEN) && configured;
    advertising->setScanFilter(false, false);
    advertising->setMinInterval(0x20); // 20 ms; interval units are 0.625 ms.
    advertising->setMaxInterval(0x40); // 40 ms.
    advertising->enableScanResponse(false);
    configured = advertising->setAdvertisementData(advertisement) && configured;
    const auto payload = advertisement.getPayload();
    Serial.printf("Advertising payload (%u bytes):", static_cast<unsigned int>(payload.size()));
    for (uint8_t value : payload) {
        Serial.printf(" %02X", static_cast<unsigned int>(value));
    }
    Serial.println();
    if (!configured) {
        Serial.println("Advertising configuration failed; check the NimBLE version and logs.");
    } else if (!advertising->start(0)) { // Advertise indefinitely.
        Serial.println("Advertising start failed; reset the ESP32.");
    } else {
        Serial.printf("Advertising as \"%s\". Pair from the TV.\n", DEVICE_NAME);
    }
    printHelp();
    printStatus();
}

void loop() {
    while (Serial.available() > 0) {
        char command = static_cast<char>(Serial.read());
        if (command >= 'A' && command <= 'Z') {
            command += 'a' - 'A';
        }
        switch (command) {
            case 'u': tapKey(0x0042, "UP");    break;
            case 'd': tapKey(0x0043, "DOWN");  break;
            case 'l': tapKey(0x0044, "LEFT");  break;
            case 'r': tapKey(0x0045, "RIGHT"); break;
            case 'o': tapKey(0x0041, "OK");    break;
            case 'h': tapKey(0x0223, "HOME");  break;
            case 'b': tapKey(0x0224, "BACK");  break;
            case 's': printStatus(); break;
            case 'a': restartAdvertising(); break;
            case '?': printHelp(); break;
            case 'x':
                Serial.println(sendReport(0, connectionHandle.load()) ?
                               "Release queued." : "Release failed; check status with s.");
                break;
            case '\r': case '\n': case ' ': case '\t': break;
            default: Serial.println("Unknown command; send ? for help."); break;
        }
    }
    delay(5);
}
