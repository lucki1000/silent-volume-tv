#!/usr/bin/env python3
"""Exercise connection policy and report discovery with a fake BLE transport.

This does not compile the complete ESP32 firmware or test radio/security APIs.
Run: python3 tests/bridge_connection_test.py
"""
from pathlib import Path
import subprocess
import tempfile


source = (Path(__file__).resolve().parents[1] / "BLEHIDBridge/BLEHIDBridge.ino").read_text()


def function(signature):
    start = source.index(signature)
    body = source.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


stubs = r'''
#include <atomic>
#include <cassert>
#include <cstring>
#include <deque>
#include <string>
#include <strings.h>
#include <vector>
#include <array>
#include <cstdio>
struct Candidate { char address[18]; char name[64]; uint8_t addressType; bool hid; };
char SOURCE_ADDRESS[64] = "AA:BB:CC:DD:EE:FF", SOURCE_NAME[64] = "";
const char DEVICE_NAME[] = "ESP32 TV Remote";
uint8_t SOURCE_ADDRESS_TYPE = 0xFF;
const uint32_t RETRY_MS = 2500, SCAN_MS = 4000, TV_NOT_READY_GRACE_MS = 1500;
struct SerialStub {
    void println(const char*) {}
    template<class... Args> void printf(const char*, Args...) {}
} Serial;
void printHex(const char*, uint8_t, const uint8_t*, size_t) {}
struct NimBLEUUID {
    uint16_t value;
    explicit NimBLEUUID(uint16_t value) : value(value) {}
    bool operator!=(const NimBLEUUID& other) const { return value != other.value; }
};
struct Reference {
    std::vector<uint8_t> value;
    std::vector<uint8_t> readValue() { return value; }
};
struct NimBLERemoteCharacteristic {
    uint16_t handle, uuid = 0x2A4D;
    bool notify = true, hasReference = true;
    Reference reference;
    NimBLERemoteCharacteristic(uint16_t handle, std::vector<uint8_t> value)
        : handle(handle), reference{value} {}
    NimBLEUUID getUUID() { return NimBLEUUID(uuid); }
    uint16_t getHandle() { return handle; }
    bool canNotify() { return notify; }
    Reference* getDescriptor(NimBLEUUID) { return hasReference ? &reference : nullptr; }
};
struct NimBLERemoteService {
    std::vector<NimBLERemoteCharacteristic*> characteristics;
    const std::vector<NimBLERemoteCharacteristic*>& getCharacteristics(bool) { return characteristics; }
};
struct Binding { NimBLERemoteCharacteristic* source = nullptr; };
std::array<Binding, hidbridge::MAX_REPORTS> bindings;
hidbridge::Descriptor descriptor;
struct NimBLEAddress {
    std::string value; uint8_t type;
    NimBLEAddress(const std::string& value, uint8_t type) : value(value), type(type) {}
};
struct Scan {
    bool scanning = false; unsigned starts = 0;
    bool isScanning() { return scanning; }
    void stop() { scanning = false; }
    bool start(uint32_t, bool, bool) { ++starts; scanning = true; return true; }
} scan;
struct NimBLEDevice { static Scan* getScan() { return &scan; } };
struct Client {
    bool connected = false;
    bool isConnected() { return connected; }
    bool disconnect();
} client;
Client* sourceClient = &client;
std::atomic<bool> sourceConnected{false}, scanEnded{false}, verbose{false};
std::atomic<bool> sourceReady{false};
bool Client::disconnect() { connected = false; sourceConnected = false; return true; }
std::atomic<uint32_t> sourceSession{0};
std::string selectedAddress, selectedName;
uint8_t selectedAddressType = 0;
bool publishPending = false, directTried = false;
bool sourcePaused = false, sourceGateClosed = false, fakeTvKeysReady = false;
bool sourceReleaseRequested = false;
uint32_t sourceUnavailableSince = 0;
std::vector<uint8_t> reportMap;
bool tvKeysReady() { return fakeTvKeysReady; }
uint32_t now = 100, nextAttempt = 0;
uint32_t millis() { return now; }
void* candidateQueue = nullptr;
std::deque<Candidate> candidates;
const int pdTRUE = 1;
int xQueueReceive(void*, Candidate* out, int) {
    if (candidates.empty()) return 0;
    *out = candidates.front(); candidates.pop_front(); return pdTRUE;
}
void xQueueReset(void*) { candidates.clear(); }
bool startPeripheral() { return true; }
std::vector<NimBLEAddress> attempts;
// 0: no link; 1: ready; 2: link established, security/profile failed and disconnected.
std::deque<int> outcomes;
bool connectSource(const NimBLEAddress& address, const std::string&) {
    attempts.push_back(address);
    int result = 0;
    if (!outcomes.empty()) { result = outcomes.front(); outcomes.pop_front(); }
    if (result) ++sourceSession;
    if (result == 1) { client.connected = true; sourceConnected = true; return true; }
    return false;
}
void reset() {
    strcpy(SOURCE_ADDRESS, "AA:BB:CC:DD:EE:FF"); SOURCE_NAME[0] = 0;
    SOURCE_ADDRESS_TYPE = 0xFF;
    selectedAddress.clear(); selectedName.clear(); selectedAddressType = 0;
    publishPending = directTried = false; nextAttempt = 0; now = 100;
    sourceConnected = scanEnded = false; sourceSession = 0;
    sourceReady = false; sourcePaused = sourceGateClosed = fakeTvKeysReady = false;
    sourceReleaseRequested = false;
    sourceUnavailableSince = 0; reportMap.clear();
    client.connected = false; scan = Scan{};
    candidates.clear(); attempts.clear(); outcomes.clear();
}
'''

checks = r'''
int main() {
    reset(); assert(validSourceAddress());
    strcpy(SOURCE_ADDRESS, "aA:bB:01:02:03:fF"); assert(validSourceAddress());
    strcpy(SOURCE_ADDRESS, "AA:BB:..."); assert(!validSourceAddress());
    strcpy(SOURCE_ADDRESS, "AA:BB:CC:DD:EE:GG"); assert(!validSourceAddress());
    SOURCE_ADDRESS[0] = 0; assert(validSourceAddress());

    // Fresh profile: connect the configured MAC without any scan result.
    reset(); outcomes = {1}; maintainSource();
    assert(attempts.size() == 1 && attempts[0].value == SOURCE_ADDRESS);
    assert(attempts[0].type == 0 && scan.starts == 0);

    // Unknown type: try random after public times out, before scanning.
    reset(); outcomes = {0, 1}; maintainSource();
    assert(attempts.size() == 2 && attempts[0].type == 0 && attempts[1].type == 1);
    assert(scan.starts == 0);

    // Explicit random address uses that type alone.
    reset(); SOURCE_ADDRESS_TYPE = 1; outcomes = {0}; maintainSource();
    assert(attempts.size() == 1 && attempts[0].type == 1 && scan.starts == 1);

    // A security/profile failure is not an address-type failure.
    reset(); outcomes = {2}; maintainSource();
    assert(attempts.size() == 1 && scan.starts == 1);

    // Cached bonding identity and its type take priority on reboot/reconnect.
    reset(); selectedAddress = "C0:11:22:33:44:55"; selectedAddressType = 1;
    outcomes = {1}; maintainSource();
    assert(attempts.size() == 1 && attempts[0].value == selectedAddress && attempts[0].type == 1);

    // Empty MAC retains automatic HID discovery.
    reset(); SOURCE_ADDRESS[0] = 0; maintainSource();
    assert(attempts.empty() && scan.starts == 1);

    // Exhausted direct attempts scan, then retry after the scan/backoff.
    reset(); maintainSource(); assert(attempts.size() == 2 && scan.starts == 1);
    maintainSource(); assert(attempts.size() == 2);
    scan.stop(); scanEnded = true; maintainSource(); assert(attempts.size() == 2);
    now += RETRY_MS; outcomes = {1}; maintainSource(); assert(attempts.size() == 3);

    // A matching MAC in a scan is accepted even with no advertised HID UUID.
    reset(); Candidate c{}; strcpy(c.address, SOURCE_ADDRESS); c.addressType = 1;
    assert(accepts(c)); candidates.push_back(c); outcomes = {1}; maintainSource();
    assert(attempts.size() == 1 && attempts[0].type == 1);

    // Saved-name fallback can locate a rotating address without a HID UUID.
    reset(); selectedAddress = "C0:11:22:33:44:55"; selectedName = "My Remote";
    strcpy(c.address, "C1:11:22:33:44:55"); strcpy(c.name, "My Remote");
    assert(accepts(c)); strcpy(c.name, "Unrelated"); assert(!accepts(c));

    // Saved profile: TV must enable key notifications BEFORE any RC attempt.
    reset(); reportMap = {1}; candidates.push_back(c); scan.scanning = true;
    outcomes = {1}; maintainSource();
    assert(attempts.empty() && !scan.scanning && candidates.empty() && sourceGateClosed);
    now += 60000; maintainSource(); assert(attempts.empty() && scan.starts == 0);
    fakeTvKeysReady = true; maintainSource();
    assert(attempts.size() == 1 && client.connected && !sourceGateClosed);

    // Brief TV readiness loss does not churn the source link. Sustained loss
    // releases it and suppresses all reconnects so it remains available to TV.
    fakeTvKeysReady = false; maintainSource(); assert(client.connected);
    now += 1000; fakeTvKeysReady = true; maintainSource();
    assert(client.connected && attempts.size() == 1 && !sourceGateClosed);
    fakeTvKeysReady = false; maintainSource(); assert(client.connected);
    now += TV_NOT_READY_GRACE_MS - 1; maintainSource(); assert(client.connected);
    now += 1; maintainSource(); assert(!client.connected && !sourceReady);
    now += RETRY_MS * 2; maintainSource(); assert(attempts.size() == 1);
    fakeTvKeysReady = true; outcomes = {1}; maintainSource();
    assert(client.connected && attempts.size() == 2);

    // Manual pause releases immediately, and only an explicit resume reclaims RC.
    sourcePaused = true; maintainSource(); assert(!client.connected);
    maintainSource(); assert(attempts.size() == 2);
    sourcePaused = false; outcomes = {1}; maintainSource();
    assert(client.connected && attempts.size() == 3);

    // After first profile learning publishes the services, wait for TV too.
    reset(); reportMap = {1}; publishPending = true; maintainSource();
    assert(!publishPending && attempts.empty());
    maintainSource(); assert(attempts.empty() && sourceGateClosed);
    fakeTvKeysReady = true; outcomes = {1}; maintainSource(); assert(attempts.size() == 1);

    // An extra reference encountered BEFORE the mapped reports must not abort
    // discovery. Bind by (ID,type), preserving the opaque input mappings too.
    hidbridge::Report keys, opaqueUp, opaqueDown;
    keys.id = 3; opaqueUp.id = 0xE9; opaqueDown.id = 0xE8;
    descriptor.reports = {keys, opaqueUp, opaqueDown};
    NimBLERemoteCharacteristic extra(0x10, {1,3}), keyInput(0x20, {3,1}),
        opaqueInputUp(0x30, {0xE9,1}), opaqueInputDown(0x40, {0xE8,1});
    NimBLERemoteService service{{&extra, &keyInput, &opaqueInputUp, &opaqueInputDown}};
    assert(discoverSourceReports(&service));
    assert(bindings[0].source == &keyInput && bindings[1].source == &opaqueInputUp);
    assert(bindings[2].source == &opaqueInputDown);

    // Missing inputs, including opaque ones, and duplicate mapped references
    // must still fail; ignoring extra references cannot produce a partial bridge.
    service.characteristics.pop_back(); assert(!discoverSourceReports(&service));
    service.characteristics.push_back(&opaqueInputDown);
    NimBLERemoteCharacteristic duplicate(0x50, {3,1});
    service.characteristics.push_back(&duplicate); assert(!discoverSourceReports(&service));
    service.characteristics.pop_back();
    opaqueInputDown.notify = false; assert(!discoverSourceReports(&service));
    opaqueInputDown.notify = true;
    keyInput.reference.value = {3}; assert(!discoverSourceReports(&service));
    keyInput.reference.value = {3,1};
    keyInput.hasReference = false; assert(!discoverSourceReports(&service));
    keyInput.hasReference = true; assert(discoverSourceReports(&service));
    puts("Bridge connection policy tests passed.");
}
'''

with tempfile.TemporaryDirectory(prefix="hid-bridge-connection-") as directory:
    cpp = Path(directory) / "connection.cpp"
    binary = Path(directory) / "connection-test"
    header = Path(__file__).resolve().parents[1] / "BLEHIDBridge/HidReportFilter.h"
    cpp.write_text(f'#include "{header}"\n' + stubs + "\n" + "\n".join(function(s) for s in (
        "static bool validSourceAddress() {",
        "static bool accepts(const Candidate& candidate) {",
        "static void maintainSource() {",
        "static bool discoverSourceReports(NimBLERemoteService* service) {",
    )) + "\n" + checks)
    subprocess.run(["g++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
