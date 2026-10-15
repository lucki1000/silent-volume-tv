#pragma once

// NimBLE-Arduino 2.5.1 on the original ESP32 resolves connection RPAs through
// peer_dev_rec, separate from the persisted security records. Its NVS helper
// does not update a peer record when the record count is unchanged after
// re-pairing. Refresh this RAM cache from the authoritative saved peer keys,
// before starting any advertising/scanning. Never write or log key material.
#if defined(CONFIG_IDF_TARGET_ESP32) && CONFIG_IDF_TARGET_ESP32 && \
    defined(NIMBLE_CPP_VERSION_MAJOR) && NIMBLE_CPP_VERSION_MAJOR == 2 && \
    NIMBLE_CPP_VERSION_MINOR == 5 && NIMBLE_CPP_VERSION_PATCH == 1
#define HIDBRIDGE_RESTORE_IDENTITY_CACHE 1
#include "nimble/nimble/host/src/ble_hs_resolv_priv.h"
#else
#define HIDBRIDGE_RESTORE_IDENTITY_CACHE 0
#endif

namespace hidbridge {
static inline void printBondIdentityResolution(const NimBLEAddress& address) {
#if HIDBRIDGE_RESTORE_IDENTITY_CACHE && MYNEWT_VAL(BLE_HOST_BASED_PRIVACY)
    ble_addr_t air = *address.getBase();
    if (air.type != 1 || (air.val[5] & 0xC0) != 0x40) return;
    const int count = NimBLEDevice::getNumBonds();
    for (int i = 0; i < count; ++i) {
        const auto identity = NimBLEDevice::getBondedAddress(i);
        ble_store_key_sec key{};
        ble_store_value_sec peer{};
        key.peer_addr = *identity.getBase();
        if (ble_store_read_peer_sec(&key, &peer) != 0 || !peer.irk_present) continue;
        uint8_t reversed[16];
        for (size_t j = 0; j < sizeof(reversed); ++j) reversed[j] = peer.irk[sizeof(reversed) - 1 - j];
        const int rawRc = ble_hs_resolv_rpa(air.val, peer.irk);
        const int reversedRc = ble_hs_resolv_rpa(air.val, reversed);
        Serial.printf("TV RPA check: saved identity=%s stored-order-match=%s host-order-match=%s peer-cache=%s resolving-list=%s\n",
                      identity.toString().c_str(), rawRc == 0 ? "yes" : "no",
                      reversedRc == 0 ? "yes" : "no",
                      ble_rpa_find_peer_dev_rec(peer.peer_addr.val) ? "yes" : "no",
                      ble_hs_resolv_list_find(peer.peer_addr.val) ? "yes" : "no");
    }
#else
    (void)address;
#endif
}

static inline void restoreBondIdentityCache() {
#if HIDBRIDGE_RESTORE_IDENTITY_CACHE && MYNEWT_VAL(BLE_HOST_BASED_PRIVACY)
    const int capacity = MYNEWT_VAL(BLE_STORE_MAX_BONDS) + 1;
    const int count = NimBLEDevice::getNumBonds();
    Serial.printf("Host identity cache: records=%d; checking %d saved bonds.\n",
                  ble_rpa_get_num_peer_dev_records(), count);
    for (int i = 0; i < count; ++i) {
        const auto identity = NimBLEDevice::getBondedAddress(i);
        ble_store_key_sec key{};
        ble_store_value_sec peer{};
        key.peer_addr = *identity.getBase();
        if (ble_store_read_peer_sec(&key, &peer) != 0 || !peer.irk_present) continue;
        auto* record = ble_rpa_find_peer_dev_rec(peer.peer_addr.val);
        const bool added = record == nullptr;
        if (added) {
            const int used = ble_rpa_get_num_peer_dev_records();
            if (used < 0 || used >= capacity || ble_rpa_resolv_add_peer_rec(peer.peer_addr.val) != 0) {
                Serial.println("Host identity cache full; saved bond is retained.");
                continue;
            }
            record = ble_rpa_find_peer_dev_rec(peer.peer_addr.val);
            if (!record) continue;
        }
        // Stored SMP IRKs use wire byte order; peer_dev_rec uses the reversed
        // order, just as ble_sm.c's swap_buf() does after identity exchange.
        bool matches = record->peer_sec.irk_present &&
                       record->peer_sec.peer_addr.type == peer.peer_addr.type &&
                       memcmp(record->identity_addr, peer.peer_addr.val, 6) == 0;
        for (size_t j = 0; j < sizeof(peer.irk); ++j)
            matches = matches && record->peer_sec.irk[j] == peer.irk[sizeof(peer.irk) - 1 - j];
        if (!matches) {
            record->peer_sec.peer_addr = peer.peer_addr;
            record->peer_sec.irk_present = true;
            memcpy(record->identity_addr, peer.peer_addr.val, 6);
            for (size_t j = 0; j < sizeof(peer.irk); ++j)
                record->peer_sec.irk[j] = peer.irk[sizeof(peer.irk) - 1 - j];
        }
        Serial.printf("Host identity cache %s: %s type=%u (saved IRK; keys not logged).\n",
                      added ? "restored" : matches ? "matches" : "refreshed",
                      identity.toString().c_str(), unsigned(identity.getType()));
    }
#else
    Serial.println("Host identity cache workaround: not required for this library/target.");
#endif
}
} // namespace hidbridge
