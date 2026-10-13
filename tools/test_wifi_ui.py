"""Compile the real renderer/hit tests as a Windows DLL; test and export previews.

Uses Windows TinyCC (TCC environment variable) and the Python standard library.
No Wi-Fi or touch hardware is mocked as working by these tests.
"""
import ctypes
import os
from pathlib import Path
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "ui_host"
OUT.mkdir(parents=True, exist_ok=True)
compiler = Path(os.environ.get("TCC", str(OUT / "tcc/tcc.exe")))
stubs = OUT / "stubs"
stubs.mkdir(exist_ok=True)
(stubs / "esp_err.h").write_text("typedef int esp_err_t;\n")
subprocess.run([str(compiler), "-shared", "-I" + str(stubs), "-I" + str(ROOT / "main/include"),
                str(ROOT / "main/wifi_ui_view.c"), str(ROOT / "tools/wifi_ui_host.c"),
                "-o", str(OUT / "ui.dll")], check=True)
dll = ctypes.CDLL(str(OUT / "ui.dll"))
dll.host_tests.restype = ctypes.c_int
failure = dll.host_tests()
assert failure == 0, f"UI test failed at C line {failure}"
font = (ctypes.c_uint8 * 193040).from_buffer_copy((ROOT / "main/assets/ui_font.bin").read_bytes())
background = (ctypes.c_uint16 * (480 * 480)).from_buffer_copy((ROOT / "main/assets/wifi_background.rgb565").read_bytes())
pixels = (ctypes.c_uint16 * (480 * 480))()
dll.host_render.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(ctypes.c_uint16), ctypes.POINTER(ctypes.c_uint16)]

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

for screen, name in [(1, "wifi_configuration"), (2, "wifi_keyboard"), (3, "wifi_connected"), (4, "wifi_connecting"), (5, "agent_settings"), (6, "agent_conversation")]:
    dll.host_render(screen, font, background, pixels)
    raw = bytearray()
    for y in range(480):
        raw.append(0)
        for x in range(480):
            pixel = pixels[y * 480 + x]
            raw.extend((((pixel >> 11) & 31) * 255 // 31,
                        ((pixel >> 5) & 63) * 255 // 63, (pixel & 31) * 255 // 31))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 480, 480, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    (ROOT / "main/assets" / (name + "_preview.png")).write_bytes(png)
print("PASS: touch, keyboard, password limits, connection/timeout routing, agent controls, and six renderer previews")
