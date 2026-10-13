"""Test real credential persistence/validation against an in-memory NVS fake.

Requires the same Windows TinyCC compiler as test_wifi_ui.py. This checks the
application's handling of NVS results, not physical flash/power-loss behavior.
"""
import ctypes
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/credentials_host"
STUBS = OUT / "stubs"
STUBS.mkdir(parents=True, exist_ok=True)
compiler = Path(os.environ.get("TCC", str(ROOT / "build/ui_host/tcc/tcc.exe")))
(STUBS / "esp_err.h").write_text("""#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
""")
(STUBS / "esp_wifi_types.h").write_text("""#pragma once
enum { WIFI_AUTH_OPEN=0, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK,
       WIFI_AUTH_WPA_WPA2_PSK, WIFI_AUTH_ENTERPRISE, WIFI_AUTH_WPA3_PSK, WIFI_AUTH_WPA2_WPA3_PSK };
""")
(STUBS / "nvs.h").write_text("""#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef unsigned nvs_handle_t;
enum { NVS_READONLY, NVS_READWRITE };
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
""")
subprocess.run([str(compiler), "-shared", "-I" + str(STUBS), "-I" + str(ROOT / "main/include"),
                str(ROOT / "main/wifi_credentials.c"), str(ROOT / "tools/wifi_credentials_host.c"),
                "-o", str(OUT / "credentials.dll")], check=True)
dll = ctypes.CDLL(str(OUT / "credentials.dll"))
dll.credential_tests.restype = ctypes.c_int
failure = dll.credential_tests()
assert failure == 0, f"Credential test failed at C line {failure}"
print("PASS: NVS round-trip, unchanged-write avoidance, credential bounds, invalid blobs, and NVS error handling")
