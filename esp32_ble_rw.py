import asyncio
import sys
import os
from bleak import BleakClient

# ESP32 MAC address
DEVICE_ADDRESS = "8c:4f:00:15:4d:7a"

# UUIDs for GATT characteristics
FILE_RW_CHAR_UUID = "00001526-1212-efde-1523-785feabcd123"     # Read/write file data
OFFSET_CHAR_UUID = "00001527-1212-efde-1523-785feabcd123"      # Set read offset

CHUNK_SIZE = 500  # Must match ESP32 chunk size

# --- Upload ---
async def write_file(client, filepath):
    with open(filepath, "rb") as f:
        data = f.read()

    print(f"Sending {len(data)} bytes...")
    for i in range(0, len(data), CHUNK_SIZE):
        chunk = data[i:i+CHUNK_SIZE]
        await client.write_gatt_char(FILE_RW_CHAR_UUID, chunk)

    # If it's a .bin file, send OTA_END to trigger esp_restart()
    if filepath.lower().endswith(".bin"):
        print("Sending OTA_END signal...")
        await client.write_gatt_char(FILE_RW_CHAR_UUID, b"OTA_END")

    print("File sent to ESP32!")

# --- Download ---
async def read_file(client, filepath):
    data = bytearray()
    offset = 0

    while True:
        # Send read offset to ESP32
        offset_bytes = offset.to_bytes(4, byteorder='little')
        await client.write_gatt_char(OFFSET_CHAR_UUID, offset_bytes)

        # Read chunk
        chunk = await client.read_gatt_char(FILE_RW_CHAR_UUID)
        if not chunk:
            break

        data.extend(chunk)
        print(f"Read {len(chunk)} bytes at offset {offset}")

        if len(chunk) < CHUNK_SIZE:
            break  # Last chunk

        offset += len(chunk)

    with open(filepath, "wb") as f:
        f.write(data)
    print(f"File downloaded from ESP32! ({len(data)} bytes)")

# --- Main ---
async def run(upload_path):
    download_path = f"downloaded_{os.path.basename(upload_path)}"

    async with BleakClient(DEVICE_ADDRESS) as client:
        await write_file(client, upload_path)

        # Skip reading if it's a .bin file
        if upload_path.lower().endswith(".bin"):
            print("Skipping read step for .bin file.")
            return

        try:
            await read_file(client, download_path)
        except Exception as e:
            print(f"Skipping read step due to error: {e}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python esp32_ble_rw.py <file_to_upload>")
        sys.exit(1)

    filepath = sys.argv[1]

    if not os.path.isfile(filepath):
        print(f"File not found: {filepath}")
        sys.exit(1)

    asyncio.run(run(filepath))

