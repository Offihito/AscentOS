#!/bin/bash
# Create a FAT32 test image for AscentOS kernel testing

set -e

IMG_SIZE=64  # MB (FAT32 requires ~65525 clusters minimum)
IMG_NAME="fat32_test.img"
MOUNT_DIR="/tmp/fat32_mount"

echo "Creating FAT32 test image ($IMG_SIZE MB)..."

# Create empty image
dd if=/dev/zero of="$IMG_NAME" bs=1M count=$IMG_SIZE

# Format as FAT32
mkfs.fat -F 32 "$IMG_NAME"

# Mount and populate
mkdir -p "$MOUNT_DIR"
sudo mount -o loop,uid=$(id -u),gid=$(id -g) "$IMG_NAME" "$MOUNT_DIR"

# Create test files
echo "Hello from FAT32!" > "$MOUNT_DIR/hello.txt"
echo "This is a test file for AscentOS FAT32 driver testing." > "$MOUNT_DIR/readme.txt"

# Create a file with known content for binary testing
dd if=/dev/urandom of="$MOUNT_DIR/binary.dat" bs=512 count=4 2>/dev/null

# Create a subdirectory with files
mkdir -p "$MOUNT_DIR/subdir"
echo "File in subdirectory" > "$MOUNT_DIR/subdir/nested.txt"

# Create files with long filenames (LFN test)
echo "Long filename test" > "$MOUNT_DIR/This Is A Very Long Filename.txt"
echo "Another long name" > "$MOUNT_DIR/Another Long File Name For Testing.dat"

# Create a larger file that spans multiple clusters
dd if=/dev/zero of="$MOUNT_DIR/largefile.bin" bs=4096 count=4 2>/dev/null

# Show what we created
echo ""
echo "Created test files:"
ls -la "$MOUNT_DIR"
ls -la "$MOUNT_DIR/subdir"

# Unmount
sudo umount "$MOUNT_DIR"
rmdir "$MOUNT_DIR"

echo ""
echo "FAT32 test image created: $IMG_NAME"
echo "Size: $(stat -c %s $IMG_NAME) bytes"
