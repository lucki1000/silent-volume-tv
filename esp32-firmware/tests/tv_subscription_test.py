#!/usr/bin/env python3
"""Run actual sketch TV callbacks/helpers against a fake NimBLE transport.

Checks callback ordering, reconnects, bounded security requests and CCCD recovery.
This does not compile the full firmware or test Android's notification setup.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "BLEHIDBridge/BLEHIDBridge.ino").read_text()


def extract(signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + (";" if signature.startswith(("class ", "struct ")) else "")


stubs = r'''
#include <atomic>
#include <array>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
constexpr uint16_t BLE_HS_CONN_HANDLE_NONE = 0xFFFF;
constexpr uint32_t TV_SECURITY_GRACE_MS = 10000;
struct SerialStub {
    template<class... Args> void printf(const char*, Args...) {}
    void println(const char*) {}
} Serial;
struct ble_addr_t { int value; };
struct ble_store_key_cccd { ble_addr_t peer_addr; uint16_t chr_val_handle; uint8_t idx; };
struct ble_store_value_cccd { uint16_t flags; };
struct Address {
    ble_addr_t address{1};
    std::string toString() const { return "tv"; }
    uint8_t getType() const { return 0; }
    const ble_addr_t* getBase() const { return &address; }
};
namespace hidbridge { void printBondIdentityResolution(const Address&) {} }
struct NimBLEConnInfo {
    uint16_t handle;
    bool slave, encrypted, bonded;
    uint16_t getConnHandle() const { return handle; }
    bool isSlave() const { return slave; }
    bool isEncrypted() const { return encrypted; }
    bool isBonded() const { return bonded; }
    Address getAddress() const { return {}; }
    Address getIdAddress() const { return {}; }
};
struct NimBLECharacteristic {
    uint16_t handle;
    uint16_t getHandle() { return handle; }
    std::vector<uint8_t> getValue() { return {}; }
};
struct NimBLEServer {
    uint16_t mtu = 23, disconnected = BLE_HS_CONN_HANDLE_NONE;
    bool peerEncrypted = true, peerBonded = true;
    uint16_t getPeerMTU(uint16_t) { return mtu; }
    void disconnect(uint16_t handle) { disconnected = handle; }
    NimBLEConnInfo getPeerInfoByHandle(uint16_t handle) { return {handle, true, peerEncrypted, peerBonded}; }
} server;
unsigned securityCalls = 0;
int securityResult = 0;
bool securityAccepted = true;
uint16_t securedHandle = BLE_HS_CONN_HANDLE_NONE;
unsigned advertisingStops = 0;
struct NimBLEAdvertising { void stop() { ++advertisingStops; } } advertising;
struct NimBLEDevice {
    static NimBLEAdvertising* getAdvertising() { return &advertising; }
    static NimBLEServer* getServer() { return &server; }
    static bool isBonded(const Address&) { return true; }
    static bool startSecurity(uint16_t handle, int* rc) {
        ++securityCalls; securedHandle = handle; *rc = securityResult;
        return securityAccepted;
    }
};
struct NimBLEUtils {
    static const char* returnCodeToString(int) { return "test status"; }
};
struct NimBLEServerCallbacks {
    virtual void onConnect(NimBLEServer*, NimBLEConnInfo&) {}
    virtual void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {}
    virtual void onAuthenticationComplete(NimBLEConnInfo&) {}
    virtual void onIdentity(NimBLEConnInfo&) {}
    virtual void onMTUChange(uint16_t, NimBLEConnInfo&) {}
    virtual ~NimBLEServerCallbacks() = default;
};
struct NimBLECharacteristicCallbacks {
    virtual void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t) {}
    virtual void onWrite(NimBLECharacteristic*, NimBLEConnInfo&) {}
    virtual ~NimBLECharacteristicCallbacks() = default;
};
struct Binding {
    NimBLECharacteristic* local = nullptr;
    TvSubscription subscribed;
};
std::array<Binding, hidbridge::MAX_REPORTS> bindings;
hidbridge::Descriptor descriptor;
std::atomic<bool> peripheralStarted{true}, tvEncrypted{false}, tvLost{false}, queueFault{false};
std::atomic<bool> tvBonded{false}, tvRestorePending{false};
std::atomic<uint32_t> tvRestoreAt{0};
std::atomic<uint32_t> tvSecurityAt{0};
std::atomic<bool> tvSecurityPending{false};
uint32_t now = 100;
uint32_t millis() { return now; }
std::atomic<uint16_t> tvHandle{BLE_HS_CONN_HANDLE_NONE};
std::atomic<uint32_t> tvSession{0}, tvSyncMask{0}, sourceSession{0};
enum { HOST_WRITE };
struct ReportEvent {
    int kind; uint8_t index; size_t size; uint32_t sourceSession, tvSession;
    uint8_t data[hidbridge::MAX_REPORT_BYTES];
};
int reportQueue = 0;
constexpr int pdTRUE = 1;
int xQueueSend(int, const ReportEvent*, int) { return pdTRUE; }
uint8_t cccd[3] = {};
int queryResult = 0;
unsigned queryCalls = 0;
bool changeSessionInQuery = false;
int query(uint16_t handle, uint16_t chrHandle, uint8_t* value) {
    assert(handle == tvHandle.load());
    ++queryCalls;
    *value = cccd[chrHandle - 10];
    if (changeSessionInQuery) tvSession.fetch_add(1);
    return queryResult;
}
int (*ble_gatts_read_cccd)(uint16_t, uint16_t, uint8_t*) = query;
std::array<int, 3> savedFlags{{-1, -1, -1}};
unsigned storeCalls = 0;
bool unsubscribeDuringRead = false, changeSessionDuringStoreRead = false;
int ble_store_read_cccd(const ble_store_key_cccd* key, ble_store_value_cccd* value) {
    assert(key->peer_addr.value == 1 && key->idx == 0);
    ++storeCalls;
    const int flags = savedFlags[key->chr_val_handle - 10];
    if (unsubscribeDuringRead) bindings[0].subscribed.store(false);
    if (changeSessionDuringStoreRead) tvSession.fetch_add(1);
    if (flags < 0) return 1;
    value->flags = uint16_t(flags);
    return 0;
}
'''

checks = r'''
int main() {
    const uint8_t map[] = {
        0x05,0x0C,0x09,0x01,0xA1,0x01,0x85,0x03,0x19,0x00,0x2A,0x9C,0x02,
        0x15,0x00,0x26,0x9C,0x02,0x95,0x02,0x75,0x10,0x81,0x00,0xC0,
        0x05,0x0C,0x09,0x01,0xA1,0x01,0x85,0xE9,0x95,0xFF,0x75,0x08,
        0x15,0x00,0x26,0xFF,0x00,0x81,0x00,0xC0,
        0x05,0x0C,0x09,0x01,0xA1,0x01,0x85,0xE8,0x95,0xFF,0x75,0x08,
        0x15,0x00,0x26,0xFF,0x00,0x81,0x00,0xC0
    };
    assert(descriptor.parse(map, sizeof(map)));
    NimBLECharacteristic key{10}, opaque1{11}, opaque2{12};
    bindings[0].local = &key; bindings[1].local = &opaque1; bindings[2].local = &opaque2;
    TvServerCallbacks serverImpl;
    TvReportCallbacks reportImpl;
    NimBLEServerCallbacks& callbacks = serverImpl;
    NimBLECharacteristicCallbacks& reports = reportImpl;
    NimBLEConnInfo tv{7, true, true, true}, remote{8, false, true, true};

    // An early restored subscription is kept when connect arrives afterward.
    reports.onSubscribe(&key, tv, 1);
    const uint32_t firstSession = tvSession.load();
    assert(tvHandle == 7 && bindings[0].subscribed && tvEncrypted);
    callbacks.onConnect(&server, tv);
    assert(advertisingStops == 1); // No advertising while the sole TV is connected.
    assert(tvSession == firstSession && bindings[0].subscribed && tvReady(0));
    assert(tvKeysReady()); // Large opaque reports do not block navigation.
    // Stale unencrypted connect/subscription snapshots cannot undo encryption.
    NimBLEConnInfo earlyTv{7, true, false, true};
    callbacks.onConnect(&server, earlyTv);
    reports.onSubscribe(&key, earlyTv, 1);
    assert(tvEncrypted && tvReady(0));
    maintainTvSecurity();
    assert(securityCalls == 0); // No request on an already encrypted link.

    // Central/source callbacks never become or replace a TV connection.
    callbacks.onConnect(&server, remote);
    assert(advertisingStops == 2); // The central RC link cannot change TV advertising.
    callbacks.onAuthenticationComplete(remote);
    reports.onSubscribe(&key, remote, 0);
    callbacks.onDisconnect(&server, remote, 0);
    assert(tvHandle == 7 && tvSession == firstSession && tvReady(0));
    assert(server.disconnected == BLE_HS_CONN_HANDLE_NONE);

    // A second peripheral host is rejected; active TV state stays intact.
    NimBLEConnInfo other{9, true, true, true};
    callbacks.onConnect(&server, other);
    assert(server.disconnected == 9 && tvHandle == 7 && tvReady(0));
    reports.onSubscribe(&key, other, 0);
    assert(bindings[0].subscribed);

    // A true unsubscribe is respected, rather than forcing notifications.
    reports.onSubscribe(&key, tv, 0);
    assert(!tvReady(0));
    assert(!tvKeysReady());
    assert(std::string(tvBlockReason(0)).find("notifications disabled") != std::string::npos);
    cccd[0] = 1;
    refreshTvSubscriptions(); // Recover a missed callback from actual host state.
    assert(tvReady(0) && (tvSyncMask.load() & 1));
    cccd[0] = 0;
    refreshTvSubscriptions();
    assert(!bindings[0].subscribed && !tvReady(0));
    cccd[0] = 2; // Indication enabled alone is not a notification subscription.
    refreshTvSubscriptions();
    assert(!bindings[0].subscribed);
    cccd[0] = cccd[1] = cccd[2] = 1;
    refreshTvSubscriptions();
    assert(tvReady(0) && !tvReady(1) && !tvReady(2)); // Key fits MTU 23.
    assert(std::string(tvBlockReason(1)).find("MTU too small") != std::string::npos);
    server.mtu = 258;
    assert(tvReady(1) && tvReady(2));

    // Failed/unavailable queries leave callback state authoritative.
    cccd[0] = 0; queryResult = 1;
    refreshTvSubscriptions();
    assert(bindings[0].subscribed);
    queryResult = 0; ble_gatts_read_cccd = nullptr;
    const unsigned calls = queryCalls;
    refreshTvSubscriptions();
    assert(queryCalls == calls && bindings[0].subscribed);
    ble_gatts_read_cccd = query;
    tvEncrypted = false;
    refreshTvSubscriptions();
    assert(queryCalls == calls);
    tvEncrypted = true; changeSessionInQuery = true;
    refreshTvSubscriptions();
    assert(bindings[0].subscribed); // Discard reads across connection generations.
    changeSessionInQuery = false;

    // Older host: recover only a saved enable for this bonded, encrypted peer.
    ble_gatts_read_cccd = nullptr;
    tvBonded = true; tvRestorePending = true; tvRestoreAt = now + 500;
    for (auto& binding : bindings) binding.subscribed.reset();
    savedFlags = {{1, 2, -1}};
    refreshTvSubscriptions(); assert(storeCalls == 0 && !tvReady(0));
    now += 500; refreshTvSubscriptions();
    assert(storeCalls == 3 && tvReady(0) && tvKeysReady());
    assert(!bindings[1].subscribed && !bindings[2].subscribed);
    refreshTvSubscriptions(); assert(storeCalls == 3); // One recovery attempt.

    // An explicit unsubscribe wins even over an old enabled bond record.
    reports.onSubscribe(&key, tv, 0);
    tvRestorePending = true; refreshTvSubscriptions();
    assert(!tvReady(0));
    bindings[0].subscribed.reset(); savedFlags[0] = 0;
    tvRestorePending = true; refreshTvSubscriptions(); assert(!tvReady(0));
    savedFlags[0] = -1;
    tvRestorePending = true; refreshTvSubscriptions(); assert(!tvReady(0));
    savedFlags[0] = 1; unsubscribeDuringRead = true;
    tvRestorePending = true; refreshTvSubscriptions(); assert(!tvReady(0));
    unsubscribeDuringRead = false;
    bindings[0].subscribed.reset(); changeSessionDuringStoreRead = true;
    tvRestorePending = true; refreshTvSubscriptions(); assert(!tvReady(0));
    changeSessionDuringStoreRead = false;
    const unsigned callsBeforeSecurity = storeCalls;
    tvBonded = false; tvRestorePending = true; refreshTvSubscriptions();
    assert(storeCalls == callsBeforeSecurity);
    tvBonded = true; server.peerEncrypted = false;
    tvRestorePending = true; refreshTvSubscriptions(); assert(storeCalls == callsBeforeSecurity);
    server.peerEncrypted = true;
    ble_gatts_read_cccd = query;

    callbacks.onDisconnect(&server, tv, 0);
    assert(tvHandle == BLE_HS_CONN_HANDLE_NONE && !tvEncrypted && tvLost);
    assert(!bindings[0].subscribed && !bindings[1].subscribed && !bindings[2].subscribed);
    assert(tvSyncMask == 0 && !tvBonded && !tvRestorePending);
    const unsigned disconnectedCalls = queryCalls;
    refreshTvSubscriptions();
    assert(queryCalls == disconnectedCalls);
    assert(std::string(tvBlockReason(0)) == "TV disconnected");

    // Authentication-first reconnect also retains subscriptions at connect.
    tv.handle = 10;
    callbacks.onAuthenticationComplete(tv);
    reports.onSubscribe(&key, tv, 1);
    const uint32_t secondSession = tvSession.load();
    callbacks.onConnect(&server, tv);
    assert(secondSession > firstSession && tvSession == secondSession && tvReady(0));
    tv.encrypted = false;
    callbacks.onAuthenticationComplete(tv);
    assert(!tvEncrypted && server.disconnected == 10);

    // Give the TV time to restore encryption before requesting security.
    callbacks.onDisconnect(&server, tv, 531);
    tv.handle = 11; tv.bonded = false;
    securityAccepted = false; securityResult = 6;
    callbacks.onConnect(&server, tv);
    assert(securityCalls == 0 && !tvEncrypted);
    const uint32_t connectedTime = now;
    maintainTvSecurity();
    now = connectedTime + TV_SECURITY_GRACE_MS - 1;
    maintainTvSecurity();
    assert(securityCalls == 0);
    ++now;
    maintainTvSecurity();
    assert(securityCalls == 1 && securedHandle == 11 && !tvEncrypted);
    now += TV_SECURITY_GRACE_MS * 10;
    maintainTvSecurity();
    assert(securityCalls == 1); // A failed request does not start endless retries.

    // The manual e command arms one immediate attempt on a stalled link.
    securityAccepted = true; securityResult = 2; // Existing exchange accepted.
    tvSecurityAt = now; tvSecurityPending = true;
    maintainTvSecurity();
    assert(securityCalls == 2 && !tvEncrypted);
    const uint32_t unencryptedSession = tvSession.load();
    callbacks.onIdentity(tv);
    callbacks.onIdentity(remote); // Resolving the source must not affect the TV.
    assert(tvSession == unencryptedSession && tvHandle == 11);

    tv.encrypted = true; tv.bonded = true;
    callbacks.onAuthenticationComplete(tv);
    now += TV_SECURITY_GRACE_MS;
    maintainTvSecurity();
    assert(securityCalls == 2 && tvEncrypted); // Stop as soon as encrypted.
    callbacks.onDisconnect(&server, tv, 531);
    now += TV_SECURITY_GRACE_MS;
    maintainTvSecurity();
    assert(securityCalls == 2); // Never request security on a dead handle.

    // A reused handle gets a fresh grace period and exactly one fallback.
    tv.encrypted = false; tv.bonded = false;
    securityResult = 0;
    callbacks.onConnect(&server, tv);
    assert(securityCalls == 2);
    now += TV_SECURITY_GRACE_MS;
    maintainTvSecurity();
    assert(securityCalls == 3 && securedHandle == 11);
    now += TV_SECURITY_GRACE_MS * 10;
    maintainTvSecurity();
    assert(securityCalls == 3); // Accepted request also must not repeat forever.
    assert(!tvEncrypted && !tvKeysReady());

    callbacks.onDisconnect(&server, tv, 531);
    callbacks.onConnect(&server, tv);
    now += TV_SECURITY_GRACE_MS - 1;
    tv.encrypted = true; tv.bonded = true;
    callbacks.onAuthenticationComplete(tv);
    ++now;
    maintainTvSecurity();
    assert(securityCalls == 3 && tvEncrypted); // TV encryption wins the grace race.
}
'''

stubs = stubs.replace("struct Binding {", extract("struct TvSubscription {") + "\nstruct Binding {")
code = '#include "' + str(root / "BLEHIDBridge/HidReportFilter.h") + '"\n' + stubs
for signature in (
    "static bool tvReady(", "static bool tvKeysReady(", "static void requestSync(",
    "static void printTvIdentity(", "static bool trackTvConnection(", "static void maintainTvSecurity(",
    "static void restoreTvSubscriptions(", "static void refreshTvSubscriptions(",
    "static const char* tvBlockReason(", "class TvServerCallbacks :",
    "class TvReportCallbacks :",
):
    code += "\n" + extract(signature) + "\n"
code += checks
with tempfile.TemporaryDirectory(prefix="esp32-tv-subscription-") as directory:
    cpp = Path(directory) / "test.cpp"
    binary = Path(directory) / "test"
    cpp.write_text(code)
    subprocess.run(["g++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("TV subscription callback and CCCD tests passed.")
