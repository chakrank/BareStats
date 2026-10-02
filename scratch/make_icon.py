import struct
import zlib
import os

def create_png(width, height, draw_func):
    """Creates a PNG in pure Python using zlib without external libraries."""
    raw_data = bytearray()
    for y in range(height):
        raw_data.append(0) # Filter byte 0 (None)
        for x in range(width):
            r, g, b, a = draw_func(x, y, width, height)
            raw_data.extend([r, g, b, a])
    
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    png += chunk(b"IHDR", ihdr)
    compressed = zlib.compress(bytes(raw_data), 9)
    png += chunk(b"IDAT", compressed)
    png += chunk(b"IEND", b"")
    return png

def create_bmp_dib(width, height, draw_func):
    """Creates a 32-bit BGRA DIB for Windows ICO entry."""
    header = struct.pack("<IIIHHIIIIII",
        40, width, height * 2, 1, 32, 0, width * height * 4, 0, 0, 0, 0
    )
    xor_data = bytearray()
    for y in range(height - 1, -1, -1):
        for x in range(width):
            r, g, b, a = draw_func(x, y, width, height)
            xor_data.extend([b, g, r, a]) # BGRA

    row_bytes = ((width + 31) // 32) * 4
    and_mask = bytearray(row_bytes * height)
    return header + bytes(xor_data) + bytes(and_mask)

def draw_chip_telemetry_icon(x, y, w, h):
    nx = (x + 0.5) / float(w)
    ny = (y + 0.5) / float(h)

    # 1. Check Contact Pins (Silicon package pins around perimeter)
    # 3 pins on each of 4 edges: centers at 0.32, 0.50, 0.68 with half-width 0.04
    pin_centers = [0.32, 0.50, 0.68]
    is_pin = False
    
    # Left pins: nx in [0.03, 0.12]
    if 0.03 <= nx <= 0.12:
        for c in pin_centers:
            if abs(ny - c) <= 0.045:
                is_pin = True
                break
    # Right pins: nx in [0.88, 0.97]
    if 0.88 <= nx <= 0.97:
        for c in pin_centers:
            if abs(ny - c) <= 0.045:
                is_pin = True
                break
    # Top pins: ny in [0.03, 0.12]
    if 0.03 <= ny <= 0.12:
        for c in pin_centers:
            if abs(nx - c) <= 0.045:
                is_pin = True
                break
    # Bottom pins: ny in [0.88, 0.97]
    if 0.88 <= ny <= 0.97:
        for c in pin_centers:
            if abs(nx - c) <= 0.045:
                is_pin = True
                break

    # 2. Main Chip Body: Rounded square in [0.11, 0.89] x [0.11, 0.89]
    rx = abs(nx - 0.5)
    ry = abs(ny - 0.5)
    max_extent = 0.39
    cr = 0.07 # corner radius
    c_limit = max_extent - cr
    
    in_body = False
    if rx <= max_extent and ry <= max_extent:
        if rx > c_limit and ry > c_limit:
            dx = rx - c_limit
            dy = ry - c_limit
            if dx*dx + dy*dy <= cr*cr:
                in_body = True
        else:
            in_body = True

    if not in_body:
        if is_pin:
            # Metallic titanium-gold pin
            return 210, 195, 140, 255
        return 0, 0, 0, 0 # Transparent outside chip

    # 3. Chip Body Styling
    # Beveled metallic perimeter border
    is_outer_border = False
    if rx >= max_extent - 0.025 or ry >= max_extent - 0.025:
        is_outer_border = True
    if rx > c_limit and ry > c_limit:
        dx = rx - c_limit
        dy = ry - c_limit
        if dx*dx + dy*dy >= (cr - 0.025)*(cr - 0.025):
            is_outer_border = True

    if is_outer_border:
        return 55, 75, 95, 255 # Gunmetal steel border

    # Inner Silicon Die Window: [0.19, 0.81] x [0.19, 0.81]
    is_die_border = False
    in_die = False
    if abs(nx - 0.5) <= 0.31 and abs(ny - 0.5) <= 0.31:
        if abs(nx - 0.5) >= 0.29 or abs(ny - 0.5) >= 0.29:
            is_die_border = True
        else:
            in_die = True

    if is_die_border:
        return 38, 52, 70, 255

    if not in_die:
        # Carrier plate
        return 18, 25, 36, 255

    # 4. Inside the Silicon Die (Telemetric Hardware Meter Bars)
    # Background of die: Deep dark matte obsidian silicon
    bg_die = (10, 14, 20, 255)

    # 3 Horizontal telemetry level meters:
    # Bar 1 (Top, CPU): ny in [0.27, 0.38]
    # Bar 2 (Mid, GPU): ny in [0.44, 0.55]
    # Bar 3 (Bot, RAM): ny in [0.61, 0.72]
    # Track width: nx in [0.27, 0.73]
    track_left = 0.27
    track_right = 0.73
    track_w = track_right - track_left

    bars = [
        # (y_top, y_bot, fill_pct, color_main, color_hi, tag_x)
        (0.27, 0.38, 0.65, (0, 220, 255), (90, 245, 255)), # CPU: Cyan
        (0.44, 0.55, 0.85, (0, 235, 140), (120, 255, 190)), # GPU: Emerald Mint
        (0.61, 0.72, 0.48, (255, 185, 20), (255, 225, 90)), # RAM: Amber Gold
    ]

    for y_top, y_bot, fill_pct, col_main, col_hi in bars:
        # Mini channel indicator tick at left: nx in [0.22, 0.245]
        if y_top + 0.02 <= ny <= y_bot - 0.02 and 0.225 <= nx <= 0.245:
            return col_main[0], col_main[1], col_main[2], 255

        # Bar track
        if y_top <= ny <= y_bot and track_left <= nx <= track_right:
            fill_x = track_left + track_w * fill_pct
            # Groove corner rounding
            edge_pad_y = min(ny - y_top, y_bot - ny)
            if nx <= fill_x:
                # Active glowing meter level
                if ny - y_top <= 0.035:
                    return col_hi[0], col_hi[1], col_hi[2], 255 # Top specular highlight
                return col_main[0], col_main[1], col_main[2], 255
            else:
                # Unfilled groove background
                return 22, 32, 45, 255

    return bg_die

def build_ico(output_path):
    sizes = [16, 24, 32, 48, 256]
    images = []
    
    for s in sizes:
        if s == 256:
            data = create_png(s, s, draw_chip_telemetry_icon)
        else:
            data = create_bmp_dib(s, s, draw_chip_telemetry_icon)
        images.append((s, data))
        
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    
    with open(output_path, "wb") as f:
        # ICONDIR header: idReserved=0, idType=1 (ICO), idCount=len(images)
        f.write(struct.pack("<HHH", 0, 1, len(images)))
        
        offset = 6 + len(images) * 16
        for s, data in images:
            width_byte = 0 if s == 256 else s
            height_byte = 0 if s == 256 else s
            f.write(struct.pack("<BBBBHHII",
                width_byte, height_byte, 0, 0, 1, 32, len(data), offset
            ))
            offset += len(data)
            
        for s, data in images:
            f.write(data)
            
    print(f"Generated {output_path} successfully ({os.path.getsize(output_path)} bytes).")

if __name__ == "__main__":
    out_ico = os.path.join(os.path.dirname(__file__), "..", "resources", "bare_stats.ico")
    build_ico(out_ico)
