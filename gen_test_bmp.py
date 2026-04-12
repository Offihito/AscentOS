import struct

def create_test_bmp(filename, width, height):
    # BITMAPFILEHEADER (14 bytes)
    # bfType (2), bfSize (4), bfReserved1 (2), bfReserved2 (2), bfOffBits (4)
    file_header = struct.pack('<2sIHHI', b'BM', 14 + 40 + width * height * 3, 0, 0, 14 + 40)
    
    # BITMAPINFOHEADER (40 bytes)
    # biSize (4), biWidth (4), biHeight (4), biPlanes (2), biBitCount (2), 
    # biCompression (4), biSizeImage (4), biXPelsPerMeter (4), biYPelsPerMeter (4), 
    # biClrUsed (4), biClrImportant (4)
    info_header = struct.pack('<IIIHHIIIIII', 40, width, height, 1, 24, 0, width * height * 3, 2835, 2835, 0, 0)
    
    with open(filename, 'wb') as f:
        f.write(file_header)
        f.write(info_header)
        
        # Pixels (BGR) - Draw a simple pattern
        for y in range(height):
            for x in range(width):
                # Simple gradient
                r = (x * 255 // width)
                g = (y * 255 // height)
                b = 128
                f.write(struct.pack('<BBB', b, g, r))
            
            # Padding to 4-byte boundary
            padding = (4 - (width * 3) % 4) % 4
            f.write(b'\x00' * padding)

if __name__ == "__main__":
    create_test_bmp('test.bmp', 256, 256)
    print("Created test.bmp")
