#!/usr/bin/env bash
# scripts/setup-alpine.sh - Downloads and installs Alpine Linux rootfs into AscentOS disk image
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISK_IMG="${ROOT_DIR}/disk.img"
BUILD_DIR="${ROOT_DIR}/build/alpine"
POPULATE_SCRIPT="${ROOT_DIR}/scripts/populate-ext2-dir.sh"

ALPINE_VERSION="3.21.0"
ALPINE_TARBALL="alpine-minirootfs-${ALPINE_VERSION}-x86_64.tar.gz"
ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/x86_64/${ALPINE_TARBALL}"

mkdir -p "${BUILD_DIR}"

# 1. Download Alpine rootfs if not present
if [ ! -f "${BUILD_DIR}/${ALPINE_TARBALL}" ]; then
    echo "[*] Downloading Alpine ${ALPINE_VERSION}..."
    curl -L "${ALPINE_URL}" -o "${BUILD_DIR}/${ALPINE_TARBALL}"
fi

# 2. Extract rootfs to a temporary location
ROOTFS_DIR="${BUILD_DIR}/rootfs"
echo "[*] extracting Alpine rootfs to ${ROOTFS_DIR}..."
rm -rf "${ROOTFS_DIR}"
mkdir -p "${ROOTFS_DIR}"
tar -xzf "${BUILD_DIR}/${ALPINE_TARBALL}" -C "${ROOTFS_DIR}"

# 3. Helper to download and install Alpine packages manually
install_apk() {
    local PKG_NAME=$1
    local REPO=$2
    echo "[*] Installing package: ${PKG_NAME} from ${REPO}..."
    
    # Stricter grep to find the exact package filename
    local APK_FILENAME=$(curl -sL "https://dl-cdn.alpinelinux.org/alpine/v3.21/${REPO}/x86_64/" | grep -oP ">${PKG_NAME}-\d+[^<]*\.apk<" | sed 's/>//;s/<//' | sort -V | tail -n 1)
    
    # Fallback to a known version if the search fails
    if [ -z "${APK_FILENAME}" ] && [ "${PKG_NAME}" == "st" ]; then
        APK_FILENAME="st-0.9.2-r0.apk"
    fi
    
    if [ -z "${APK_FILENAME}" ]; then
        echo "[!] Could not find package ${PKG_NAME} in ${REPO}"
        return 1
    fi
    
    local APK_URL="https://dl-cdn.alpinelinux.org/alpine/v3.21/${REPO}/x86_64/${APK_FILENAME}"
    echo "[*] Downloading ${APK_URL}..."
    curl -L "${APK_URL}" -o "${BUILD_DIR}/${APK_FILENAME}"
    
    # APK files are multi-part tarballs. cat | tar ensures all streams are processed.
    cat "${BUILD_DIR}/${APK_FILENAME}" | tar -xz -C "${ROOTFS_DIR}" --warning=no-unknown-keyword || true
}

# Install st terminal (usually in community)
install_apk "st" "community"

# 4. Inject into disk.img
if [ ! -f "${DISK_IMG}" ]; then
    echo "[!] disk.img not found. Please run 'make disk.img' first."
    exit 1
fi

echo "[*] Extracting partition 1 from disk.img..."
PART_IMG="${BUILD_DIR}/part1.img"
dd if="${DISK_IMG}" of="${PART_IMG}" bs=1M skip=1 status=none

echo "[*] Populating partition with Alpine rootfs (using debugfs)..."
"${POPULATE_SCRIPT}" "${PART_IMG}" "${ROOTFS_DIR}" "/"

echo "[*] Re-injecting partition 1 into disk.img..."
dd if="${PART_IMG}" of="${DISK_IMG}" bs=1M seek=1 conv=notrunc status=none

echo "[SUCCESS] Alpine rootfs with 'st' installed into ${DISK_IMG}"
