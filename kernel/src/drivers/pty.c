// ── PTY (Pseudoterminal) Driver ──────────────────────────────────────────────
// Implements POSIX-style PTY multiplexor (/dev/ptmx) and slave devices
// (/dev/pts/N) for terminal emulators and shell support.

#include "pty.h"
#include "../console/klog.h"
#include "../fb/framebuffer.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../sched/sched.h"
#include "../sched/wait.h"

// PTY pair pool
static pty_pair_t pty_pool[PTY_MAX_PAIRS];
static int pty_next_index = 0;

// ── Ring buffer helpers ──────────────────────────────────────────────────────

static inline uint32_t ring_used(uint32_t head, uint32_t tail) {
  return (head >= tail) ? (head - tail) : (PTY_BUFFER_SIZE - tail + head);
}

static inline uint32_t ring_free(uint32_t head, uint32_t tail) {
  return PTY_BUFFER_SIZE - ring_used(head, tail) - 1;
}

static inline bool ring_empty(uint32_t head, uint32_t tail) {
  return head == tail;
}

// Write to ring buffer, returns bytes written
static uint32_t ring_write(uint8_t *buffer, uint32_t *head, uint32_t tail,
                           const uint8_t *data, uint32_t len) {
  uint32_t free_space = ring_free(*head, tail);
  if (free_space == 0)
    return 0;
  
  uint32_t to_write = (len < free_space) ? len : free_space;
  
  for (uint32_t i = 0; i < to_write; i++) {
    buffer[*head] = data[i];
    *head = (*head + 1) % PTY_BUFFER_SIZE;
  }
  
  return to_write;
}

// Read from ring buffer, returns bytes read
static uint32_t ring_read(uint8_t *buffer, uint32_t head, uint32_t *tail,
                          uint8_t *data, uint32_t len) {
  uint32_t used = ring_used(head, *tail);
  if (used == 0)
    return 0;
  
  uint32_t to_read = (len < used) ? len : used;
  
  for (uint32_t i = 0; i < to_read; i++) {
    data[i] = buffer[*tail];
    *tail = (*tail + 1) % PTY_BUFFER_SIZE;
  }
  
  return to_read;
}

// ── PTY subsystem initialization ─────────────────────────────────────────────

void pty_init(void) {
  for (int i = 0; i < PTY_MAX_PAIRS; i++) {
    pty_pool[i].index = i;
    pty_pool[i].allocated = false;
    pty_pool[i].locked = false;
    pty_pool[i].m2s_head = 0;
    pty_pool[i].m2s_tail = 0;
    pty_pool[i].s2m_head = 0;
    pty_pool[i].s2m_tail = 0;
    pty_pool[i].pgid = 0;
    pty_pool[i].master_waitq = NULL;
    pty_pool[i].slave_waitq = NULL;
    
    // Default terminal settings (same as console)
    pty_pool[i].termios.c_iflag = 0x00000100; // ICRNL
    pty_pool[i].termios.c_oflag = 0x00000005; // OPOST | ONLCR
    pty_pool[i].termios.c_cflag = 0;
    pty_pool[i].termios.c_lflag = 0x0000000b; // ISIG | ICANON | ECHO
    pty_pool[i].termios.c_line = 0;
    for (int j = 0; j < NCCS; j++)
      pty_pool[i].termios.c_cc[j] = 0;
    
    // Default window size (80x24)
    pty_pool[i].winsize.ws_row = 24;
    pty_pool[i].winsize.ws_col = 80;
    pty_pool[i].winsize.ws_xpixel = 80 * 8;
    pty_pool[i].winsize.ws_ypixel = 24 * 16;
  }
  
  klog_puts("[PTY] Initialized with ");
  klog_uint64(PTY_MAX_PAIRS);
  klog_puts(" PTY pairs\n");
}

// ── PTY allocation ───────────────────────────────────────────────────────────

int pty_alloc_pair(void) {
  for (int i = 0; i < PTY_MAX_PAIRS; i++) {
    int idx = (pty_next_index + i) % PTY_MAX_PAIRS;
    if (!pty_pool[idx].allocated) {
      pty_pool[idx].allocated = true;
      pty_pool[idx].locked = false;
      pty_pool[idx].m2s_head = 0;
      pty_pool[idx].m2s_tail = 0;
      pty_pool[idx].s2m_head = 0;
      pty_pool[idx].s2m_tail = 0;
      pty_pool[idx].pgid = 0;
      
      // Reset terminal settings
      pty_pool[idx].termios.c_iflag = 0x00000100;
      pty_pool[idx].termios.c_oflag = 0x00000005;
      pty_pool[idx].termios.c_cflag = 0;
      pty_pool[idx].termios.c_lflag = 0x0000000b;
      
      pty_next_index = (idx + 1) % PTY_MAX_PAIRS;
      
      klog_puts("[PTY] Allocated PTY pair ");
      klog_uint64(idx);
      klog_puts("\n");
      
      return idx;
    }
  }
  
  klog_puts("[PTY] No free PTY pairs\n");
  return -1; // ENOSPC
}

pty_pair_t *pty_get_pair(int index) {
  if (index < 0 || index >= PTY_MAX_PAIRS)
    return NULL;
  if (!pty_pool[index].allocated)
    return NULL;
  return &pty_pool[index];
}

// ── Data availability checks ────────────────────────────────────────────────

bool pty_master_can_read(pty_pair_t *pty) {
  return !ring_empty(pty->s2m_head, pty->s2m_tail);
}

bool pty_slave_can_read(pty_pair_t *pty) {
  return !ring_empty(pty->m2s_head, pty->m2s_tail);
}

// ── /dev/ptmx (master) VFS operations ────────────────────────────────────────

uint32_t ptmx_read(struct vfs_node *node, uint32_t offset, uint32_t size,
                   uint8_t *buffer) {
  (void)offset;
  
  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return 0;
  
  // Master reads from slave→master buffer
  return ring_read(pty->slave_to_master, pty->s2m_head, &pty->s2m_tail,
                   buffer, size);
}

uint32_t ptmx_write(struct vfs_node *node, uint32_t offset, uint32_t size,
                    uint8_t *buffer) {
  (void)offset;
  
  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return 0;
  
  // Master writes to master→slave buffer
  uint32_t written = ring_write(pty->master_to_slave, &pty->m2s_head,
                                pty->m2s_tail, buffer, size);
  
  // Wake up any readers waiting on slave
  if (written > 0 && pty->slave_waitq) {
    wait_queue_wake_all(pty->slave_waitq);
  }
  
  return written;
}

int ptmx_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return -9; // EBADF
  
  switch (request) {
  case TIOCGPTN: {
    // Get PTY number
    int *ptn = (int *)arg;
    if (!ptn)
      return -14; // EFAULT
    *ptn = pty->index;
    return 0;
  }
  
  case TIOCSPTLCK: {
    // Lock/unlock PTY
    int *lock = (int *)arg;
    if (!lock)
      return -14;
    pty->locked = (*lock != 0);
    return 0;
  }
  
  case TIOCGPTLCK: {
    // Get lock state
    int *lock = (int *)arg;
    if (!lock)
      return -14;
    *lock = pty->locked ? 1 : 0;
    return 0;
  }
  
  case TIOCSIG: {
    // Send signal to slave's foreground process group
    int sig = (int)arg;
    if (sig <= 0 || sig > 64)
      return -22; // EINVAL
    
    if (pty->pgid != 0) {
      // Send signal to process group
      extern void signal_send_pgid(uint32_t pgid, int sig);
      signal_send_pgid(pty->pgid, sig);
    }
    return 0;
  }
  
  case TIOCGWINSZ: {
    struct winsize *ws = (struct winsize *)arg;
    if (!ws)
      return -14;
    *ws = pty->winsize;
    return 0;
  }
  
  case TIOCSWINSZ: {
    const struct winsize *ws = (const struct winsize *)arg;
    if (!ws)
      return -14;
    pty->winsize = *ws;
    
    // Send SIGWINCH to slave's foreground process group
    if (pty->pgid != 0) {
      extern void signal_send_pgid(uint32_t pgid, int sig);
      signal_send_pgid(pty->pgid, 28); // SIGWINCH
    }
    return 0;
  }
  
  default:
    return -25; // ENOTTY
  }
}

int ptmx_poll(struct vfs_node *node, int events) {
  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return 0;
  
  int revents = 0;
  
  if (events & POLLIN) {
    if (pty_master_can_read(pty)) {
      revents |= POLLIN;
    }
  }
  
  if (events & POLLOUT) {
    // Always ready to write (buffer permitting)
    revents |= POLLOUT;
  }
  
  return revents;
}

// ── /dev/pts/N (slave) VFS operations ────────────────────────────────────────

uint32_t pty_slave_read(struct vfs_node *node, uint32_t offset, uint32_t size,
                        uint8_t *buffer) {
  (void)offset;
  
  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return 0;
  
  // Slave reads from master→slave buffer
  // TODO: Implement canonical mode processing (line editing)
  // For now, raw mode only
  return ring_read(pty->master_to_slave, pty->m2s_head, &pty->m2s_tail,
                   buffer, size);
}

uint32_t pty_slave_write(struct vfs_node *node, uint32_t offset, uint32_t size,
                         uint8_t *buffer) {
  (void)offset;
  
  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return 0;
  
  // Slave writes to slave→master buffer
  // Apply output processing (ONLCR: convert \n to \r\n)
  uint32_t written = 0;
  
  if (pty->termios.c_oflag & ONLCR) {
    // Process each character
    for (uint32_t i = 0; i < size; i++) {
      if (buffer[i] == '\n') {
        // Write \r first
        if (ring_write(pty->slave_to_master, &pty->s2m_head, pty->s2m_tail,
                       (const uint8_t *)"\r", 1) == 0) {
          break;
        }
      }
      if (ring_write(pty->slave_to_master, &pty->s2m_head, pty->s2m_tail,
                     &buffer[i], 1) == 0) {
        break;
      }
      written++;
    }
  } else {
    written = ring_write(pty->slave_to_master, &pty->s2m_head, pty->s2m_tail,
                         buffer, size);
  }
  
  // Wake up any readers waiting on master
  if (written > 0 && pty->master_waitq) {
    wait_queue_wake_all(pty->master_waitq);
  }
  
  return written;
}

int pty_slave_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return -9; // EBADF
  
  switch (request) {
  case TCGETS: {
    struct termios *term = (struct termios *)arg;
    if (!term)
      return -14;
    *term = pty->termios;
    return 0;
  }
  
  case TCSETS:
  case TCSETSW:
  case TCSETSF: {
    const struct termios *term = (const struct termios *)arg;
    if (!term)
      return -14;
    pty->termios = *term;
    return 0;
  }
  
  case TIOCGWINSZ: {
    struct winsize *ws = (struct winsize *)arg;
    if (!ws)
      return -14;
    *ws = pty->winsize;
    return 0;
  }
  
  case TIOCSWINSZ: {
    const struct winsize *ws = (const struct winsize *)arg;
    if (!ws)
      return -14;
    pty->winsize = *ws;
    // Note: SIGWINCH is sent to the foreground process group
    return 0;
  }
  
  case TIOCGPGRP: {
    // Get foreground process group
    int *pgrp = (int *)arg;
    if (!pgrp)
      return -14;
    *pgrp = (int)pty->pgid;
    return 0;
  }
  
  case TIOCSPGRP: {
    // Set foreground process group
    int *pgrp = (int *)arg;
    if (!pgrp)
      return -14;
    pty->pgid = (uint32_t)*pgrp;
    return 0;
  }
  
  default:
    return -25; // ENOTTY
  }
}

int pty_slave_poll(struct vfs_node *node, int events) {
  pty_pair_t *pty = (pty_pair_t *)node->device;
  if (!pty)
    return 0;
  
  int revents = 0;
  
  if (events & POLLIN) {
    if (pty_slave_can_read(pty)) {
      revents |= POLLIN;
    }
  }
  
  if (events & POLLOUT) {
    revents |= POLLOUT;
  }
  
  return revents;
}

// ── Device registration ─────────────────────────────────────────────────────

void pty_register_devices(void) {
  pty_init();
  
  // /dev/ptmx - PTY master multiplexor
  // This is a special device: each open creates a new PTY pair
  // The device pointer will be set dynamically on open
  vfs_node_t *ptmx_node = kmalloc(sizeof(vfs_node_t));
  if (!ptmx_node)
    return;
  
  memset(ptmx_node, 0, sizeof(vfs_node_t));
  strcpy(ptmx_node->name, "ptmx");
  ptmx_node->flags = FS_CHARDEV;
  ptmx_node->mask = 0666;
  ptmx_node->read = ptmx_read;
  ptmx_node->write = ptmx_write;
  ptmx_node->ioctl = ptmx_ioctl;
  ptmx_node->poll = ptmx_poll;
  ptmx_node->device = NULL; // Set on open
  
  fb_register_device_node("ptmx", ptmx_node);
  
  klog_puts("[PTY] Registered /dev/ptmx\n");
}
