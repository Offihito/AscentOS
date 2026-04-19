#include "wait.h"
#include "sched.h"
#include "../mm/heap.h"
#include <stddef.h>

void wait_queue_init(wait_queue_t *wq) {
    spinlock_init(&wq->lock);
    wq->head = NULL;
}

void wait_queue_add(wait_queue_t *wq, wait_queue_entry_t *entry) {
    if (!wq || !entry) return;
    
    spinlock_acquire(&wq->lock);
    entry->next = wq->head;
    wq->head = entry;
    spinlock_release(&wq->lock);
}

void wait_queue_remove(wait_queue_t *wq, wait_queue_entry_t *entry) {
    if (!wq || !entry) return;
    
    spinlock_acquire(&wq->lock);
    if (wq->head == entry) {
        wq->head = entry->next;
    } else {
        wait_queue_entry_t *curr = wq->head;
        while (curr && curr->next != entry) {
            curr = curr->next;
        }
        if (curr) {
            curr->next = entry->next;
        }
    }
    spinlock_release(&wq->lock);
}

void wait_queue_wake_all(wait_queue_t *wq) {
    if (!wq) return;
    
    spinlock_acquire(&wq->lock);
    wait_queue_entry_t *curr = wq->head;
    while (curr) {
        if (curr->thread && curr->thread->state == THREAD_BLOCKED) {
            curr->thread->state = THREAD_READY;
            curr->thread->wakeup_ticks = 0; // Clear any pending timeout
        }
        curr = curr->next;
    }
    spinlock_release(&wq->lock);
}
