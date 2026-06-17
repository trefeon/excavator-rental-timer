import gzip
import os
import sys

# Default: build from frontend/index.html (new live dashboard).
# Override with CLI arg 1: <input.html>
candidates = ["../frontend/index.html"]
if len(sys.argv) > 1 and sys.argv[1]:
    input_file = sys.argv[1]
else:
    input_file = next((c for c in candidates if os.path.exists(c)), candidates[0])

output_file = "wifi_master_esp32_WEBUI/index_html.h"

print(f"Reading {input_file}...")
with open(input_file, "rb") as f:
    html_data = f.read()

print("Compressing...")
compressed_data = gzip.compress(html_data)

print(f"Original size: {len(html_data)} bytes")
print(f"Compressed size: {len(compressed_data)} bytes")

# Format as C array
c_array = ", ".join([f"0x{b:02X}" for b in compressed_data])

header_content = f"""// Auto-generated from {os.path.basename(input_file)} via firmware/build_frontend.py
#ifndef INDEX_HTML_H
#define INDEX_HTML_H

#include <Arduino.h>

const uint8_t index_html_gz[] PROGMEM = {{
    {c_array}
}};

#endif
"""

print(f"Writing to {output_file}...")
with open(output_file, "w") as f:
    f.write(header_content)

print("Done!")
