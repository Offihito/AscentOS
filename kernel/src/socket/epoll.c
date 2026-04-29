// ── Epoll Implementation
// ─────────────────────────────────────── Phase 8: epoll Infrastructure
// Implements the epoll API for event multiplexing

#include "epoll.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "socket.h"
#include <stdint.h>

// ── Global Epoll Instance Table
// ───────────────────────────────────────────────────────
static eventpoll_t *epoll_table[EPOLL_MAX_INSTANCES];
static int epoll_count = 0;
static spinlock_t epoll_table_lock = SPINLOCK_INIT;

// ── Epoll Instance Table Management
// ────────────────────────────────────────────────────────

static int epoll_table_alloc(void) {
  spinlock_acquire(&epoll_table_lock);
  
  for (int i = 0; i < EPOLL_MAX_INSTANCES; i++) {
    if (epoll_table[i] == NULL) {
      epoll_count++;
      spinlock_release(&epoll_table_lock);
      return i;
    }
  }
  
  spinlock_release(&epoll_table_lock);
  return -1; // Table full
}

static void epoll_table_free(int idx) {
  if (idx < 0 || idx >= EPOLL_MAX_INSTANCES)
    return;
  
  spinlock_acquire(&epoll_table_lock);
  epoll_table[idx] = NULL;
  epoll_count--;
  spinlock_release(&epoll_table_lock);
}

static eventpoll_t *epoll_table_get(int idx) {
  if (idx < 0 || idx >= EPOLL_MAX_INSTANCES)
    return NULL;
  
  spinlock_acquire(&epoll_table_lock);
  eventpoll_t *ep = epoll_table[idx];
  spinlock_release(&epoll_table_lock);
  return ep;
}

// ── Epoll Item Management
// ───────────────────────────────────────────────────────

static epitem_t *epitem_alloc(void) {
  epitem_t *epi = kmalloc(sizeof(epitem_t));
  if (!epi)
    return NULL;
  
  memset(epi, 0, sizeof(epitem_t));
  INIT_LIST_HEAD(&epi->rdllink);
  INIT_LIST_HEAD(&epi->fllink);
  spinlock_init(&epi->lock);
  return epi;
}

static void epitem_free(epitem_t *epi) {
  if (!epi)
    return;
  kfree(epi);
}

// ── Epoll Instance Creation/Destruction
// ─────────────────────────────────────────────────────────

eventpoll_t *epoll_create(void) {
  // Allocate epoll structure
  eventpoll_t *ep = kmalloc(sizeof(eventpoll_t));
  if (!ep) {
    klog_puts("[ERR] epoll: failed to allocate eventpoll structure\n");
    return NULL;
  }
  
  memset(ep, 0, sizeof(eventpoll_t));
  
  // Allocate table slot
  int idx = epoll_table_alloc();
  if (idx < 0) {
    kfree(ep);
    klog_puts("[ERR] epoll: epoll table full\n");
    return NULL;
  }
  
  // Initialize
  ep->fd = -1;
  ep->item_count = 0;
  INIT_LIST_HEAD(&ep->rdllist);
  ep->rdllist_count = 0;
  wait_queue_init(&ep->wq);
  spinlock_init(&ep->lock);
  ep->refcount = 1;
  
  epoll_table[idx] = ep;
  
  return ep;
}

void epoll_destroy(eventpoll_t *ep) {
  if (!ep)
    return;
  
  spinlock_acquire(&ep->lock);
  
  // Free all watched items
  for (int i = 0; i < EPOLL_MAX_WATCHED; i++) {
    if (ep->items[i]) {
      epitem_free(ep->items[i]);
      ep->items[i] = NULL;
    }
  }
  
  spinlock_release(&ep->lock);
  
  // Remove from table
  for (int i = 0; i < EPOLL_MAX_INSTANCES; i++) {
    if (epoll_table[i] == ep) {
      epoll_table_free(i);
      break;
    }
  }
  
  kfree(ep);
}

void epoll_get(eventpoll_t *ep) {
  if (!ep)
    return;
  __atomic_fetch_add(&ep->refcount, 1, __ATOMIC_ACQ_REL);
}

void epoll_put(eventpoll_t *ep) {
  if (!ep)
    return;
  
  if (__atomic_fetch_sub(&ep->refcount, 1, __ATOMIC_ACQ_REL) == 1) {
    epoll_destroy(ep);
  }
}

// ── Helper: Check if FD has events
// ────────────────────────────────────────────────────────

static uint32_t ep_check_events(epitem_t *epi) {
  if (!epi || !epi->node)
    return 0;
  
  // Get the registered events, excluding modifier flags
  uint32_t watch_mask = epi->registered_events & ~(EPOLLET | EPOLLONESHOT | EPOLLEXCLUSIVE | EPOLLWAKEUP);
  
  // Call VFS poll to get current events
  int revents = vfs_poll(epi->node, watch_mask);
  
  // Mask to only requested events plus error/hangup
  return (uint32_t)revents & (watch_mask | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
}

// ── Helper: Add item to ready list
// ────────────────────────────────────────────────────────

static void ep_add_to_ready_list(eventpoll_t *ep, epitem_t *epi) {
  if (epi->on_ready_list)
    return;
  
  spinlock_acquire(&ep->lock);
  
  if (!epi->on_ready_list) {
    list_add_tail(&epi->rdllink, &ep->rdllist);
    epi->on_ready_list = true;
    ep->rdllist_count++;
  }
  
  spinlock_release(&ep->lock);
}

// ── Helper: Remove item from ready list
// ─────────────────────────────────────────────────────────────

static void ep_remove_from_ready_list(eventpoll_t *ep, epitem_t *epi) {
  if (!epi->on_ready_list)
    return;
  
  spinlock_acquire(&ep->lock);
  
  if (epi->on_ready_list) {
    list_del(&epi->rdllink);
    epi->on_ready_list = false;
    ep->rdllist_count--;
  }
  
  spinlock_release(&ep->lock);
}

// ── epoll_ctl Operations
// ───────────────────────────────────────────────────────

int epoll_ctl_add(eventpoll_t *ep, int fd, struct epoll_event *event) {
  if (!ep || !event)
    return -22; // EINVAL
  
  klog_puts("[EPOLL_CTL_ADD] fd=");
  klog_uint64(fd);
  klog_puts(" epoll_fd=");
  klog_uint64(ep->fd);
  klog_puts("\n");
  
  if (fd < 0 || fd >= EPOLL_MAX_WATCHED)
    return -9; // EBADF
  
  // Get current thread and VFS node for FD
  struct thread *t = sched_get_current();
  if (!t)
    return -1;
  
  if (fd >= MAX_FDS || !t->fds[fd])
    return -9; // EBADF
  
  vfs_node_t *node = t->fds[fd];
  
  // Check if already registered for the same node
  if (ep->items[fd]) {
    epitem_t *old = ep->items[fd];
    if (old->node == node) {
      return -17; // EEXIST - same socket already registered
    }
    // Different node - fd was reused, remove stale item
    ep_remove_from_ready_list(ep, old);
    spinlock_acquire(&ep->lock);
    ep->items[fd] = NULL;
    ep->item_count--;
    spinlock_release(&ep->lock);
    epitem_free(old);
  }
  
  // Create epitem
  epitem_t *epi = epitem_alloc();
  if (!epi)
    return -12; // ENOMEM
  
  epi->fd = fd;
  epi->node = node;
  epi->event = *event;
  epi->ep = ep;
  epi->last_events = 0;
  epi->on_ready_list = false;
  epi->oneshot = (event->events & EPOLLONESHOT) != 0;
  epi->oneshot_disabled = false;
  epi->exclusive = (event->events & EPOLLEXCLUSIVE) != 0;
  epi->registered_events = event->events;
  
  // Add to epoll instance
  spinlock_acquire(&ep->lock);
  ep->items[fd] = epi;
  ep->item_count++;
  spinlock_release(&ep->lock);
  
  // Check for immediate events (level-triggered)
  if (!(event->events & EPOLLET)) {
    uint32_t events = ep_check_events(epi);
    if (events) {
      epi->last_events = events;
      ep_add_to_ready_list(ep, epi);
    }
  }
  
  return 0;
}

int epoll_ctl_del(eventpoll_t *ep, int fd) {
  if (!ep)
    return -22; // EINVAL
  
  if (fd < 0 || fd >= EPOLL_MAX_WATCHED)
    return -9; // EBADF
  
  epitem_t *epi = ep->items[fd];
  if (!epi)
    return -2; // ENOENT
  
  // Remove from ready list if present
  ep_remove_from_ready_list(ep, epi);
  
  // Remove from epoll instance
  spinlock_acquire(&ep->lock);
  ep->items[fd] = NULL;
  ep->item_count--;
  spinlock_release(&ep->lock);
  
  // Free epitem
  epitem_free(epi);
  
  return 0;
}

int epoll_ctl_mod(eventpoll_t *ep, int fd, struct epoll_event *event) {
  if (!ep || !event)
    return -22; // EINVAL
  
  if (fd < 0 || fd >= EPOLL_MAX_WATCHED)
    return -9; // EBADF
  
  epitem_t *epi = ep->items[fd];
  if (!epi)
    return -2; // ENOENT
  
  // Update event mask
  spinlock_acquire(&epi->lock);
  epi->event = *event;
  epi->oneshot = (event->events & EPOLLONESHOT) != 0;
  epi->exclusive = (event->events & EPOLLEXCLUSIVE) != 0;
  epi->registered_events = event->events;  // Always update registered_events
  
  // Reset oneshot disabled state when re-arming via EPOLL_CTL_MOD
  if (epi->oneshot) {
    epi->oneshot_disabled = false;
  }
  spinlock_release(&epi->lock);
  
  // Remove from ready list and re-check
  ep_remove_from_ready_list(ep, epi);
  
  // Re-check events
  uint32_t events = ep_check_events(epi);
  if (events) {
    epi->last_events = events;
    ep_add_to_ready_list(ep, epi);
  }
  
  return 0;
}

// ── epoll_wait Implementation
// ────────────────────────────────────────────────────────

int epoll_wait_impl(eventpoll_t *ep, struct epoll_event *events, 
                    int maxevents, int timeout_ms) {
  if (!ep || !events)
    return -22; // EINVAL
  
  if (maxevents <= 0)
    return -22; // EINVAL
  
  int returned = 0;
  
  // Check ready list
  spinlock_acquire(&ep->lock);
  
  // If ready list has items, return them immediately
  if (!list_empty(&ep->rdllist)) {
    struct list_head *pos, *n;
    int count = 0;
    
    list_for_each_safe(pos, n, &ep->rdllist) {
      if (count >= maxevents)
        break;
      
      epitem_t *epi = list_entry(pos, epitem_t, rdllink);
      
      // Get current events
      uint32_t current_events = ep_check_events(epi);
      
      if (current_events) {
        // Copy event to user
        events[count].events = current_events;
        // Copy the entire data union
        events[count].data.u64 = epi->event.data.u64;
        
        // Debug: log event data
        klog_puts("[EPOLL] Returning event: events=");
        klog_uint64(current_events);
        klog_puts(" data.u32=");
        klog_uint64(epi->event.data.u32);
        klog_puts(" data.u64=");
        klog_uint64(epi->event.data.u64);
        klog_puts("\n");
        
        count++;
        
        // Handle edge-triggered mode
        if (epi->event.events & EPOLLET) {
          // ET: only report if registered events changed
          // Mask to only registered events (ignore EPOLLHUP/EPOLLERR for ET tracking)
          uint32_t registered_mask = epi->event.events & (EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLRDNORM | EPOLLWRNORM);
          uint32_t tracked_events = current_events & registered_mask;
          
          if (epi->last_events == tracked_events) {
            // Same tracked event as before, skip - remove from list but don't report
            list_del(&epi->rdllink);
            epi->on_ready_list = false;
            ep->rdllist_count--;
            continue;
          }
          // Edge-triggered: remove from ready list after reporting
          list_del(&epi->rdllink);
          epi->on_ready_list = false;
          ep->rdllist_count--;
          epi->last_events = tracked_events;
        }
        
        // Handle oneshot mode
        if (epi->oneshot) {
          // Disable further events until EPOLL_CTL_MOD
          epi->oneshot_disabled = true;
          epi->registered_events = 0; // Clear to prevent further matches
          // Remove from ready list so it won't be reported again
          if (!(epi->event.events & EPOLLET)) {
            list_del(&epi->rdllink);
            epi->on_ready_list = false;
            ep->rdllist_count--;
          }
        }
      } else {
        // No longer has events, remove from ready list
        list_del(&epi->rdllink);
        epi->on_ready_list = false;
        ep->rdllist_count--;
      }
    }
    
    spinlock_release(&ep->lock);
    return count;
  }
  
  spinlock_release(&ep->lock);
  
  // No events ready - handle timeout
  if (timeout_ms == 0) {
    // Non-blocking: return immediately
    return 0;
  }
  
  // Blocking wait (simplified: yield and poll)
  if (timeout_ms > 0) {
    // Approximate wait by yielding
    uint64_t max_iterations = timeout_ms / 10;
    if (max_iterations < 1) max_iterations = 1;
    if (max_iterations > 1000) max_iterations = 1000;
    
    for (uint64_t iter = 0; iter < max_iterations && returned == 0; iter++) {
      sched_yield();
      
      // Re-check all watched FDs
      spinlock_acquire(&ep->lock);
      
      for (int i = 0; i < EPOLL_MAX_WATCHED && returned < maxevents; i++) {
        epitem_t *epi = ep->items[i];
        if (!epi)
          continue;
        
        // Skip disabled oneshot items
        if (epi->oneshot && epi->oneshot_disabled)
          continue;
        
        uint32_t current_events = ep_check_events(epi);
        
        // For ET mode: track only registered events
        uint32_t registered_mask = epi->event.events & (EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLRDNORM | EPOLLWRNORM);
        uint32_t tracked_events = current_events & registered_mask;
        
        // For ET mode: clear last_events when no tracked events (so next rising edge fires)
        if ((epi->event.events & EPOLLET) && tracked_events == 0) {
          epi->last_events = 0;
        }
        
        if (current_events) {
          // Edge-triggered: only report if registered events appeared (transitioned from 0)
          if (epi->event.events & EPOLLET) {
            // ET fires when tracked events transition from 0 to non-zero
            if (epi->last_events == 0 && tracked_events != 0) {
              events[returned].events = current_events;
              events[returned].data.u64 = epi->event.data.u64;
              returned++;
              epi->last_events = tracked_events;
              
              // Handle oneshot
              if (epi->oneshot) {
                epi->oneshot_disabled = true;
                epi->registered_events = 0;
              }
            }
          } else {
            // Level-triggered: always report
            events[returned].events = current_events;
            events[returned].data.u64 = epi->event.data.u64;
            returned++;
            
            // Handle oneshot
            if (epi->oneshot) {
              epi->oneshot_disabled = true;
              epi->registered_events = 0;
            }
          }
        }
      }
      
      spinlock_release(&ep->lock);
    }
  } else {
    // Infinite wait (timeout_ms < 0)
    // Poll indefinitely until events arrive
    while (returned == 0) {
      sched_yield();
      
      spinlock_acquire(&ep->lock);
      
      for (int i = 0; i < EPOLL_MAX_WATCHED && returned < maxevents; i++) {
        epitem_t *epi = ep->items[i];
        if (!epi)
          continue;
        
        if (epi->oneshot && epi->oneshot_disabled)
          continue;
        
        uint32_t current_events = ep_check_events(epi);
        
        // For ET mode: track only registered events
        uint32_t registered_mask = epi->event.events & (EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLRDNORM | EPOLLWRNORM);
        uint32_t tracked_events = current_events & registered_mask;
        
        // For ET mode: clear last_events when no tracked events (so next rising edge fires)
        if ((epi->event.events & EPOLLET) && tracked_events == 0) {
          epi->last_events = 0;
        }
        
        if (current_events) {
          if (epi->event.events & EPOLLET) {
            // ET fires when tracked events transition from 0 to non-zero
            if (epi->last_events == 0 && tracked_events != 0) {
              events[returned].events = current_events;
              events[returned].data.u64 = epi->event.data.u64;
              returned++;
              epi->last_events = tracked_events;
              
              if (epi->oneshot) {
                epi->oneshot_disabled = true;
                epi->registered_events = 0;
              }
            }
          } else {
            events[returned].events = current_events;
            events[returned].data.u64 = epi->event.data.u64;
            returned++;
            
            if (epi->oneshot) {
              epi->oneshot_disabled = true;
              epi->registered_events = 0;
            }
          }
        }
      }
      
      spinlock_release(&ep->lock);
    }
  }
  
  return returned;
}

// ── Epoll FD Management
// ───────────────────────────────────────────────────────

int epoll_alloc_fd(eventpoll_t *ep) {
  struct thread *t = sched_get_current();
  if (!t)
    return -1;
  
  int fd = alloc_fd(t);
  if (fd < 0)
    return -24; // EMFILE
  
  // Create VFS node for epoll instance
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node) {
    return -12; // ENOMEM
  }
  
  memset(node, 0, sizeof(vfs_node_t));
  node->flags = FS_EPOLL;
  node->inode = (uint32_t)(uint64_t)ep;
  node->device = ep;
  node->wait_queue = &ep->wq;
  
  // Set epoll VFS operations
  node->read = epoll_vfs_read;
  node->write = epoll_vfs_write;
  node->open = epoll_vfs_open;
  node->close = epoll_vfs_close;
  node->poll = epoll_vfs_poll;
  
  ep->fd = fd;
  t->fds[fd] = node;
  t->fd_offsets[fd] = 0;
  
  return fd;
}

eventpoll_t *epoll_from_fd(int fd) {
  struct thread *t = sched_get_current();
  if (!t)
    return NULL;
  
  if (fd < 0 || fd >= MAX_FDS)
    return NULL;
  
  vfs_node_t *node = t->fds[fd];
  if (!node)
    return NULL;
  
  if ((node->flags & 0xFF) != FS_EPOLL)
    return NULL;
  
  return (eventpoll_t *)node->device;
}

int epoll_close_fd(int fd) {
  struct thread *t = sched_get_current();
  if (!t)
    return -9; // EBADF
  
  if (fd < 0 || fd >= MAX_FDS)
    return -9; // EBADF
  
  vfs_node_t *node = t->fds[fd];
  if (!node)
    return -9; // EBADF
  
  if ((node->flags & 0xFF) != FS_EPOLL)
    return -22; // EINVAL
  
  eventpoll_t *ep = (eventpoll_t *)node->device;
  
  // Clear FD
  t->fds[fd] = NULL;
  t->fd_offsets[fd] = 0;
  
  // Free VFS node
  kfree(node);
  
  // Release epoll reference
  if (ep) {
    epoll_put(ep);
  }
  
  return 0;
}

// ── Epoll VFS Operations
// ───────────────────────────────────────────────────────

uint32_t epoll_vfs_read(struct vfs_node *node, uint32_t offset, 
                        uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buffer;
  // epoll instances are not readable via read()
  return 0;
}

uint32_t epoll_vfs_write(struct vfs_node *node, uint32_t offset, 
                         uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buffer;
  // epoll instances are not writable via write()
  return 0;
}

void epoll_vfs_open(struct vfs_node *node) {
  if (!node)
    return;
  eventpoll_t *ep = (eventpoll_t *)node->device;
  if (ep)
    epoll_get(ep);
}

void epoll_vfs_close(struct vfs_node *node) {
  if (!node)
    return;
  eventpoll_t *ep = (eventpoll_t *)node->device;
  if (ep)
    epoll_put(ep);
}

int epoll_vfs_poll(struct vfs_node *node, int events) {
  if (!node)
    return POLLNVAL;
  
  eventpoll_t *ep = (eventpoll_t *)node->device;
  if (!ep)
    return POLLNVAL;
  
  int revents = 0;
  
  // Check if ready list has items
  spinlock_acquire(&ep->lock);
  if (!list_empty(&ep->rdllist)) {
    if (events & POLLIN)
      revents |= POLLIN;
  }
  // Always writable (for poll purposes)
  if (events & POLLOUT)
    revents |= POLLOUT;
  spinlock_release(&ep->lock);
  
  return revents;
}

// ── Event Notification
// ────────────────────────────────────────────────────────

void epoll_notify_event(struct vfs_node *node, uint32_t events) {
  if (!node)
    return;
  
  // Track if any exclusive waiter was woken (for EPOLLEXCLUSIVE)
  bool exclusive_woken = false;
  
  // Find all epoll instances watching this node
  // (Simplified: scan all epoll instances)
  for (int i = 0; i < EPOLL_MAX_INSTANCES; i++) {
    eventpoll_t *ep = epoll_table[i];
    if (!ep)
      continue;
    
    // Find epitem for this node
    for (int fd = 0; fd < EPOLL_MAX_WATCHED; fd++) {
      epitem_t *epi = ep->items[fd];
      if (!epi)
        continue;
      
      if (epi->node == node) {
        // Found matching epitem
        // Skip disabled oneshot items
        if (epi->oneshot && epi->oneshot_disabled)
          continue;
        
        uint32_t mask = events & (epi->registered_events | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
        
        if (mask) {
          // Edge-triggered: add to ready list, let epoll_wait handle ET logic
          if (epi->event.events & EPOLLET) {
            // Don't set last_events here - epoll_wait will handle it
            ep_add_to_ready_list(ep, epi);
            
            // Handle exclusive wake-up (prevent thundering herd)
            if (epi->exclusive) {
              if (!exclusive_woken) {
                // Wake only one waiter for exclusive FDs
                wait_queue_wake_one(&ep->wq);
                exclusive_woken = true;
              }
              // Don't wake others - only one epoll instance gets notified
            } else {
              // Normal wake-up: wake all waiters
              wait_queue_wake_all(&ep->wq);
            }
          } else {
            // Level-triggered: always add to ready list
            ep_add_to_ready_list(ep, epi);
            
            // Handle exclusive wake-up
            if (epi->exclusive) {
              if (!exclusive_woken) {
                wait_queue_wake_one(&ep->wq);
                exclusive_woken = true;
              }
            } else {
              wait_queue_wake_all(&ep->wq);
            }
          }
        }
      }
    }
  }
}

// Notify epoll by socket FD (for abstract sockets without VFS node)
void epoll_notify_socket(int fd, uint32_t events) {
  if (fd < 0)
    return;
  
  klog_puts("[EPOLL] notify_socket: fd=");
  klog_uint64(fd);
  klog_puts(" events=");
  klog_uint64(events);
  klog_puts("\n");
  
  // Track if any exclusive waiter was woken (for EPOLLEXCLUSIVE)
  bool exclusive_woken = false;
  
  // Find all epoll instances watching this FD
  for (int i = 0; i < EPOLL_MAX_INSTANCES; i++) {
    eventpoll_t *ep = epoll_table[i];
    if (!ep)
      continue;
    
    // Find epitem for this FD
    epitem_t *epi = ep->items[fd];
    if (!epi)
      continue;
    
    // Skip disabled oneshot items
    if (epi->oneshot && epi->oneshot_disabled)
      continue;
    
    uint32_t mask = events & (epi->registered_events | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
    
    if (mask) {
      // Edge-triggered: add to ready list
      if (epi->event.events & EPOLLET) {
        ep_add_to_ready_list(ep, epi);
        
        if (epi->exclusive) {
          if (!exclusive_woken) {
            wait_queue_wake_one(&ep->wq);
            exclusive_woken = true;
          }
        } else {
          wait_queue_wake_all(&ep->wq);
        }
      } else {
        // Level-triggered
        ep_add_to_ready_list(ep, epi);
        
        if (epi->exclusive) {
          if (!exclusive_woken) {
            wait_queue_wake_one(&ep->wq);
            exclusive_woken = true;
          }
        } else {
          wait_queue_wake_all(&ep->wq);
        }
      }
    }
  }
}

// ── Epoll Subsystem Initialization
// ───────────────────────────────────────────────────────

void epoll_init(void) {
  memset(epoll_table, 0, sizeof(epoll_table));
  epoll_count = 0;
  spinlock_init(&epoll_table_lock);
  
  klog_puts("[OK] Epoll subsystem initialized (max instances: ");
  klog_uint64(EPOLL_MAX_INSTANCES);
  klog_puts(", max watched FDs: ");
  klog_uint64(EPOLL_MAX_WATCHED);
  klog_puts(")\n");
}
