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

# 2. Extract rootfs to a temporary location if not already present
ROOTFS_DIR="${BUILD_DIR}/rootfs"
if [ ! -d "${ROOTFS_DIR}/etc" ]; then
    echo "[*] extracting Alpine rootfs to ${ROOTFS_DIR}..."
    mkdir -p "${ROOTFS_DIR}"
    tar -xzf "${BUILD_DIR}/${ALPINE_TARBALL}" -C "${ROOTFS_DIR}"
fi

# 3. Helper to download and install Alpine packages manually
install_apk() {
    local PKG_NAME=$1
    local REPO=$2
    
    if [ -f "${ROOTFS_DIR}/etc/ascentos-pkg/${PKG_NAME}" ]; then
        echo "[*] Package ${PKG_NAME} already installed, skipping."
        return 0
    fi

    echo "[*] Installing package: ${PKG_NAME} from ${REPO}..."
    
    # Escape dots and pluses in PKG_NAME for grep
    local ESCAPED_PKG_NAME=$(echo "${PKG_NAME}" | sed 's/\./\\./g;s/+/\\+/g')
    local APK_FILENAME=$(curl -sL "https://dl-cdn.alpinelinux.org/alpine/v3.21/${REPO}/x86_64/" | grep -oP ">${ESCAPED_PKG_NAME}-[0-9][^<]*\.apk<" | sed 's/>//;s/<//' | sort -V | tail -n 1)
    
    # Fallback to a known version if the search fails
    if [ -z "${APK_FILENAME}" ] && [ "${PKG_NAME}" == "st" ]; then
        APK_FILENAME="st-0.9.2-r0.apk"
    fi
    
    if [ -z "${APK_FILENAME}" ]; then
        echo "[!] Could not find package ${PKG_NAME} in ${REPO}"
        return 1
    fi
    
    local APK_URL="https://dl-cdn.alpinelinux.org/alpine/v3.21/${REPO}/x86_64/${APK_FILENAME}"
    
    if [ ! -f "${BUILD_DIR}/${APK_FILENAME}" ]; then
        echo "[*] Downloading ${APK_URL}..."
        curl -L "${APK_URL}" -o "${BUILD_DIR}/${APK_FILENAME}"
    fi
    
    # APK files are 3 concatenated gzip streams (signature + control + data).
    # tar --ignore-zeros -xz processes all streams in the concatenation.
    tar --ignore-zeros -xzf "${BUILD_DIR}/${APK_FILENAME}" -C "${ROOTFS_DIR}" --warning=no-unknown-keyword 2>/dev/null || true
    
    # Mark as installed
    mkdir -p "${ROOTFS_DIR}/etc/ascentos-pkg"
    touch "${ROOTFS_DIR}/etc/ascentos-pkg/${PKG_NAME}"
}

# Install st terminal and its dependencies
install_apk "st" "community"
install_apk "libxft" "main"
install_apk "fontconfig" "main"
install_apk "libxrender" "main"
install_apk "libexpat" "main"
install_apk "libuuid" "main"
install_apk "libpng" "main"
install_apk "freetype" "main"
install_apk "libx11" "main"
install_apk "libxcb" "main"
install_apk "libxau" "main"
install_apk "libxdmcp" "main"
install_apk "libbsd" "main"
install_apk "libmd" "main"
install_apk "ncurses-terminfo-base" "main"
install_apk "font-liberation" "main"
install_apk "libbz2" "main"
install_apk "brotli-libs" "main"
install_apk "zlib" "main"
install_apk "libxcursor" "main"
install_apk "libxfixes" "main"
install_apk "libxrender" "main"
install_apk "libxft" "main"
install_apk "fastfetch" "community"
install_apk "hwdata-pci" "main"

# Minimal GTK (GTK 2.0) and core dependencies
echo "[*] Installing GTK 2.0 and core dependencies..."
install_apk "gtk+2.0" "community"
install_apk "glib" "main"
install_apk "pango" "main"
install_apk "libatk-1.0" "main"
install_apk "gdk-pixbuf" "main"
install_apk "cairo" "main"
install_apk "fribidi" "main"
install_apk "harfbuzz" "main"
install_apk "libx11" "main"
install_apk "libxext" "main"
install_apk "libxrender" "main"
install_apk "libxi" "main"
install_apk "libxcursor" "main"
install_apk "libxfixes" "main"
install_apk "fontconfig" "main"
install_apk "freetype" "main"
install_apk "libpng" "main"
install_apk "libexpat" "main"
install_apk "libuuid" "main"
install_apk "shared-mime-info" "main"
install_apk "pcre2" "main"
install_apk "libffi" "main"
install_apk "pixman" "main"
install_apk "libjpeg-turbo" "main"
install_apk "libmount" "main"
install_apk "libblkid" "main"
install_apk "libeconf" "main"
install_apk "libintl" "main"
install_apk "graphite2" "main"
install_apk "libxcomposite" "main"
install_apk "libxdamage" "main"
install_apk "gettext-libs" "main"
install_apk "libxrandr" "main"
install_apk "libxinerama" "main"
install_apk "util-linux" "main"
install_apk "libbz2" "main"
install_apk "brotli-libs" "main"
install_apk "zlib" "main"

# GTK2 Development headers (for host compilation)
echo "[*] Installing GTK 2.0 development packages..."
install_apk "gtk+2.0-dev" "community"
install_apk "glib-dev" "main"
install_apk "pango-dev" "main"
install_apk "harfbuzz-dev" "main"
install_apk "graphite2-dev" "main"
install_apk "libxcomposite-dev" "main"
install_apk "libxdamage-dev" "main"
install_apk "at-spi2-core-dev" "main"
install_apk "gdk-pixbuf-dev" "main"
install_apk "libjpeg-turbo-dev" "main"
install_apk "util-linux-dev" "main"
install_apk "libeconf-dev" "main"
install_apk "gettext-dev" "main"
install_apk "libxrandr-dev" "main"
install_apk "libxinerama-dev" "main"
install_apk "cairo-dev" "main"
install_apk "libx11-dev" "main"
install_apk "xorgproto" "main"

# 4. Inject custom binaries
echo "[*] Injecting custom binaries into rootfs..."
mkdir -p "${ROOTFS_DIR}/bin"
if [ -f "${ROOT_DIR}/userland/gtk_test.elf" ]; then
    cp "${ROOT_DIR}/userland/gtk_test.elf" "${ROOTFS_DIR}/bin/gtk_test"
    chmod +x "${ROOTFS_DIR}/bin/gtk_test"
fi

# 5. Inject into disk.img
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

echo "[SUCCESS] Alpine rootfs with GTK 2.0 and 'st' installed into ${DISK_IMG}"
