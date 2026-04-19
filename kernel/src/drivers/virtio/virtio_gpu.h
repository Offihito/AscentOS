#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include "drivers/virtio/virtio.h"
#include "drivers/virtio/virtio_pci.h"
#include <stdbool.h>
#include <stdint.h>

// ── VirtIO GPU Feature Bits (§5.7.3) ────────────────────────────────────────
#define VIRTIO_GPU_F_VIRGL 0 // 3D support (not used)
#define VIRTIO_GPU_F_EDID 1  // EDID support

// ── VirtIO GPU Command Types (§5.7.6.7) ─────────────────────────────────────
// 2D commands
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF 0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT 0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH 0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO 0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET 0x0109
#define VIRTIO_GPU_CMD_GET_EDID 0x010A

// Cursor commands
#define VIRTIO_GPU_CMD_UPDATE_CURSOR 0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR 0x0301

// Response types (success)
#define VIRTIO_GPU_RESP_OK_NODATA 0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO 0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET 0x1103
#define VIRTIO_GPU_RESP_OK_EDID 0x1104

// Response types (error)
#define VIRTIO_GPU_RESP_ERR_UNSPEC 0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY 0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID 0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID 0x1204
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER 0x1205

// ── Pixel formats ───────────────────────────────────────────────────────────
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM 3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM 4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM 67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM 68
#define VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM 121
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM 134

// ── Maximum display outputs ─────────────────────────────────────────────────
#define VIRTIO_GPU_MAX_SCANOUTS 16

// ── Control Header (§5.7.6.7) ───────────────────────────────────────────────
struct virtio_gpu_ctrl_hdr {
  uint32_t type;
  uint32_t flags;
  uint64_t fence_id;
  uint32_t ctx_id;
  uint8_t ring_idx;
  uint8_t padding[3];
} __attribute__((packed));

// ── Rectangle ───────────────────────────────────────────────────────────────
struct virtio_gpu_rect {
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;
} __attribute__((packed));

// ── GET_DISPLAY_INFO response ───────────────────────────────────────────────
struct virtio_gpu_display_one {
  struct virtio_gpu_rect r;
  uint32_t enabled;
  uint32_t flags;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));

// ── RESOURCE_CREATE_2D ──────────────────────────────────────────────────────
struct virtio_gpu_resource_create_2d {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t format;
  uint32_t width;
  uint32_t height;
} __attribute__((packed));

// ── RESOURCE_ATTACH_BACKING ─────────────────────────────────────────────────
struct virtio_gpu_mem_entry {
  uint64_t addr; // Guest-physical address
  uint32_t length;
  uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t nr_entries;
  // Followed by nr_entries × virtio_gpu_mem_entry
} __attribute__((packed));

// ── SET_SCANOUT ─────────────────────────────────────────────────────────────
struct virtio_gpu_set_scanout {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect r;
  uint32_t scanout_id;
  uint32_t resource_id;
} __attribute__((packed));

// ── TRANSFER_TO_HOST_2D ─────────────────────────────────────────────────────
struct virtio_gpu_transfer_to_host_2d {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect r;
  uint64_t offset;
  uint32_t resource_id;
  uint32_t padding;
} __attribute__((packed));

// ── RESOURCE_FLUSH ──────────────────────────────────────────────────────────
struct virtio_gpu_resource_flush {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect r;
  uint32_t resource_id;
  uint32_t padding;
} __attribute__((packed));

// ── VirtIO GPU Device State ─────────────────────────────────────────────────
struct virtio_gpu_device {
  struct virtio_pci_device vdev;
  struct virtqueue controlq;
  struct virtqueue cursorq;

  // Display info
  uint32_t width;
  uint32_t height;
  uint32_t scanout_id;

  // Resource tracking
  uint32_t next_resource_id;

  // Is the device initialized?
  bool initialized;
};

// ── Public API ──────────────────────────────────────────────────────────────

// Initialize the VirtIO GPU driver. Returns true if a device was found
// and successfully initialized.
bool virtio_gpu_init(void);

// Get display dimensions (valid after init).
uint32_t virtio_gpu_get_width(void);
uint32_t virtio_gpu_get_height(void);

// Check if the VirtIO GPU is available and initialized.
bool virtio_gpu_is_available(void);

// Run Phase 2 self-tests (device discovery, queue setup, display info).
void virtio_gpu_self_test(void);

#endif
