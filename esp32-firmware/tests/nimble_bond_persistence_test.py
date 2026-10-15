#!/usr/bin/env python3
"""Run the patch's actual C persistence helper with synthetic NVS records."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("bond_patch", root / "tools/patch_nimble_bonds.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

mock = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int nvs_handle_t;
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define NVS_READWRITE 1
#define NIMBLE_NVS_NAMESPACE "nimble_bond"
#define BLE_HS_ESTORE_FAIL 10
#define BLE_HS_ESTORE_CAP 11
#define BLE_HS_ENOMEM 12
struct entry { char key[16]; unsigned char data[64]; size_t size; bool used; };
static struct entry entries[24];
static int sets, erases, commits, closes;
static bool fail_open, fail_write, fail_commit;
static struct entry *lookup(const char *key) {
    for (size_t i = 0; i < 24; ++i)
        if (entries[i].used && !strcmp(entries[i].key, key)) return &entries[i];
    return NULL;
}
static int nvs_open(const char *ns, int mode, nvs_handle_t *handle) {
    assert(!strcmp(ns, NIMBLE_NVS_NAMESPACE) && mode == NVS_READWRITE);
    *handle = 1; return fail_open ? 99 : ESP_OK;
}
static int nvs_get_blob(int handle, const char *key, void *data, size_t *size) {
    assert(handle == 1);
    struct entry *entry = lookup(key);
    if (!entry) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < entry->size) return 99;
    memcpy(data, entry->data, entry->size); *size = entry->size; return ESP_OK;
}
static int nvs_set_blob(int handle, const char *key, const void *data, size_t size) {
    assert(handle == 1 && size <= 64); if (fail_write) return 99;
    struct entry *entry = lookup(key);
    if (!entry) for (size_t i = 0; i < 24; ++i)
        if (!entries[i].used) { entry = &entries[i]; break; }
    assert(entry); strcpy(entry->key, key); entry->used = true;
    memcpy(entry->data, data, size); entry->size = size; ++sets; return ESP_OK;
}
static int nvs_erase_key(int handle, const char *key) {
    assert(handle == 1); struct entry *entry = lookup(key);
    if (!entry) return ESP_ERR_NVS_NOT_FOUND;
    if (fail_write) return 99;
    entry->used = false; ++erases; return ESP_OK;
}
static int nvs_commit(int handle) { assert(handle == 1); ++commits; return fail_commit ? 99 : 0; }
static void nvs_close(int handle) { assert(handle == 1); ++closes; }
'''

checks = r'''
int main(void) {
    unsigned char records[3][16] = {{1}, {2}, {3}};
    const char *prefixes[] = {"our_sec", "peer_sec", "cccd_sec", "p_dev_rec"};
    for (size_t i = 0; i < 4; ++i) {
        assert(hidbridge_sync_nvs_records(prefixes[i], records, 16, 2, 3) == 0);
    }
    assert(sets == 8 && commits == 4);
    // Pairing replaces an existing key: the record count does not change.
    records[0][0] = 77;
    assert(hidbridge_sync_nvs_records("peer_sec", records, 16, 2, 3) == 0);
    assert(sets == 9 && commits == 5);
    unsigned char reloaded[16]; size_t size = sizeof(reloaded);
    assert(nvs_get_blob(1, "peer_sec_1", reloaded, &size) == 0);
    assert(reloaded[0] == 77 && size == 16); // Value loaded on next boot.
    assert(lookup("our_sec_1")->data[0] == 1); // Other collections untouched.
    // Identical snapshots produce no extra writes or commits.
    assert(hidbridge_sync_nvs_records("peer_sec", records, 16, 2, 3) == 0);
    assert(sets == 9 && commits == 5 && erases == 0);
    // Addition and deletion preserve contiguous native key numbering.
    assert(hidbridge_sync_nvs_records("peer_sec", records, 16, 3, 3) == 0);
    memcpy(records[0], records[2], 16);
    assert(hidbridge_sync_nvs_records("peer_sec", records, 16, 1, 3) == 0);
    assert(lookup("peer_sec_1")->data[0] == 3);
    assert(!lookup("peer_sec_2") && !lookup("peer_sec_3"));
    assert(lookup("our_sec_2"));
    assert(hidbridge_sync_nvs_records("peer_sec", records, 16, 0, 3) == 0);
    assert(!lookup("peer_sec_1"));
    assert(hidbridge_sync_nvs_records("peer_sec", records, 16, 4, 3) == BLE_HS_ESTORE_CAP);
    assert(hidbridge_sync_nvs_records("peer_sec", records, 16, -1, 3) == BLE_HS_ESTORE_CAP);
    fail_open = true;
    assert(hidbridge_sync_nvs_records("peer_sec", records, 16, 1, 3) == BLE_HS_ESTORE_FAIL);
    fail_open = false; fail_write = true;
    int previous_closes = closes;
    assert(hidbridge_sync_nvs_records("peer_sec", records, 16, 1, 3) == BLE_HS_ESTORE_FAIL);
    assert(closes == previous_closes + 1);
    fail_write = false; fail_commit = true;
    assert(hidbridge_sync_nvs_records("peer_sec", records, 16, 1, 3) == BLE_HS_ESTORE_FAIL);
    puts("NimBLE persistence: same-count updates, unchanged snapshots, deletion and errors passed");
}
'''

# Ensure every targeted upstream function delegates to the tested helper.
source_path = Path.home() / "Arduino/libraries/NimBLE-Arduino/src/nimble/nimble/host/store/config/src/ble_store_nvs.c"
if source_path.exists():
    patched = module.patch(source_path.read_text())
    assert module.patch(patched) == patched
    for name in module.REPLACEMENTS:
        start = patched.index(name + "(void)")
        body = patched[patched.index("{", start):patched.index("}", start)]
        assert "return hidbridge_sync_nvs_records" in body
        assert "nvs_count" not in body

with tempfile.TemporaryDirectory(prefix="nimble-persistence-") as directory:
    path = Path(directory)
    source, binary = path / "test.c", path / "test"
    source.write_text(mock + module.HELPER + checks)
    subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
