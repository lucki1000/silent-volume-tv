/*
 * ESP32-WROOM-32D simultaneous BLE HID central + peripheral bridge.
 * Tested with ESP32 core 3.3.12 and patched NimBLE-Arduino 2.5.1.
 * See ../BUILD.md for the tested board target and build settings.
 * Keep the supplied headers in this directory. Serial: 115200 baud.
 * Configure Wi-Fi / HTTP volume control in BridgeConfig.h.
 * Saved profile: pair/reconnect the TV first, then the remote is claimed.
 * Fresh profile: temporarily learn the remote map before advertising the TV.
 */
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <NimBLEUtils.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
#include <array>
#include <string>
#include <vector>
#include <cstring>
#include <strings.h>
#include "HidReportFilter.h"
#include "VolumeHttp.h"
#if defined(CONFIG_NIMBLE_CPP_IDF)
#include "host/ble_store.h"
#else
#include "nimble/nimble/host/include/host/ble_store.h"
#endif

#include "BondIdentityCache.h"

// Newer bundled NimBLE hosts expose a live, per-connection CCCD query.
// Older 2.x releases fall back to onSubscribe() without a link dependency.
extern "C" int ble_gatts_read_cccd(uint16_t conn_handle, uint16_t chr_val_handle,
                                  uint8_t* cccd_value) __attribute__((weak));

#if defined(CONFIG_BT_NIMBLE_MAX_CONNECTIONS) && CONFIG_BT_NIMBLE_MAX_CONNECTIONS < 2
#error "NimBLE needs at least two connections for the remote and the TV"
#endif

// A configured MAC is connected DIRECTLY, even without an advertised HID UUID.
// Empty address = discovery by SOURCE_NAME, or by advertised HID service.
static constexpr const char* SOURCE_NAME = bridgeconfig::SOURCE_NAME;
static constexpr const char* SOURCE_ADDRESS = bridgeconfig::SOURCE_ADDRESS;
// 0 = public, 1 = random, 0xFF = try public then random if connection fails.
// Set 0 or 1 if your working example already specifies the address type.
static constexpr uint8_t SOURCE_ADDRESS_TYPE = bridgeconfig::SOURCE_ADDRESS_TYPE;
static_assert(SOURCE_ADDRESS_TYPE == 0 || SOURCE_ADDRESS_TYPE == 1 || SOURCE_ADDRESS_TYPE == 0xFF,
              "SOURCE_ADDRESS_TYPE must be 0, 1, or 0xFF");
static constexpr char DEVICE_NAME[] = "ESP32 TV Remote";
static constexpr size_t QUEUE_DEPTH = 32; // ~17 KiB with 512-byte payloads.
static constexpr uint32_t SCAN_MS = 4000;
static constexpr uint32_t RETRY_MS = 2500;
static constexpr size_t MAX_MAP_BYTES = 512;
static constexpr uint32_t TV_NOT_READY_GRACE_MS = 1500;
static constexpr uint32_t TV_SECURITY_GRACE_MS = 10000;

// 0=unknown, 2=known disabled, 3=known enabled. One atomic state prevents a
// saved-CCCD fallback from racing with and overriding an explicit unsubscribe.
struct TvSubscription {
    std::atomic<uint8_t> state{0};
    bool load() const { return (state.load() & 1) != 0; }
    explicit operator bool() const { return load(); }
    void store(bool enabled) { state.store(enabled ? 3 : 2); }
    bool exchange(bool enabled) { return (state.exchange(enabled ? 3 : 2) & 1) != 0; }
    void reset() { state.store(0); }
    bool known() const { return state.load() != 0; }
    bool restoreEnabled() {
        uint8_t expected = 0;
        return state.compare_exchange_strong(expected, uint8_t(3));
    }
};
struct Binding {
    NimBLECharacteristic* local = nullptr;
    NimBLERemoteCharacteristic* source = nullptr; // Used only from loop().
    TvSubscription subscribed;
    uint8_t volume = 0; // This report's intercepted key state.
};
enum EventKind : uint8_t { INPUT_EVENT, HOST_WRITE, CONTROL_WRITE };
struct ReportEvent {
    EventKind kind;
    uint8_t index;
    uint16_t size;
    uint32_t sourceSession, tvSession;
    bool forwardToTv;
    uint8_t data[hidbridge::MAX_REPORT_BYTES];
};
struct Candidate {
    char address[18];
    char name[64];
    uint8_t addressType;
    bool hid;
};
static bool accepts(const Candidate& candidate);

static hidbridge::Descriptor descriptor;
static std::vector<uint8_t> reportMap;
// Mirror PnP identifiers when available, so Android can retain vendor-specific
// key layouts. These prototype values are used only if the remote omits PnP.
static uint8_t sourcePnp[7] = {0x02, 0x34, 0x12, 0x01, 0x00, 0x00, 0x01};
static uint8_t sourceCountry = 0, sourceHidFlags = 0x02;
static std::array<Binding, hidbridge::MAX_REPORTS> bindings;
static Preferences preferences;
static NimBLEClient* sourceClient = nullptr;
static NimBLEHIDDevice* tvHid = nullptr;
static QueueHandle_t reportQueue = nullptr, candidateQueue = nullptr;
static std::atomic<bool> sourceConnected{false}, sourceReady{false};
static std::atomic<bool> sourceLost{false}, queueFault{false};
static std::atomic<bool> peripheralStarted{false}, verbose{true};
static std::atomic<uint32_t> sourceSession{0}, tvSession{0};
static std::atomic<uint16_t> tvHandle{BLE_HS_CONN_HANDLE_NONE};
static std::atomic<bool> tvEncrypted{false}, tvLost{false}, scanEnded{false};
static std::atomic<bool> tvBonded{false}, tvRestorePending{false};
static std::atomic<uint32_t> tvRestoreAt{0};
static std::atomic<uint32_t> tvSecurityAt{0};
static std::atomic<bool> tvSecurityPending{false};
static std::atomic<uint32_t> tvSyncMask{0};
static std::string selectedAddress, selectedName;
static uint8_t selectedAddressType = 0;
static bool identityPinned = false, publishPending = false, directTried = false;
static bool advertisingConfigured = false;
static uint32_t nextAttempt = 0;
static uint8_t volumeState = 0;
static bool sourcePaused = false, sourceGateClosed = false;
static uint32_t sourceUnavailableSince = 0;
static bool sourceReleaseRequested = false;

static bool validSourceAddress() {
    if (!SOURCE_ADDRESS[0]) return true;
    if (strlen(SOURCE_ADDRESS) != 17) return false;
    for (size_t i = 0; i < 17; ++i) {
        const char c = SOURCE_ADDRESS[i];
        if (i % 3 == 2) { if (c != ':') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static bool tvReady(size_t index) {
    return peripheralStarted.load() && tvHandle.load() != BLE_HS_CONN_HANDLE_NONE &&
           tvEncrypted.load() && bindings[index].subscribed.load() &&
           NimBLEDevice::getServer()->getPeerMTU(tvHandle.load()) >= descriptor.reports[index].bytes() + 3;
}

static bool tvKeysReady() {
    bool hasKeys = false, hasInputs = false;
    for (size_t i = 0; i < descriptor.reports.size(); ++i)
        if (descriptor.reports[i].type == 1 && !descriptor.reports[i].fields.empty()) {
            hasKeys = true;
            if (!tvReady(i)) return false;
        }
    if (hasKeys) return true; // Opaque RC860G reports need a larger MTU, not navigation.
    for (size_t i = 0; i < descriptor.reports.size(); ++i)
        if (descriptor.reports[i].type == 1) {
            hasInputs = true;
            if (!tvReady(i)) return false;
        }
    return hasInputs;
}

static void printHex(const char* label, uint8_t id, const uint8_t* data, size_t size) {
    if (!verbose.load()) return;
    Serial.printf("%s id=%u [%u]:", label, unsigned(id), unsigned(size));
    for (size_t i = 0; i < size; ++i) Serial.printf(" %02X", unsigned(data[i]));
    Serial.println();
}

// Intercepted volume queues an HTTP action. No volume usage reaches the TV.
// pressed=true, repeat=false: down edge; repeat=true: another held-key report;
// pressed=false: release edge. Avoid blocking here to preserve forwarding.
static void onVolume(uint16_t usage, bool pressed, bool repeat) {
    Serial.printf("INTERCEPT %s %s (Consumer 0x%04X)\n",
                  usage == 0xE9 ? "VOL+" : "VOL-",
                  !pressed ? "release" : repeat ? "repeat" : "press", unsigned(usage));
    volumehttp::onKey(usage, pressed, repeat);
}

static void updateVolume(size_t index, uint8_t mask) {
    bindings[index].volume = mask;
    uint8_t combined = 0;
    for (size_t i = 0; i < descriptor.reports.size(); ++i) combined |= bindings[i].volume;
    for (uint8_t bit = 1; bit <= 2; bit <<= 1) {
        const bool before = (volumeState & bit) != 0, after = (combined & bit) != 0;
        if (before != after || (after && (mask & bit)))
            onVolume(bit == 1 ? 0xE9 : 0xEA, after, before && after);
    }
    volumeState = combined;
}

static void clearVolume() {
    if (volumeState & 1) onVolume(0xE9, false, false);
    if (volumeState & 2) onVolume(0xEA, false, false);
    volumeState = 0;
    for (auto& binding : bindings) binding.volume = 0;
}

static void requestSync() {
    uint32_t mask = 0;
    for (size_t i = 0; i < descriptor.reports.size(); ++i)
        if (descriptor.reports[i].type == 1 && bindings[i].subscribed.load()) mask |= uint32_t(1) << i;
    tvSyncMask.fetch_or(mask);
}

static void printTvIdentity(const char* event, NimBLEConnInfo& info) {
    const auto address = info.getAddress();
    const auto identity = info.getIdAddress();
    Serial.printf("TV identity (%s): over-air=%s type=%u identity=%s type=%u stored-bond=%s encrypted=%s bonded=%s\n",
                  event, address.toString().c_str(), unsigned(address.getType()),
                  identity.toString().c_str(), unsigned(identity.getType()),
                  NimBLEDevice::isBonded(identity) ? "yes" : "no",
                  info.isEncrypted() ? "yes" : "no", info.isBonded() ? "yes" : "no");
    // An unresolved private address can initially miss the bond lookup. The
    // identity callback below logs again after NimBLE resolves the peer's IRK.
    hidbridge::printBondIdentityResolution(address);
}

static bool trackTvConnection(NimBLEConnInfo& info) {
    // The ESP32 is the peripheral on the TV link, central on the RC link.
    if (!info.isSlave() || info.getConnHandle() == BLE_HS_CONN_HANDLE_NONE) return false;
    const uint16_t current = tvHandle.load();
    if (current != BLE_HS_CONN_HANDLE_NONE && current != info.getConnHandle()) return false;
    if (current == BLE_HS_CONN_HANDLE_NONE) {
        for (auto& binding : bindings) binding.subscribed.reset();
        tvSyncMask.store(0);
        tvEncrypted.store(info.isEncrypted());
        tvBonded.store(info.isBonded());
        tvRestoreAt.store(millis() + 500);
        tvSecurityAt.store(millis() + TV_SECURITY_GRACE_MS);
        tvSecurityPending.store(true);
        tvRestorePending.store(info.isEncrypted() && info.isBonded());
        tvSession.fetch_add(1);
        tvHandle.store(info.getConnHandle());
        Serial.printf("TV host connected: %s handle=%u\n", info.getAddress().toString().c_str(),
                      unsigned(info.getConnHandle()));
        printTvIdentity("connected", info);
    } else if (info.isEncrypted()) {
        tvEncrypted.store(true);
        if (info.isBonded()) tvBonded.store(true);
    }
    // Subscribe/security callbacks can precede the application's onConnect.
    // A later callback for this same link must preserve its subscriptions.
    return true;
}

static void maintainTvSecurity() {
    const uint16_t handle = tvHandle.load();
    if (handle == BLE_HS_CONN_HANDLE_NONE || tvEncrypted.load() ||
        !tvSecurityPending.load() || int32_t(millis() - tvSecurityAt.load()) < 0) return;
    const uint32_t session = tvSession.load();
    // Let the TV restore encryption first. Claim one fallback attempt before
    // calling NimBLE, which can trigger callbacks. Serial e permits a retry.
    if (!tvSecurityPending.exchange(false)) return;
    int rc = 0;
    const bool started = NimBLEDevice::startSecurity(handle, &rc);
    if (session != tvSession.load() || handle != tvHandle.load() || tvEncrypted.load()) return;
    Serial.printf("TV security request (one attempt): %s rc=%d (%s); waiting for TV encryption; e=manual retry.\n",
                  started ? "accepted/already in progress" : "failed",
                  rc, NimBLEUtils::returnCodeToString(rc));
    // In the peripheral role this sends an SMP Security Request; the TV must
    // initiate encryption. EALREADY leaves an existing exchange untouched.
    // Do not keep sending security requests on a stalled/unanswered TV link.
}

static void restoreTvSubscriptions() {
    // This is saved bond state, not a live query. Use it once after encryption
    // only when no subscription event for that report has arrived on this link.
    if (ble_gatts_read_cccd || !tvBonded.load() || !tvEncrypted.load() ||
        !tvRestorePending.load() || int32_t(millis() - tvRestoreAt.load()) < 0) return;
    if (!tvRestorePending.exchange(false)) return;
    const uint16_t handle = tvHandle.load();
    const uint32_t session = tvSession.load();
    if (handle == BLE_HS_CONN_HANDLE_NONE) return;
    const auto info = NimBLEDevice::getServer()->getPeerInfoByHandle(handle);
    if (info.getConnHandle() != handle || !info.isSlave() || !info.isEncrypted() || !info.isBonded()) return;
    const auto identity = info.getIdAddress();
    for (size_t i = 0; i < descriptor.reports.size(); ++i) {
        if (descriptor.reports[i].type != 1 || !bindings[i].local || bindings[i].subscribed.known()) continue;
        ble_store_key_cccd key{};
        ble_store_value_cccd value{};
        key.peer_addr = *identity.getBase();
        key.chr_val_handle = bindings[i].local->getHandle();
        const int rc = ble_store_read_cccd(&key, &value);
        if (handle != tvHandle.load() || session != tvSession.load()) return;
        if (rc == 0 && (value.flags & 1) && bindings[i].subscribed.restoreEnabled()) {
            tvSyncMask.fetch_or(uint32_t(1) << i);
            Serial.printf("TV subscription recovered id=%u notifications=yes (saved bond CCCD; no live query)\n",
                          unsigned(descriptor.reports[i].id));
        } else if (!bindings[i].subscribed.known()) {
            Serial.printf("TV saved CCCD id=%u: %s (rc=%d); waiting for TV notification enable.\n",
                          unsigned(descriptor.reports[i].id), rc == 0 ? "notifications disabled" : "not readable", rc);
        }
    }
}

static void refreshTvSubscriptions() {
    restoreTvSubscriptions();
    if (!ble_gatts_read_cccd || !peripheralStarted.load() || !tvEncrypted.load()) return;
    const uint16_t handle = tvHandle.load();
    const uint32_t session = tvSession.load();
    if (handle == BLE_HS_CONN_HANDLE_NONE) return;
    for (size_t i = 0; i < descriptor.reports.size(); ++i) {
        if (descriptor.reports[i].type != 1 || !bindings[i].local) continue;
        uint8_t value = 0;
        const int rc = ble_gatts_read_cccd(handle, bindings[i].local->getHandle(), &value);
        if (session != tvSession.load() || handle != tvHandle.load()) return;
        if (rc != 0) continue; // Never infer a subscription from a failed read.
        const bool enabled = (value & 1) != 0;
        if (bindings[i].subscribed.exchange(enabled) != enabled) {
            if (enabled) tvSyncMask.fetch_or(uint32_t(1) << i);
            Serial.printf("TV subscription sync id=%u notifications=%s (live CCCD=0x%02X)\n",
                          unsigned(descriptor.reports[i].id), enabled ? "yes" : "no", unsigned(value));
        }
    }
}

static const char* tvBlockReason(size_t index) {
    const uint16_t handle = tvHandle.load();
    if (!peripheralStarted.load() || handle == BLE_HS_CONN_HANDLE_NONE) return "TV disconnected";
    if (!tvEncrypted.load()) return "TV encryption pending";
    if (!bindings[index].subscribed.load()) return "TV connected, input notifications disabled";
    if (NimBLEDevice::getServer()->getPeerMTU(handle) < descriptor.reports[index].bytes() + 3)
        return "TV connected, MTU too small for this report";
    return "TV ready";
}

static void printBondStatus() {
#if defined(MYNEWT_VAL_BLE_STORE_CONFIG_PERSIST)
    Serial.printf("NimBLE bond persistence: %s\n", MYNEWT_VAL_BLE_STORE_CONFIG_PERSIST ? "enabled (NVS flash)" : "disabled");
#elif defined(CONFIG_BT_NIMBLE_NVS_PERSIST)
    Serial.printf("NimBLE bond persistence config: %s\n", CONFIG_BT_NIMBLE_NVS_PERSIST ? "enabled" : "disabled");
#else
    Serial.println("NimBLE bond persistence config: not exposed; compare stored peers before/after power cycle.");
#endif
    const int count = NimBLEDevice::getNumBonds();
    Serial.printf("Stored BLE bonds=%d; saved HID profile=%s\n", count, reportMap.empty() ? "no" : "yes");
    for (int i = 0; i < count; ++i) {
        const auto address = NimBLEDevice::getBondedAddress(i);
        const bool remote = !selectedAddress.empty() && address == NimBLEAddress(selectedAddress, selectedAddressType);
        Serial.printf("  Bond[%d] identity=%s type=%u (%s)\n", i, address.toString().c_str(),
                      unsigned(address.getType()), remote ? "physical remote" : "other peer; compare with TV security identity");
        ble_store_key_sec key{};
        ble_store_value_sec ours{}, peer{};
        key.peer_addr = *address.getBase();
        const int ourRc = ble_store_read_our_sec(&key, &ours);
        const int peerRc = ble_store_read_peer_sec(&key, &peer);
        Serial.printf("    Security metadata: local-record rc=%d LTK=%s; peer-record rc=%d LTK=%s IRK=%s\n",
                      ourRc, ourRc == 0 && ours.ltk_present ? "yes" : "no",
                      peerRc, peerRc == 0 && peer.ltk_present ? "yes" : "no",
                      peerRc == 0 && peer.irk_present ? "yes" : "no");
    }
    // Identity addresses only. Never print the stored encryption/identity keys.
}

static void printStatus() {
    volumehttp::printStatus();
    Serial.printf("Remote connection policy: TV first; manual pause=%s; TV keys ready=%s\n",
                  sourcePaused ? "yes" : "no", tvKeysReady() ? "yes" : "no");
    unsigned inputCount = 0, readyCount = 0, keyCount = 0, keyReadyCount = 0;
    for (size_t i = 0; i < descriptor.reports.size(); ++i)
        if (descriptor.reports[i].type == 1) {
            ++inputCount;
            const bool ready = tvReady(i);
            if (ready) ++readyCount;
            // RC860G report 03 has declared key fields; E9/E8 are opaque data.
            if (!descriptor.reports[i].fields.empty()) {
                ++keyCount; if (ready) ++keyReadyCount;
            }
        }
    const bool tvInputReady = keyCount ? keyReadyCount == keyCount : inputCount && readyCount == inputCount;
    Serial.printf("Remote: connected=%s ready=%s; TV: connected=%s encrypted=%s ready=%s; advertising=%s scanning=%s queued=%u bonds=%d\n",
                  sourceConnected.load() ? "yes" : "no", sourceReady.load() ? "yes" : "no",
                  tvHandle.load() != BLE_HS_CONN_HANDLE_NONE ? "yes" : "no",
                  tvEncrypted.load() ? "yes" : "no", tvInputReady ? "yes" : "no",
                  peripheralStarted.load() && NimBLEDevice::getAdvertising()->isAdvertising() ? "yes" : "no",
                  NimBLEDevice::getScan()->isScanning() ? "yes" : "no",
                  unsigned(uxQueueMessagesWaiting(reportQueue)), NimBLEDevice::getNumBonds());
    Serial.printf("Selected remote: %s (type %u), name=\"%s\"; reports=%u\n",
                  selectedAddress.empty() ? "not learned" : selectedAddress.c_str(),
                  unsigned(selectedAddressType), selectedName.c_str(), unsigned(descriptor.reports.size()));
    Serial.printf("Configured remote MAC: %s; address type=%s\n",
                  SOURCE_ADDRESS[0] ? SOURCE_ADDRESS : "none (scan discovery)",
                  SOURCE_ADDRESS_TYPE == 0xFF ? "auto (public then random)" :
                  SOURCE_ADDRESS_TYPE == 0 ? "public (0)" : "random (1)");
    if (!peripheralStarted.load())
        Serial.println("TV advertising waits for the remote HID map. Connect the remote first.");
    Serial.printf("TV key reports ready=%u/%u; all inputs ready=%u/%u, MTU=%u\n",
                  keyReadyCount, keyCount, readyCount, inputCount,
                  tvHandle.load() == BLE_HS_CONN_HANDLE_NONE ? 0 : unsigned(NimBLEDevice::getServer()->getPeerMTU(tvHandle.load())));
    if (tvHandle.load() != BLE_HS_CONN_HANDLE_NONE) {
        const uint16_t mtu = NimBLEDevice::getServer()->getPeerMTU(tvHandle.load());
        for (size_t i = 0; i < descriptor.reports.size(); ++i)
            if (descriptor.reports[i].type == 1)
                Serial.printf("TV input id=%u handle=0x%04X notifications=%s: %s\n",
                              unsigned(descriptor.reports[i].id), unsigned(bindings[i].local->getHandle()),
                              bindings[i].subscribed.load() ? "yes" : "no", tvBlockReason(i));
        if (keyReadyCount < keyCount && tvEncrypted.load())
            Serial.println("If key notifications stay disabled, reconnect the ESP32 accessory on the TV; if needed forget/re-pair it on the TV.");
        for (const auto& report : descriptor.reports)
            if (report.type == 1 && mtu < report.bytes() + 3)
                Serial.printf("TV input id=%u needs MTU >= %u; payload cannot currently be forwarded intact.\n",
                              unsigned(report.id), unsigned(report.bytes() + 3));
    }
}

static void abortTv(const char* reason) {
    Serial.printf("TV link reset: %s. Reconnect to clear held keys.\n", reason);
    const uint16_t handle = tvHandle.load();
    if (handle != BLE_HS_CONN_HANDLE_NONE) NimBLEDevice::getServer()->disconnect(handle);
}

static void sendNeutral(size_t index) {
    if (!peripheralStarted.load() || descriptor.reports[index].type != 1) return;
    uint8_t data[hidbridge::MAX_REPORT_BYTES];
    if (!descriptor.neutral(index, data)) return; // Unknown payloads have no defined key release.
    const size_t size = descriptor.reports[index].bytes();
    bindings[index].local->setValue(data, size);
    const uint16_t handle = tvHandle.load();
    if (handle != BLE_HS_CONN_HANDLE_NONE && tvReady(index)) {
        if (!bindings[index].local->notify(data, size, handle)) abortTv("release notification failed");
        else printHex("TX neutral", descriptor.reports[index].id, data, size);
    }
}

static void releaseAll() {
    clearVolume();
    for (size_t i = 0; i < descriptor.reports.size(); ++i) sendNeutral(i);
}

class TvServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
        if (!info.isSlave()) return;
        if (!trackTvConnection(info)) {
            server->disconnect(info.getConnHandle()); return;
        }
        // Only one TV is accepted. Keeping advertising active can also make
        // NimBLE 2.5.1 reject replacement-bond cleanup with BLE_HS_EBUSY.
        NimBLEDevice::getAdvertising()->stop();
        maintainTvSecurity();
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
        if (!info.isSlave() || info.getConnHandle() != tvHandle.load()) return;
        Serial.printf("TV disconnected, reason=%d (0x%X, %s); encrypted=%s bonded=%s. Remote will be released if TV readiness stays lost.\n",
                      reason, unsigned(reason), NimBLEUtils::returnCodeToString(reason),
                      tvEncrypted.load() ? "yes" : "no", tvBonded.load() ? "yes" : "no");
        tvHandle.store(BLE_HS_CONN_HANDLE_NONE);
        tvSecurityPending.store(false);
        tvEncrypted.store(false);
        tvBonded.store(false); tvRestorePending.store(false);
        tvSession.fetch_add(1);
        tvLost.store(true);
        tvSyncMask.store(0);
        for (auto& binding : bindings) binding.subscribed.reset();
        // Advertising restarts from loop(), independent of the central link.
    }
    void onIdentity(NimBLEConnInfo& info) override {
        if (trackTvConnection(info)) printTvIdentity("resolved", info);
    }
    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        if (!trackTvConnection(info)) return;
        tvEncrypted.store(info.isEncrypted());
        if (info.isEncrypted()) tvSecurityPending.store(false);
        tvBonded.store(info.isBonded());
        tvRestoreAt.store(millis() + 500);
        tvRestorePending.store(info.isEncrypted() && info.isBonded());
        Serial.printf("TV security: encrypted=%s bonded=%s identity=%s type=%u\n",
                      info.isEncrypted() ? "yes" : "no", info.isBonded() ? "yes" : "no",
                      info.getIdAddress().toString().c_str(), unsigned(info.getIdAddress().getType()));
        if (!info.isEncrypted()) NimBLEDevice::getServer()->disconnect(info.getConnHandle());
        else requestSync();
    }
    void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override {
        if (trackTvConnection(info)) {
            Serial.printf("TV MTU=%u\n", unsigned(mtu));
            requestSync();
        }
    }
};

class TvReportCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* characteristic, NimBLEConnInfo& info, uint16_t value) override {
        if (!trackTvConnection(info)) return;
        for (size_t i = 0; i < descriptor.reports.size(); ++i) {
            if (bindings[i].local != characteristic || descriptor.reports[i].type != 1) continue;
            bindings[i].subscribed.store((value & 1) != 0);
            if (value & 1) tvSyncMask.fetch_or(uint32_t(1) << i);
            Serial.printf("TV input subscription id=%u notifications=%s\n",
                          unsigned(descriptor.reports[i].id), (value & 1) ? "yes" : "no");
        }
    }
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
        if (info.getConnHandle() != tvHandle.load()) return;
        for (size_t i = 0; i < descriptor.reports.size(); ++i) {
            if (bindings[i].local != characteristic || descriptor.reports[i].type == 1) continue;
            const auto value = characteristic->getValue();
            if (value.size() > hidbridge::MAX_REPORT_BYTES) { queueFault.store(true); return; }
            ReportEvent event{};
            event.kind = HOST_WRITE; event.index = uint8_t(i); event.size = value.size();
            event.sourceSession = sourceSession.load(); event.tvSession = tvSession.load();
            memcpy(event.data, value.data(), value.size());
            if (xQueueSend(reportQueue, &event, 0) != pdTRUE) queueFault.store(true);
            return;
        }
    }
};

class TvControlCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
        if (info.getConnHandle() != tvHandle.load()) return;
        const auto value = characteristic->getValue();
        if (value.size() != 1) return;
        ReportEvent event{};
        event.kind = CONTROL_WRITE; event.size = 1; event.data[0] = value[0];
        event.sourceSession = sourceSession.load(); event.tvSession = tvSession.load();
        if (xQueueSend(reportQueue, &event, 0) != pdTRUE) queueFault.store(true);
    }
};

class SourceCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient*) override {
        sourceSession.fetch_add(1); sourceConnected.store(true); sourceReady.store(false);
        Serial.println("Remote connected; discovering and securing HID.");
    }
    void onDisconnect(NimBLEClient*, int reason) override {
        sourceReady.store(false); sourceConnected.store(false);
        sourceSession.fetch_add(1); sourceLost.store(true);
        Serial.printf("Remote disconnected, reason=%d. TV link stays active.\n", reason);
    }
    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        Serial.printf("Remote security: encrypted=%s bonded=%s identity=%s\n",
                      info.isEncrypted() ? "yes" : "no", info.isBonded() ? "yes" : "no",
                      info.getIdAddress().toString().c_str());
    }
};

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* device) override {
        if (!device->isConnectable()) return;
        const bool hid = device->isAdvertisingService(NimBLEUUID(uint16_t(0x1812)));
        const std::string name = device->getName(), address = device->getAddress().toString();
        // Selection happens in loop(), including a saved name/identity. Do not
        // hide remotes just because their advertisements omit service 0x1812.
        Candidate candidate{};
        snprintf(candidate.address, sizeof(candidate.address), "%s", address.c_str());
        snprintf(candidate.name, sizeof(candidate.name), "%s", name.c_str());
        candidate.addressType = device->getAddress().getType(); candidate.hid = hid;
        xQueueSend(candidateQueue, &candidate, 0); // Dropping scan candidates is harmless; scans repeat.
    }
    void onScanEnd(const NimBLEScanResults&, int) override { scanEnded.store(true); }
};

static TvServerCallbacks tvServerCallbacks;
static TvReportCallbacks tvReportCallbacks;
static TvControlCallbacks tvControlCallbacks;
static SourceCallbacks sourceCallbacks;
static ScanCallbacks scanCallbacks;

static bool startPeripheral() {
    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(&tvServerCallbacks, false);
    server->advertiseOnDisconnect(false);
    tvHid = new NimBLEHIDDevice(server);
    tvHid->setManufacturer("ESP32 HID Bridge");
    tvHid->setPnp(sourcePnp[0], uint16_t(sourcePnp[1]) | (uint16_t(sourcePnp[2]) << 8),
                 uint16_t(sourcePnp[3]) | (uint16_t(sourcePnp[4]) << 8),
                 uint16_t(sourcePnp[5]) | (uint16_t(sourcePnp[6]) << 8));
    tvHid->setHidInfo(sourceCountry, sourceHidFlags);
    tvHid->setBatteryLevel(100); // USB-powered bridge.
    tvHid->setReportMap(reportMap.data(), uint16_t(reportMap.size()));
    for (size_t i = 0; i < descriptor.reports.size(); ++i) {
        const auto& report = descriptor.reports[i];
        auto& binding = bindings[i];
        binding.local = report.type == 1 ? tvHid->getInputReport(report.id) :
                        report.type == 2 ? tvHid->getOutputReport(report.id) : tvHid->getFeatureReport(report.id);
        binding.local->setCallbacks(&tvReportCallbacks);
        uint8_t data[hidbridge::MAX_REPORT_BYTES] = {};
        if (report.type != 1 || descriptor.neutral(i, data))
            binding.local->setValue(data, report.bytes());
        Serial.printf("Cloned report id=%u type=%u bytes=%u synthetic-release=%s\n",
                      unsigned(report.id), unsigned(report.type), unsigned(report.bytes()),
                      report.type == 1 && report.canNeutralize ? "yes" : "no");
    }
    tvHid->getHidControl()->setCallbacks(&tvControlCallbacks);
    tvHid->startServices(); // Required in older 2.x; harmless no-op in newer 2.x.
    server->start();
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData data;
    bool ok = data.setFlags(0x06);
    ok = data.addServiceUUID(NimBLEUUID(uint16_t(0x1812))) && ok;
    ok = data.setAppearance(HID_KEYBOARD) && ok; // Retain the working PEAQ discovery hint.
    ok = data.setName(DEVICE_NAME) && ok;
    ok = advertising->setConnectableMode(BLE_GAP_CONN_MODE_UND) && ok;
    ok = advertising->setDiscoverableMode(BLE_GAP_DISC_MODE_GEN) && ok;
    advertising->setScanFilter(false, false);
    advertising->setMinInterval(0x20); advertising->setMaxInterval(0x40);
    advertising->enableScanResponse(false);
    ok = advertising->setAdvertisementData(data) && ok;
    advertisingConfigured = ok;
    peripheralStarted.store(true);
    if (!ok || !advertising->start(0)) {
        Serial.println("TV advertising failed. Check NimBLE debug logs."); return false;
    }
    Serial.printf("Advertising \"%s\" with the remote's exact report map. Reconnect the saved TV accessory, or pair on first setup.\n", DEVICE_NAME);
    return true;
}

static void enqueueInput(size_t index, uint32_t session, const uint8_t* data, size_t size) {
    if (session != sourceSession.load()) return;
    if (size > hidbridge::MAX_REPORT_BYTES) { queueFault.store(true); return; }
    ReportEvent event{};
    event.kind = INPUT_EVENT; event.index = uint8_t(index); event.size = uint16_t(size);
    event.sourceSession = session; event.tvSession = tvSession.load();
    event.forwardToTv = tvReady(index);
    memcpy(event.data, data, size);
    if (xQueueSend(reportQueue, &event, 0) != pdTRUE) queueFault.store(true);
}

static void saveIdentity(const std::string& name, const NimBLEAddress& address) {
    selectedAddress = address.toString(); selectedAddressType = address.getType();
    if (!name.empty()) selectedName = name;
    identityPinned = true;
    preferences.putString("address", selectedAddress.c_str());
    preferences.putUChar("addrtype", selectedAddressType);
    preferences.putString("name", selectedName.c_str());
}

static bool discoverSourceReports(NimBLERemoteService* service) {
    for (auto& binding : bindings) binding.source = nullptr;
    bool duplicate = false;
    for (auto* characteristic : service->getCharacteristics(true)) {
        if (characteristic->getUUID() != NimBLEUUID(uint16_t(0x2A4D))) continue;
        auto* reference = characteristic->getDescriptor(NimBLEUUID(uint16_t(0x2908)));
        if (!reference) {
            Serial.printf("Remote report handle=0x%04X has no Report Reference; skipped.\n",
                          unsigned(characteristic->getHandle()));
            continue;
        }
        const auto value = reference->readValue();
        if (value.size() != 2) {
            Serial.printf("Remote Report Reference handle=0x%04X length=%u (expected 2); skipped.\n",
                          unsigned(characteristic->getHandle()), unsigned(value.size()));
            printHex("Invalid Report Reference", 0, value.data(), value.size());
            continue;
        }
        Serial.printf("Remote Report Reference id=%u (0x%02X) type=%u handle=0x%04X notify=%s\n",
                      unsigned(value[0]), unsigned(value[0]), unsigned(value[1]),
                      unsigned(characteristic->getHandle()), characteristic->canNotify() ? "yes" : "no");
        const size_t index = descriptor.find(value[0], value[1]);
        if (index == descriptor.reports.size()) {
            // Extra GATT reports may be absent from the readable Report Map.
            // They cannot be mirrored safely; continue to discover mapped keys.
            Serial.println("  Not present in the learned Report Map; skipping this extra characteristic.");
            continue;
        }
        if (bindings[index].source) {
            Serial.printf("  Duplicate mapped reference: handles 0x%04X and 0x%04X.\n",
                          unsigned(bindings[index].source->getHandle()), unsigned(characteristic->getHandle()));
            duplicate = true;
            continue; // Finish the inventory so diagnostics show every reference.
        }
        bindings[index].source = characteristic;
    }
    bool complete = !duplicate;
    for (size_t i = 0; i < descriptor.reports.size(); ++i) {
        if (descriptor.reports[i].type == 1 &&
            (!bindings[i].source || !bindings[i].source->canNotify())) {
            Serial.printf("Missing notifying mapped input id=%u (0x%02X).\n",
                          unsigned(descriptor.reports[i].id), unsigned(descriptor.reports[i].id));
            complete = false;
        }
    }
    return complete;
}

static bool connectSource(const NimBLEAddress& address, const std::string& name) {
    Serial.printf("Connecting remote %s (type %u)...\n", address.toString().c_str(), unsigned(address.getType()));
    // Re-discover attributes each connection: never reuse deleted remote pointers.
    if (!sourceClient->connect(address, true)) {
        Serial.printf("Remote connection failed: %d\n", sourceClient->getLastError()); return false;
    }
    const uint32_t session = sourceSession.load();
    const bool secured = sourceClient->secureConnection();
    const auto securedInfo = sourceClient->getConnInfo();
    if (!secured || !securedInfo.isEncrypted()) {
        Serial.println("Remote encryption failed. Use the remote's pairing mode; check stale bonds on both sides.");
        sourceClient->disconnect(); return false;
    }
    const NimBLEAddress verifiedIdentity = securedInfo.getIdAddress();
    if (identityPinned && verifiedIdentity !=
                          NimBLEAddress(selectedAddress, selectedAddressType)) {
        Serial.println("Candidate is not the learned remote identity; ignoring it.");
        sourceClient->disconnect(); return false;
    }
    NimBLERemoteService* service = sourceClient->getService(NimBLEUUID(uint16_t(0x1812)));
    if (!service) { Serial.println("Remote has no HID service."); sourceClient->disconnect(); return false; }
    auto* mapCharacteristic = service->getCharacteristic(NimBLEUUID(uint16_t(0x2A4B)));
    if (!mapCharacteristic) { Serial.println("Remote has no HID Report Map."); sourceClient->disconnect(); return false; }
    const auto map = mapCharacteristic->readValue();
    if (map.size() == 0 || map.size() > MAX_MAP_BYTES) {
        Serial.println("Cannot read remote HID Report Map (limit 512 bytes)."); sourceClient->disconnect(); return false;
    }
    if (reportMap.empty()) {
        // Keep the raw descriptor in diagnostics even when parsing fails.
        const bool previous = verbose.exchange(true);
        printHex("Remote Report Map", 0, map.data(), map.size());
        verbose.store(previous);
        if (!descriptor.parse(map.data(), map.size())) {
            Serial.printf("Unsupported report map: %s\n", descriptor.error.c_str());
            sourceClient->disconnect(); return false;
        }
        if (auto* information = service->getCharacteristic(NimBLEUUID(uint16_t(0x2A4A)))) {
            const auto value = information->readValue();
            if (value.size() == 4) {
                sourceCountry = value[2]; sourceHidFlags = uint8_t((value[3] & 1) | 2);
                preferences.putUChar("country", sourceCountry);
                preferences.putUChar("hidflags", sourceHidFlags);
            }
        }
        if (auto* information = sourceClient->getService(NimBLEUUID(uint16_t(0x180A)))) {
            if (auto* pnp = information->getCharacteristic(NimBLEUUID(uint16_t(0x2A50)))) {
                const auto value = pnp->readValue();
                if (value.size() == sizeof(sourcePnp)) {
                    memcpy(sourcePnp, value.data(), sizeof(sourcePnp));
                    preferences.putBytes("pnp", sourcePnp, sizeof(sourcePnp));
                    printHex("Remote PnP", 0, sourcePnp, sizeof(sourcePnp));
                }
            }
        }
        if (!sourceClient->isConnected() || session != sourceSession.load()) return false;
        reportMap.assign(map.data(), map.data() + map.size());
        saveIdentity(name, verifiedIdentity);
        if (preferences.putBytes("map", reportMap.data(), reportMap.size()) != reportMap.size())
            Serial.println("Warning: report map was not saved; it will be learned again after reboot.");
        printHex("Learned Report Map", 0, reportMap.data(), reportMap.size());
        // Register the local GATT server without an active link. On the first
        // boot we briefly disconnect the learned remote, then reconnect it
        // after publishing the cloned services. Subsequent boots use NVS.
        publishPending = true;
        sourceClient->disconnect();
        return false;
    }
    if (map.size() != reportMap.size() || memcmp(map.data(), reportMap.data(), map.size()) != 0) {
        Serial.println("Remote report map differs from the saved TV profile. No reports forwarded.");
        Serial.println("Forget the ESP32 accessory on the TV, then send p to reset the bridge profile.");
        sourceClient->disconnect(); return false;
    }

    if (!discoverSourceReports(service)) {
        Serial.println("Mapped input discovery incomplete or ambiguous; see the full reference inventory above.");
        sourceClient->disconnect(); return false;
    }
    // HOGP report mode: notifications contain payload only, no leading Report ID.
    if (auto* protocol = service->getCharacteristic(NimBLEUUID(uint16_t(0x2A4E)))) {
        const uint8_t reportMode = 1;
        if (!protocol->writeValue(&reportMode, 1, !protocol->canWriteNoResponse())) {
            Serial.println("Could not select remote Report Protocol mode."); sourceClient->disconnect(); return false;
        }
    }
    // Populate cached output/feature values for TV reads without blocking a
    // NimBLE callback on a synchronous client GATT read.
    for (size_t i = 0; i < descriptor.reports.size(); ++i) {
        if (descriptor.reports[i].type != 1 && bindings[i].source && bindings[i].source->canRead()) {
            const auto value = bindings[i].source->readValue();
            if (value.size() == descriptor.reports[i].bytes()) bindings[i].local->setValue(value.data(), value.size());
        }
    }
    for (size_t i = 0; i < descriptor.reports.size(); ++i) {
        if (descriptor.reports[i].type != 1) continue;
        const auto callback = [i, session](NimBLERemoteCharacteristic*, uint8_t* data, size_t size, bool) {
            enqueueInput(i, session, data, size);
        };
        if (!bindings[i].source->subscribe(true, callback, true)) {
            Serial.printf("Remote subscription failed id=%u.\n", unsigned(descriptor.reports[i].id));
            sourceClient->disconnect(); return false;
        }
    }
    if (!sourceClient->isConnected() || session != sourceSession.load()) return false;
    saveIdentity(name, verifiedIdentity);
    sourceReady.store(true);
    Serial.println("REMOTE READY. Non-volume key reports are forwarded while the TV is ready.");
    printStatus();
    return true;
}

static bool accepts(const Candidate& candidate) {
    if (!selectedAddress.empty()) {
        // Names allow locating a rotating OTA address; encryption must then
        // resolve to the saved identity in connectSource() before forwarding.
        return strcasecmp(selectedAddress.c_str(), candidate.address) == 0 ||
               (SOURCE_ADDRESS[0] && strcasecmp(SOURCE_ADDRESS, candidate.address) == 0) ||
               (!selectedName.empty() && selectedName == candidate.name);
    }
    if (SOURCE_ADDRESS[0]) return strcasecmp(SOURCE_ADDRESS, candidate.address) == 0;
    if (SOURCE_NAME[0]) return strcmp(SOURCE_NAME, candidate.name) == 0;
    return candidate.hid && strcmp(candidate.name, DEVICE_NAME) != 0;
}

static void maintainSource() {
    if (publishPending) {
        if (!sourceClient->isConnected()) {
            publishPending = false;
            startPeripheral();
            nextAttempt = millis(); directTried = false;
        }
        return;
    }
    // With a saved map, advertise immediately and leave the physical remote
    // available until the TV has encrypted and enabled its key notifications.
    // A fresh profile still needs one temporary connection to learn its map.
    if (sourcePaused || (!reportMap.empty() && !tvKeysReady())) {
        if (!sourceGateClosed) {
            sourceGateClosed = true; sourceUnavailableSince = millis();
            NimBLEDevice::getScan()->stop(); xQueueReset(candidateQueue);
            scanEnded.store(false); directTried = false;
            Serial.println(sourcePaused ? "Remote paused by Serial; TV advertising remains available." :
                           "Waiting for TV key notifications before claiming the physical remote.");
        }
        if (!sourceClient->isConnected()) sourceReleaseRequested = false;
        if (sourceClient->isConnected() && !sourceReleaseRequested &&
            (sourcePaused || uint32_t(millis() - sourceUnavailableSince) >= TV_NOT_READY_GRACE_MS)) {
            sourceReady.store(false);
            Serial.println("Releasing physical remote while TV input is unavailable; remote bond is kept.");
            sourceReleaseRequested = sourceClient->disconnect();
        }
        return;
    }
    if (sourceGateClosed) {
        sourceGateClosed = false;
        scanEnded.store(false); nextAttempt = millis(); directTried = false;
        Serial.println("TV key reports ready; physical remote connection is now allowed.");
    }
    if (!sourceClient->isConnected()) sourceReleaseRequested = false;
    if (sourceConnected.load() || sourceClient->isConnected()) return;
    if (scanEnded.exchange(false)) {
        nextAttempt = millis() + RETRY_MS; directTried = false;
        if (verbose.load()) Serial.println("Remote scan ended; retrying shortly.");
    }
    Candidate candidate{};
    while (xQueueReceive(candidateQueue, &candidate, 0) == pdTRUE) {
        if (verbose.load()) Serial.printf("SCAN %s type=%u name=\"%s\" HID=%s\n",
                                         candidate.address, unsigned(candidate.addressType), candidate.name,
                                         candidate.hid ? "yes" : "no");
        if (!accepts(candidate)) continue;
        NimBLEDevice::getScan()->stop();
        xQueueReset(candidateQueue);
        scanEnded.store(false);
        connectSource(NimBLEAddress(std::string(candidate.address), candidate.addressType), candidate.name);
        nextAttempt = millis() + RETRY_MS; directTried = false;
        return;
    }
    if (NimBLEDevice::getScan()->isScanning() || int32_t(millis() - nextAttempt) < 0) return;
    if ((!selectedAddress.empty() || SOURCE_ADDRESS[0]) && !directTried) {
        directTried = true;
        // First boot uses the configured MAC; subsequent boots prefer the
        // authenticated identity saved during bonding. No scan match is needed.
        const std::string address = selectedAddress.empty() ? SOURCE_ADDRESS : selectedAddress;
        const uint8_t type = selectedAddress.empty() ? SOURCE_ADDRESS_TYPE : selectedAddressType;
        const std::string name = selectedName.empty() ? SOURCE_NAME : selectedName;
        const uint32_t session = sourceSession.load();
        if (connectSource(NimBLEAddress(address, type == 0xFF ? 0 : type), name) || publishPending) return;
        if (sourceClient->isConnected()) return; // Await asynchronous disconnect before scanning.
        // Retry the other type only when no link was established. A security
        // or descriptor failure means the address type was already correct.
        if (type == 0xFF && sourceSession.load() == session) {
            Serial.println("No public-address connection; trying the same MAC as random (type 1).");
            if (connectSource(NimBLEAddress(address, 1), name) || publishPending) return;
            if (sourceClient->isConnected()) return;
        }
    }
    Serial.println("Scanning for the physical HID remote. Wake it or enable pairing mode.");
    if (!NimBLEDevice::getScan()->start(SCAN_MS, false, true)) {
        Serial.println("Scan start failed; will retry."); nextAttempt = millis() + RETRY_MS;
    }
}

static void processReports() {
    ReportEvent event{};
    // Bound each pass so Serial and connection maintenance remain responsive.
    for (unsigned batch = 0; batch < 16 && xQueueReceive(reportQueue, &event, 0) == pdTRUE; ++batch) {
        if (event.sourceSession != sourceSession.load()) continue; // Discard previous connection's queued data.
        if (event.kind == CONTROL_WRITE) {
            if (!sourceReady.load() || event.tvSession != tvSession.load()) continue;
            auto* service = sourceClient->getService(NimBLEUUID(uint16_t(0x1812)));
            auto* control = service ? service->getCharacteristic(NimBLEUUID(uint16_t(0x2A4C))) : nullptr;
            if (control) control->writeValue(event.data, event.size, !control->canWriteNoResponse());
            continue;
        }
        if (event.index >= descriptor.reports.size()) continue;
        const auto& report = descriptor.reports[event.index];
        auto& binding = bindings[event.index];
        if (event.kind == HOST_WRITE) {
            if (!sourceReady.load() || event.tvSession != tvSession.load() || !binding.source) continue;
            if (event.size != report.bytes()) { Serial.println("TV report write has wrong length; ignored."); continue; }
            if (!binding.source->canWrite() && !binding.source->canWriteNoResponse()) continue;
            if (!binding.source->writeValue(event.data, event.size, !binding.source->canWriteNoResponse()))
                Serial.printf("TV-to-remote report write failed id=%u.\n", unsigned(report.id));
            continue;
        }
        if (!sourceReady.load()) continue;
        uint8_t normalized[hidbridge::MAX_REPORT_BYTES], filtered[hidbridge::MAX_REPORT_BYTES], volume = 0;
        const size_t size = report.bytes();
        printHex("RX", report.id, event.data, event.size);
        if (!descriptor.normalizeInput(event.index, event.data, event.size, normalized) ||
            !descriptor.filter(event.index, normalized, size, filtered, volume)) {
            Serial.printf("Invalid input length id=%u: got=%u expected=%u; cannot safely complete missing selectors. Disconnecting remote.\n",
                          unsigned(report.id), unsigned(event.size), unsigned(report.bytes()));
            sourceReady.store(false);
            sourceClient->disconnect(); releaseAll(); return;
        }
        if (event.size != size && verbose.load())
            Serial.printf("PAD id=%u: %u -> %u bytes; omitted array selectors filled with no-key values.\n",
                          unsigned(report.id), unsigned(event.size), unsigned(size));
        updateVolume(event.index, volume);
        binding.local->setValue(filtered, size);
        const uint16_t handle = tvHandle.load();
        if (!event.forwardToTv || handle == BLE_HS_CONN_HANDLE_NONE ||
            event.tvSession != tvSession.load() || !tvReady(event.index)) {
            if (verbose.load()) {
                const char* reason = tvBlockReason(event.index);
                if (event.tvSession != tvSession.load()) reason = "TV connection changed since RX";
                else if (!event.forwardToTv && tvReady(event.index)) reason = "TV was not ready when this input arrived";
                Serial.printf("DROP id=%u: %s; no offline replay.\n", unsigned(report.id), reason);
            }
            continue;
        }
        // No tap timer, key remapping, or duplicate suppression: every source
        // report, including releases and repeated held-key reports, is sent.
        if (!binding.local->notify(filtered, size, handle)) {
            abortTv("input notification failed"); return;
        }
        printHex("TX", report.id, filtered, size);
    }
}

static bool forgetTvBond() {
    if (tvHandle.load() != BLE_HS_CONN_HANDLE_NONE || selectedAddress.empty()) {
        Serial.println("Disconnect the TV first; a learned remote identity is required to preserve its bond.");
        return false;
    }
    const NimBLEAddress remote(selectedAddress, selectedAddressType);
    if (!NimBLEDevice::isBonded(remote)) {
        Serial.println("Learned remote bond is missing; inspect k. No bonds changed.");
        return false;
    }
    std::vector<NimBLEAddress> candidates;
    const int count = NimBLEDevice::getNumBonds();
    for (int i = 0; i < count; ++i) {
        const auto address = NimBLEDevice::getBondedAddress(i);
        if (address != remote) candidates.push_back(address);
    }
    if (candidates.size() != 1) {
        Serial.println("Expected exactly one saved TV bond; inspect k. No bonds changed.");
        return false;
    }
    // NimBLE 2.5.1 refuses to remove an IRK during advertising/discovery.
    NimBLEDevice::getAdvertising()->stop();
    NimBLEDevice::getScan()->stop();
    const bool removed = NimBLEDevice::deleteBond(candidates.front());
    Serial.printf("Forget saved TV bond %s: %s. Remote bond and learned map retained.\n",
                  candidates.front().toString().c_str(), removed ? "done" : "failed");
    return removed;
}

static void printHelp() {
    Serial.println("Commands: s=status d=toggle report debug a=restart TV advertising");
    Serial.println("          c=resume/reconnect remote after TV ready r=pause/release remote");
    Serial.println("          t=reconnect TV k=stored bond identities x=release all keys m=dump report map ?=help");
    Serial.println("          e=request TV security once (manual retry if encryption is stalled)");
    Serial.println("          f=forget sole saved TV bond and reboot; use only after forgetting ESP on TV");
    Serial.println("          p=clear learned bridge profile and reboot (BLE bonds are kept)");
    Serial.println("For p: first forget the ESP32 accessory on the TV, then put the remote in pairing mode.");
}

// Dispatch only a complete, one-character line. USB TTY echo or boot garbage
// must not turn letters inside diagnostic text into profile-clear commands.
class SerialCommandLine {
    uint8_t length = 0;
    char pending = 0;
public:
    bool feed(char byte, char& command) {
        if (byte == '\r' || byte == '\n') {
            const bool ready = length == 1;
            if (ready) command = pending;
            length = 0;
            return ready;
        }
        if (length == 0) pending = byte;
        if (length < 2) ++length; // Saturate: long/noisy lines remain invalid.
        return false;
    }
};

static void processSerial() {
    static SerialCommandLine line;
    while (Serial.available()) {
        char command;
        if (!line.feed(char(Serial.read()), command)) continue;
        if (command >= 'A' && command <= 'Z') command += 'a' - 'A';
        switch (command) {
            case 's': printStatus(); break;
            case 'k': printBondStatus(); break;
            case 'e':
                if (tvHandle.load() == BLE_HS_CONN_HANDLE_NONE) Serial.println("TV disconnected; wait for the TV to connect.");
                else if (tvEncrypted.load()) Serial.println("TV already encrypted.");
                else {
                    tvSecurityAt.store(millis()); tvSecurityPending.store(true);
                    maintainTvSecurity();
                }
                break;
            case 'd': verbose.store(!verbose.load()); Serial.printf("Report debug=%s\n", verbose.load() ? "on" : "off"); break;
            case 'a':
                if (!peripheralStarted.load()) Serial.println("Learn the remote report map before TV advertising can start.");
                else if (tvHandle.load() != BLE_HS_CONN_HANDLE_NONE) Serial.println("TV is already connected.");
                else if (advertisingConfigured) {
                    NimBLEDevice::getAdvertising()->stop();
                    if (!NimBLEDevice::getAdvertising()->start(0)) Serial.println("TV advertising restart failed.");
                }
                break;
            case 'c':
                sourcePaused = false;
                NimBLEDevice::getScan()->stop(); xQueueReset(candidateQueue); scanEnded.store(false);
                if (sourceClient->isConnected()) sourceClient->disconnect();
                nextAttempt = millis(); directTried = false; break;
            case 'r':
                sourcePaused = true;
                Serial.println("Physical remote paused. Send c to resume after the TV is ready.");
                break; // maintainSource() releases the link and suspends retries.
            case 't': {
                const uint16_t handle = tvHandle.load();
                if (handle != BLE_HS_CONN_HANDLE_NONE) NimBLEDevice::getServer()->disconnect(handle);
                else Serial.println("TV already disconnected; advertising will continue.");
                break;
            }
            case 'x': releaseAll(); break;
            case 'm': {
                const bool previous = verbose.exchange(true);
                printHex("Report Map", 0, reportMap.data(), reportMap.size()); verbose.store(previous); break;
            }
            case 'p':
                Serial.println("Clearing learned map/target and rebooting. Existing BLE bonds are retained.");
                preferences.clear(); Serial.flush(); ESP.restart(); break;
            case 'f':
                if (forgetTvBond()) { Serial.flush(); ESP.restart(); }
                break;
            case '?': printHelp(); break;
            case '\r': case '\n': case ' ': case '\t': break;
            default: Serial.println("Unknown command; send ? followed by Enter. Commands are one-character lines."); break;
        }
    }
}

void setup() {
    Serial.begin(115200); delay(300);
    Serial.println("\nESP32 dual-role BLE HID bridge; TV-first startup; Serial 115200");
    if (!validSourceAddress()) {
        Serial.println("Invalid SOURCE_ADDRESS. Use six hexadecimal bytes: AA:BB:CC:DD:EE:FF, or an empty string.");
        while (true) delay(1000);
    }
    if (strcasecmp(SOURCE_ADDRESS, "AA:BB:CC:DD:EE:FF") == 0)
        Serial.println("EXAMPLE MAC configured: replace SOURCE_ADDRESS with your remote's real Bluetooth MAC and upload again.");
    reportQueue = xQueueCreate(QUEUE_DEPTH, sizeof(ReportEvent));
    candidateQueue = xQueueCreate(8, sizeof(Candidate));
    if (!reportQueue || !candidateQueue) {
        Serial.println("Queue allocation failed."); while (true) delay(1000);
    }
    preferences.begin("hidbridge", false);
    selectedAddress = preferences.getString("address", "").c_str();
    selectedName = preferences.getString("name", "").c_str();
    selectedAddressType = preferences.getUChar("addrtype", 0);
    if (preferences.getBytesLength("pnp") == sizeof(sourcePnp))
        preferences.getBytes("pnp", sourcePnp, sizeof(sourcePnp));
    sourceCountry = preferences.getUChar("country", 0);
    sourceHidFlags = uint8_t((preferences.getUChar("hidflags", 0x02) & 1) | 2);
    identityPinned = !selectedAddress.empty();
    const size_t mapSize = preferences.getBytesLength("map");
    if (mapSize && mapSize <= MAX_MAP_BYTES) {
        reportMap.resize(mapSize);
        if (preferences.getBytes("map", reportMap.data(), mapSize) != mapSize ||
            !descriptor.parse(reportMap.data(), reportMap.size())) {
            Serial.printf("Saved HID map invalid: %s. Learning it again.\n", descriptor.error.c_str());
            reportMap.clear(); descriptor.reports.clear();
        }
    }

    // Initialize one stack for BOTH roles. Do not deinit/reinit on reconnect.
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    hidbridge::restoreBondIdentityCache(); // Before any GAP procedures start.
    // A 512-byte notification needs at least 515 bytes of negotiated ATT MTU.
    // Individual links may negotiate less; tvReady() prevents truncation.
    if (!NimBLEDevice::setMTU(517)) Serial.println("Could not request MTU 517; check negotiated report sizes.");
    Serial.printf("Bridge BLE address: %s\n", NimBLEDevice::getAddress().toString().c_str());
    printBondStatus(); // Read bonds loaded by NimBLE from flash before any connection.
    Serial.printf("TV security policy: TV first, one fallback request after %lu ms; e=manual retry.\n",
                  static_cast<unsigned long>(TV_SECURITY_GRACE_MS));
    Serial.printf("TV live subscription query: %s\n", ble_gatts_read_cccd ? "available" : "callbacks + saved-bond recovery (older host)");

    sourceClient = NimBLEDevice::createClient();
    if (!sourceClient) { Serial.println("Client allocation failed."); while (true) delay(1000); }
    sourceClient->setClientCallbacks(&sourceCallbacks, false);
    sourceClient->setConnectTimeout(5000); // NimBLE 2.x uses milliseconds.
    // 15–30 ms interval, no slave latency, 4 s supervision timeout.
    sourceClient->setConnectionParams(12, 24, 0, 400);
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&scanCallbacks, false);
    scan->setActiveScan(true);
    scan->setInterval(100); scan->setWindow(30); // Leave radio time for the TV.
    scan->setMaxResults(0); // Candidates are copied into a queue, not stored as pointers.

    if (!reportMap.empty()) startPeripheral();
    else Serial.println("First setup: learn the physical remote before the TV profile can be advertised.");
    volumehttp::begin(); // HTTP and Wi-Fi work run independently of BLE forwarding.
    printHelp(); printStatus();
}

void loop() {
    maintainTvSecurity();
    if (sourceLost.exchange(false)) {
        releaseAll(); // Clear held TV keys if the physical remote goes away.
        for (auto& binding : bindings) binding.source = nullptr;
        nextAttempt = millis() + RETRY_MS; directTried = false;
    }
    if (tvLost.exchange(false)) {
        // Clear local input values without releasing locally intercepted volume
        // keys: local volume control continues while the TV is disconnected.
        for (size_t i = 0; i < descriptor.reports.size(); ++i) sendNeutral(i);
    }
    if (queueFault.exchange(false)) {
        Serial.println("Report queue overflow/oversize: clearing queued events and held keys.");
        xQueueReset(reportQueue); releaseAll(); abortTv("report queue overflow/oversize");
        if (sourceClient->isConnected()) sourceClient->disconnect();
    }
    if (peripheralStarted.load() && advertisingConfigured && tvHandle.load() == BLE_HS_CONN_HANDLE_NONE &&
        !NimBLEDevice::getAdvertising()->isAdvertising()) {
        static uint32_t nextAdvertisingAttempt = 0;
        if (int32_t(millis() - nextAdvertisingAttempt) >= 0) {
            if (!NimBLEDevice::getAdvertising()->start(0)) Serial.println("TV advertising retry failed.");
            nextAdvertisingAttempt = millis() + 1000;
        }
    }
    static uint32_t nextSubscriptionCheck = 0;
    if (int32_t(millis() - nextSubscriptionCheck) >= 0) {
        refreshTvSubscriptions(); nextSubscriptionCheck = millis() + 200;
    }
    // Send a neutral report when a host enables input notifications. Never
    // replay a key or relative motion recorded while the TV was disconnected.
    uint32_t sync = tvSyncMask.load();
    for (size_t i = 0; i < descriptor.reports.size(); ++i) {
        if ((sync & (uint32_t(1) << i)) && tvReady(i)) {
            tvSyncMask.fetch_and(~(uint32_t(1) << i)); sendNeutral(i);
        }
    }
    processReports();
    processSerial();
    maintainSource();
    delay(2);
}
