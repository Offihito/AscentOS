#!/bin/bash
set -e

# Distribution Image Creator for AscentOS
IMG="ascentos-dist.img"
BOOT_SIZE=64
ROOT_SIZE=512

echo "[INFO] Creating blank image: $IMG"
dd if=/dev/zero of=$IMG bs=1M count=$((BOOT_SIZE + ROOT_SIZE + 32))

echo "[INFO] Partitioning image (GPT)..."
# Partition 1: BIOS Boot (for Limine BIOS), Partition 2: ESP (FAT32), Partition 3: Root (Linux)
parted -s "$IMG" mklabel gpt
parted -s "$IMG" mkpart BIOSBOOT 1MiB 2MiB
parted -s "$IMG" set 1 bios_grub on
parted -s "$IMG" mkpart ESP fat32 2MiB $((BOOT_SIZE + 2))MiB
parted -s "$IMG" set 2 esp on
parted -s "$IMG" mkpart ROOT $((BOOT_SIZE + 2))MiB $((BOOT_SIZE + ROOT_SIZE + 2))MiB

echo "[INFO] Setting up loop devices..."
# Using sudo for loop devices and mounting
sudo losetup -Pf $IMG
LOOP_DEV=$(losetup -j $IMG | cut -d':' -f1)
sudo udevadm settle

# Cleanup function
cleanup() {
    echo "[INFO] Cleaning up..."
    sudo umount /mnt || true
    sudo losetup -d $LOOP_DEV || true
}
trap cleanup EXIT

echo "[INFO] Formatting Partition 2 (FAT32/ESP)..."
sudo mkfs.vfat -F 32 ${LOOP_DEV}p2

echo "[INFO] Populating Partition 2 (Boot/UEFI)..."
sudo mount ${LOOP_DEV}p2 /mnt
sudo mkdir -p /mnt/boot/limine
sudo mkdir -p /mnt/EFI/BOOT
sudo cp kernel/bin-x86_64/kernel /mnt/boot/
sudo cp limine.conf /mnt/boot/limine/
sudo cp limine/limine-bios.sys /mnt/boot/limine/
sudo cp limine/BOOTX64.EFI /mnt/EFI/BOOT/
sudo cp limine/BOOTIA32.EFI /mnt/EFI/BOOT/ || true
sudo umount /mnt

echo "[INFO] Burn disk.img into Partition 3 (Root)..."
sudo dd if=disk.img of=${LOOP_DEV}p3 bs=4M status=progress conv=fsync

echo "[INFO] Installing Limine MBR..."
sudo ./limine/limine bios-install $IMG

echo "[SUCCESS] Distribution image '$IMG' is ready!"
echo "[HINT] Write it to your USB stick with:"
echo "       sudo dd if=$IMG of=/dev/sda bs=4M status=progress conv=fsync"
