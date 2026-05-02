#ifndef SCHED_WAIT_H
#define SCHED_WAIT_H

#include "../lock/spinlock.h"
#include <stdint.h>

struct thread;

typedef struct wait_queue_entry {
  struct thread *thread;
  struct wait_queue_entry *next;
} wait_queue_entry_t;

typedef struct {
  spinlock_t lock;
  wait_queue_entry_t *head;
} wait_queue_t;

void wait_queue_init(wait_queue_t *wq);
void wait_queue_add(wait_queue_t *wq, wait_queue_entry_t *entry);
void wait_queue_remove(wait_queue_t *wq, wait_queue_entry_t *entry);
void wait_queue_wake_all(wait_queue_t *wq);
void wait_queue_wake_one(wait_queue_t *wq);

#endif