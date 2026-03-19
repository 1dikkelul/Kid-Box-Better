import os
import sys
import struct
from PIL import Image

# LVGL v9 color format enum value for RGB565 (lv_color_format_t)
LV_COLOR_FORMAT_RGB565 = 0x12

def main():
    if len(sys.argv) < 2:
        print("Usage: python png2bin.py <path_to_icons_folder>")
        return

    folder = sys.argv[1]
    
    for filename in os.listdir(folder):
        if filename.lower().endswith(".png"):
            src_path = os.path.join(folder, filename)
            dst_path = os.path.splitext(src_path)[0] + ".bin"
            
            print(f"Converting: {filename} -> {os.path.basename(dst_path)}")
            
            try:
                with Image.open(src_path) as img:
                    img = img.convert("RGBA")
                    width, height = img.size
                    
                    # 1. RLCD HIGH CONTRAST MODE
                    # Create a solid WHITE background
                    final_img = Image.new("RGB", img.size, (255, 255, 255))

                    # Create solid BLACK ink (the icon lines)
                    ink = Image.new("RGB", img.size, (0, 0, 0))
                    
                    # Paste White Ink onto Black Background using the Icon's Alpha as a mask
                    final_img.paste(ink, (0, 0), mask=img.split()[3])
                    
                    # 2. LVGL v9 Image Header (12 bytes)
                    # Magic: 0x19, CF: 0x12 (RGB565), Flags: 0
                    magic = 0x19
                    cf = LV_COLOR_FORMAT_RGB565
                    flags = 0
                    stride = width * 2
                    reserved = 0
                    
                    header = struct.pack("<BBHHHHH", magic, cf, flags, width, height, stride, reserved)
                    
                    # 3. Convert pixels to RGB565
                    pixel_data = bytearray()
                    for r, g, b in final_img.getdata():
                        # Pack RGB888 to RGB565
                        rgb = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                        pixel_data += struct.pack("<H", rgb)
                        
                    # 4. Write binary file
                    with open(dst_path, "wb") as f:
                        f.write(header)
                        f.write(pixel_data)
                    
                    print(f"  [OK] Converted (Forced Black-on-White Box)")
                        
            except Exception as e:
                print(f"  Error converting {filename}: {e}")

if __name__ == "__main__":
    main()
