#!/usr/bin/env python3
"""Fix same-count NVS updates in NimBLE-Arduino 2.5.1; check by default.

Run --apply on the Arduino library to install. The original source is backed up
next to the patched file. This modifies source code only, never a device's NVS.
"""
import argparse
import difflib
import os
from pathlib import Path
import tempfile

MARKER = "/* BLEHIDBridge: persist changed records even when their count is unchanged. */"
HELPER = r'''
/* BLEHIDBridge: persist changed records even when their count is unchanged. */
static int
hidbridge_sync_nvs_records(const char *prefix, const void *records,
                          size_t record_size, int count, int capacity)
{
    nvs_handle_t handle;
    esp_err_t err;
    unsigned char *previous;
    const unsigned char *values = records;
    char key[16];
    bool changed = false;
    int result = BLE_HS_ESTORE_FAIL;

    if (count < 0 || count > capacity || record_size == 0) {
        return BLE_HS_ESTORE_CAP;
    }
    previous = malloc(record_size);
    if (previous == NULL) {
        return BLE_HS_ENOMEM;
    }
    err = nvs_open(NIMBLE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        free(previous);
        return BLE_HS_ESTORE_FAIL;
    }
    for (int i = 0; i < capacity; ++i) {
        snprintf(key, sizeof(key), "%s_%d", prefix, i + 1);
        if (i < count) {
            size_t size = record_size;
            err = nvs_get_blob(handle, key, previous, &size);
            if (err == ESP_OK) {
                if (size != record_size) {
                    goto done;
                }
                if (memcmp(previous, values + i * record_size, record_size) == 0) {
                    continue;
                }
            } else if (err != ESP_ERR_NVS_NOT_FOUND) {
                goto done;
            }
            err = nvs_set_blob(handle, key, values + i * record_size, record_size);
        } else {
            err = nvs_erase_key(handle, key);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                continue;
            }
        }
        if (err != ESP_OK) {
            goto done;
        }
        changed = true;
    }
    if (!changed || nvs_commit(handle) == ESP_OK) {
        result = 0;
    }
done:
    nvs_close(handle);
    free(previous);
    return result;
}
'''

REPLACEMENTS = {
    "ble_store_config_persist_our_secs": (
        "NIMBLE_NVS_OUR_SEC_KEY", "ble_store_config_our_secs", "struct ble_store_value_sec",
        "ble_store_config_num_our_secs", "MYNEWT_VAL(BLE_STORE_MAX_BONDS)"),
    "ble_store_config_persist_peer_secs": (
        "NIMBLE_NVS_PEER_SEC_KEY", "ble_store_config_peer_secs", "struct ble_store_value_sec",
        "ble_store_config_num_peer_secs", "MYNEWT_VAL(BLE_STORE_MAX_BONDS)"),
    "ble_store_config_persist_cccds": (
        "NIMBLE_NVS_CCCD_SEC_KEY", "ble_store_config_cccds", "struct ble_store_value_cccd",
        "ble_store_config_num_cccds", "MYNEWT_VAL(BLE_STORE_MAX_CCCDS)"),
    "ble_store_persist_peer_records": (
        "NIMBLE_NVS_PEER_RECORDS_KEY", "ble_rpa_get_peer_dev_records()", "struct ble_hs_dev_records",
        "ble_rpa_get_num_peer_dev_records()", "(MYNEWT_VAL(BLE_STORE_MAX_BONDS) + 1)"),
}


def patch(source):
    if MARKER in source:
        return source
    for name, (prefix, records, record_type, count, capacity) in REPLACEMENTS.items():
        start = source.index(name + "(void)")
        body = source.index("{", start)
        end, depth = body + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        original = source[body:end]
        if "nvs_count" not in original or "return 0;" not in original:
            raise ValueError(f"Unexpected upstream implementation of {name}; refusing to patch")
        source = source[:body] + (
            "{\n    return hidbridge_sync_nvs_records(" + prefix + ", " + records + ",\n"
            "                                      sizeof(" + record_type + "),\n"
            "                                      " + count + ", " + capacity + ");\n}"
        ) + source[end:]
    insertion = source.index("#if MYNEWT_VAL(BLE_STORE_MAX_CCCDS)\nint ble_store_config_persist_cccds(void)")
    source = source[:insertion] + HELPER + "\n" + source[insertion:]
    source = source.replace("#include <string.h>", "#include <string.h>\n#include <stdlib.h>\n#include <stdio.h>", 1)
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, default=Path.home() / "Arduino/libraries/NimBLE-Arduino")
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    properties = (args.library / "library.properties").read_text()
    if "version=2.5.1" not in properties.splitlines():
        parser.error("This patch requires NimBLE-Arduino exactly 2.5.1")
    path = args.library / "src/nimble/nimble/host/store/config/src/ble_store_nvs.c"
    source = path.read_text()
    result = patch(source)
    if result == source:
        print("NimBLE bond persistence patch is already installed.")
        return
    if not args.apply:
        print("".join(difflib.unified_diff(source.splitlines(True), result.splitlines(True),
                                        fromfile=str(path), tofile=str(path) + " (patched)")))
        return
    backup = path.with_suffix(path.suffix + ".blehidbridge-original")
    if backup.exists():
        parser.error(f"Backup already exists; inspect it before replacing: {backup}")
    backup.write_text(source)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(result)
        temporary.chmod(path.stat().st_mode & 0o777)
        os.replace(temporary, path)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()
    print(f"Patched {path}; original source saved as {backup.name}")


if __name__ == "__main__":
    main()
