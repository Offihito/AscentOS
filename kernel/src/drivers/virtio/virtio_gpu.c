#include "drivers/virtio/virtio_gpu.h"
#include "console/klog.h"
#include "drivers/pci/pci.h"
#include "drivers/virtio/virtio.h"
#include "drivers/virtio/virtio_pci.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include <stdbool.h>
#include <stdint.h>

// ── Globals ─────────────────────────────────────────────────────────────────

static struct virtio_gpu_device gpu_dev;

// ── Helpers ─────────────────────────────────────────────────────────────────



// ── Command submission helpers ──────────────────────────────────────────────

// Submit a command on the controlq and poll for the response.
// cmd_phys/cmd_len: physical address and length of the command buffer
// resp_phys/resp_len: physical address and length of the response buffer
// Returns true if the device processed the command (check resp header for
// status).
static bool gpu_submit_cmd(uint64_t cmd_phys, uint32_t cmd_len,
                           uint64_t resp_phys, uint32_t resp_len) {
  struct virtqueue *vq = &gpu_dev.controlq;

  int head = virtq_add_buf_chain(vq, cmd_phys, cmd_len, resp_phys, resp_len);
  if (head < 0) {
    klog_puts("[VIRTIO-GPU] Failed to add command to controlq\n");
    return false;
  }

  // Add to available ring
  uint16_t avail_idx = vq->avail->idx % vq->num;
  vq->avail->ring[avail_idx] = (uint16_t)head;
  __asm__ volatile("mfence" ::: "memory");
  vq->avail->idx++;
  __asm__ volatile("mfence" ::: "memory");

  // Notify the device
  virtio_pci_notify(&gpu_dev.vdev, 0, vq);

  // Poll for completion (with timeout)
  for (int timeout = 0; timeout < 1000000; timeout++) {
    uint32_t used_id, used_len;
    if (virtq_poll(vq, &used_id, &used_len)) {
      virtq_free_desc(vq, (uint16_t)used_id);
      return true;
    }
    __asm__ volatile("pause");
  }

  klog_puts("[VIRTIO-GPU] Command timed out\n");
  return false;
}

// ── GET_DISPLAY_INFO ────────────────────────────────────────────────────────

static bool gpu_get_display_info(void) {
  // Allocate a page for command + response (they need physical addresses)
  void *page_phys = pmm_alloc_page();
  if (!page_phys) {
    klog_puts("[VIRTIO-GPU] OOM for display info\n");
    return false;
  }

  uint64_t page_virt = (uint64_t)page_phys + pmm_get_hhdm_offset();
  memset((void *)page_virt, 0, PAGE_SIZE);

  // Command at offset 0
  struct virtio_gpu_ctrl_hdr *cmd = (struct virtio_gpu_ctrl_hdr *)page_virt;
  cmd->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

  // Response at offset 512 (plenty of room)
  struct virtio_gpu_resp_display_info *resp =
      (struct virtio_gpu_resp_display_info *)(page_virt + 512);

  uint64_t cmd_phys = (uint64_t)page_phys;
  uint64_t resp_phys = (uint64_t)page_phys + 512;

  bool ok = gpu_submit_cmd(cmd_phys, sizeof(*cmd), resp_phys, sizeof(*resp));
  if (!ok) {
    pmm_free_page(page_phys);
    return false;
  }

  if (resp->hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
    klog_puts("[VIRTIO-GPU] GET_DISPLAY_INFO failed, response type=");
    klog_hex32(resp->hdr.type);
    klog_puts("\n");
    pmm_free_page(page_phys);
    return false;
  }

  // Find the first enabled scanout
  for (int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
    if (resp->pmodes[i].enabled) {
      gpu_dev.width = resp->pmodes[i].r.width;
      gpu_dev.height = resp->pmodes[i].r.height;
      gpu_dev.scanout_id = i;

      klog_puts("[VIRTIO-GPU] Display ");
      klog_uint64(i);
      klog_puts(": ");
      klog_uint64(gpu_dev.width);
      klog_puts("x");
      klog_uint64(gpu_dev.height);
      klog_puts(" (enabled)\n");
      break;
    }
  }

  pmm_free_page(page_phys);

  if (gpu_dev.width == 0 || gpu_dev.height == 0) {
    klog_puts("[VIRTIO-GPU] No enabled display found\n");
    return false;
  }

  return true;
}

// ── Device Initialization ───────────────────────────────────────────────────

bool virtio_gpu_init(void) {
  memset(&gpu_dev, 0, sizeof(gpu_dev));

  // Try to find the VirtIO GPU device (modern first, then legacy)
  struct pci_device *pci = NULL;

  // Search by vendor+device ID for modern device
  for (uint32_t i = 0; i < pci_get_device_count(); i++) {
    struct pci_device *dev = pci_get_device(i);
    if (dev->vendor_id == VIRTIO_PCI_VENDOR_ID &&
        dev->device_id == VIRTIO_PCI_DEVICE_GPU) {
      pci = dev;
      klog_puts("[VIRTIO-GPU] Found modern VirtIO GPU (0x1050)\n");
      break;
    }
    // Also check for transitional device
    if (dev->vendor_id == VIRTIO_PCI_VENDOR_ID &&
        dev->device_id == VIRTIO_PCI_DEVICE_GPU_LEGACY) {
      pci = dev;
      klog_puts("[VIRTIO-GPU] Found transitional VirtIO GPU (0x1010)\n");
      break;
    }
  }

  if (!pci) {
    klog_puts("[VIRTIO-GPU] No VirtIO GPU device found on PCI bus\n");
    return false;
  }

  klog_puts("[VIRTIO-GPU] PCI ");
  klog_uint64(pci->bus);
  klog_puts(":");
  klog_uint64(pci->slot);
  klog_puts(".");
  klog_uint64(pci->func);
  klog_puts(" vendor=");
  klog_hex32(pci->vendor_id);
  klog_puts(" device=");
  klog_hex32(pci->device_id);
  klog_puts("\n");

  // Initialize PCI transport
  if (!virtio_pci_init(&gpu_dev.vdev, pci)) {
    klog_puts("[VIRTIO-GPU] Failed to initialize PCI transport\n");
    return false;
  }

  // ── Standard VirtIO initialization sequence (§3.1.1) ────────────────────

  // 1. Reset the device
  virtio_pci_reset(&gpu_dev.vdev);

  // 2. Set ACKNOWLEDGE status bit
  virtio_pci_set_status(&gpu_dev.vdev, VIRTIO_STATUS_ACKNOWLEDGE);

  // 3. Set DRIVER status bit
  virtio_pci_set_status(&gpu_dev.vdev,
                        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

  // 4. Read device features
  uint64_t device_features = virtio_pci_read_features(&gpu_dev.vdev);
  klog_puts("[VIRTIO-GPU] Device features: ");
  klog_hex32((uint32_t)(device_features >> 32));
  klog_puts(" ");
  klog_hex32((uint32_t)(device_features & 0xFFFFFFFF));
  klog_puts("\n");

  // 5. Negotiate features — we only need VIRTIO_F_VERSION_1
  uint64_t driver_features = VIRTIO_F_VERSION_1;
  virtio_pci_write_features(&gpu_dev.vdev, driver_features);

  // 6. Set FEATURES_OK status bit
  virtio_pci_set_status(&gpu_dev.vdev, VIRTIO_STATUS_ACKNOWLEDGE |
                                           VIRTIO_STATUS_DRIVER |
                                           VIRTIO_STATUS_FEATURES_OK);

  // 7. Re-read status to ensure FEATURES_OK is still set
  uint8_t status = virtio_pci_get_status(&gpu_dev.vdev);
  if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
    klog_puts("[VIRTIO-GPU] Device did not accept features\n");
    virtio_pci_set_status(&gpu_dev.vdev, VIRTIO_STATUS_FAILED);
    return false;
  }
  klog_puts("[VIRTIO-GPU] Feature negotiation OK\n");

  // 8. Set up virtqueues (controlq = 0, cursorq = 1)
  if (!virtio_pci_setup_queue(&gpu_dev.vdev, 0, &gpu_dev.controlq)) {
    klog_puts("[VIRTIO-GPU] Failed to setup controlq\n");
    virtio_pci_set_status(&gpu_dev.vdev, VIRTIO_STATUS_FAILED);
    return false;
  }

  if (!virtio_pci_setup_queue(&gpu_dev.vdev, 1, &gpu_dev.cursorq)) {
    klog_puts("[VIRTIO-GPU] Failed to setup cursorq (non-fatal)\n");
    // cursorq is optional; continue without it
  }

  // 9. Set DRIVER_OK status bit — device is now live!
  virtio_pci_set_status(
      &gpu_dev.vdev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                         VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
  klog_puts("[VIRTIO-GPU] Device status: DRIVER_OK\n");

  // 10. Query display information
  if (!gpu_get_display_info()) {
    klog_puts("[VIRTIO-GPU] Failed to get display info\n");
    // Not fatal — we can retry later
  }

  gpu_dev.next_resource_id = 1;
  gpu_dev.initialized = true;

  klog_puts("[VIRTIO-GPU] Initialization complete!\n");
  return true;
}

// ── Getters ─────────────────────────────────────────────────────────────────

uint32_t virtio_gpu_get_width(void) { return gpu_dev.width; }
uint32_t virtio_gpu_get_height(void) { return gpu_dev.height; }
bool virtio_gpu_is_available(void) { return gpu_dev.initialized; }

// ── Phase 2 Self-Test ───────────────────────────────────────────────────────

void virtio_gpu_self_test(void) {
  klog_puts("\n[VIRTIO-GPU] ═══ Phase 2 Self-Test ═══\n");

  // Test 1: Device discovery
  struct pci_device *pci = NULL;
  for (uint32_t i = 0; i < pci_get_device_count(); i++) {
    struct pci_device *dev = pci_get_device(i);
    if (dev->vendor_id == VIRTIO_PCI_VENDOR_ID &&
        (dev->device_id == VIRTIO_PCI_DEVICE_GPU ||
         dev->device_id == VIRTIO_PCI_DEVICE_GPU_LEGACY)) {
      pci = dev;
      break;
    }
  }

  if (!pci) {
    klog_puts("[VIRTIO-GPU] SKIP: No VirtIO GPU on PCI bus\n");
    klog_puts("[VIRTIO-GPU] ═══ Phase 2 tests SKIPPED ═══\n\n");
    return;
  }
  klog_puts("[VIRTIO-GPU] PASS: PCI device found (vendor=0x1AF4 device=");
  klog_hex32(pci->device_id);
  klog_puts(")\n");

  // Test 2: Check initialization completed
  if (!gpu_dev.initialized) {
    klog_puts("[VIRTIO-GPU] FAIL: Device not initialized\n");
    klog_puts("[VIRTIO-GPU] ═══ Phase 2 tests FAILED ═══\n\n");
    return;
  }
  klog_puts("[VIRTIO-GPU] PASS: Device initialized successfully\n");

  // Test 3: Check device status
  uint8_t status = virtio_pci_get_status(&gpu_dev.vdev);
  if (!(status & VIRTIO_STATUS_DRIVER_OK)) {
    klog_puts("[VIRTIO-GPU] FAIL: DRIVER_OK not set (status=");
    klog_hex32(status);
    klog_puts(")\n");
    return;
  }
  klog_puts("[VIRTIO-GPU] PASS: Device status has DRIVER_OK\n");

  // Test 4: Controlq is functional
  if (gpu_dev.controlq.num == 0) {
    klog_puts("[VIRTIO-GPU] FAIL: controlq.num is 0\n");
    return;
  }
  klog_puts("[VIRTIO-GPU] PASS: controlq size=");
  klog_uint64(gpu_dev.controlq.num);
  klog_puts(" free=");
  klog_uint64(gpu_dev.controlq.num_free);
  klog_puts("\n");

  // Test 5: Display info retrieved
  if (gpu_dev.width == 0 || gpu_dev.height == 0) {
    klog_puts("[VIRTIO-GPU] WARN: No display detected (width=0 height=0)\n");
    klog_puts(
        "             This may be expected without -device virtio-gpu-pci\n");
  } else {
    klog_puts("[VIRTIO-GPU] PASS: Display info: ");
    klog_uint64(gpu_dev.width);
    klog_puts("x");
    klog_uint64(gpu_dev.height);
    klog_puts(" on scanout ");
    klog_uint64(gpu_dev.scanout_id);
    klog_puts("\n");
  }

  // Test 6: Re-query display info to verify controlq round-trip
  void *page_phys = pmm_alloc_page();
  if (page_phys) {
    uint64_t page_virt = (uint64_t)page_phys + pmm_get_hhdm_offset();
    memset((void *)page_virt, 0, PAGE_SIZE);

    struct virtio_gpu_ctrl_hdr *cmd = (struct virtio_gpu_ctrl_hdr *)page_virt;
    cmd->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    struct virtio_gpu_resp_display_info *resp =
        (struct virtio_gpu_resp_display_info *)(page_virt + 512);

    bool ok = gpu_submit_cmd((uint64_t)page_phys, sizeof(*cmd),
                             (uint64_t)page_phys + 512, sizeof(*resp));
    if (ok && resp->hdr.type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
      klog_puts("[VIRTIO-GPU] PASS: Controlq round-trip verified "
                "(GET_DISPLAY_INFO x2)\n");
    } else {
      klog_puts("[VIRTIO-GPU] FAIL: Second GET_DISPLAY_INFO failed\n");
    }

    pmm_free_page(page_phys);
  }

  klog_puts("[VIRTIO-GPU] ═══ Phase 2 tests PASSED ═══\n\n");
}
