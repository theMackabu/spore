#include "cell.h"

#include <assert.h>
#include <string.h>

static int wake_modeled_futex(struct thread *threads,
                              const struct domain *domain,
                              uint64_t uaddr,
                              int count) {
    int woke = 0;
    for (size_t i = 0; i < MAX_THREADS && woke < count; ++i) {
        if (threads[i].domain == domain &&
            threads[i].state == THREAD_BLOCKED &&
            threads[i].wait_reason == WAIT_FUTEX &&
            threads[i].futex_addr == uaddr) {
            threads[i].state = THREAD_RUNNABLE;
            threads[i].wait_reason = WAIT_NONE;
            threads[i].futex_addr = 0;
            ++woke;
        }
    }
    return woke;
}

int main(void) {
    struct domain a = {.id = 1, .used = true};
    struct domain b = {.id = 2, .used = true};
    struct thread threads[MAX_THREADS];
    memset(threads, 0, sizeof(threads));

    threads[0] = (struct thread) {
        .tid = 10,
        .domain = &a,
        .state = THREAD_BLOCKED,
        .wait_reason = WAIT_FUTEX,
        .futex_addr = 0x4000,
    };
    threads[1] = (struct thread) {
        .tid = 11,
        .domain = &a,
        .state = THREAD_BLOCKED,
        .wait_reason = WAIT_FUTEX,
        .futex_addr = 0x4000,
    };
    threads[2] = (struct thread) {
        .tid = 12,
        .domain = &b,
        .state = THREAD_BLOCKED,
        .wait_reason = WAIT_FUTEX,
        .futex_addr = 0x4000,
    };
    threads[3] = (struct thread) {
        .tid = 13,
        .domain = &a,
        .state = THREAD_BLOCKED,
        .wait_reason = WAIT_FUTEX,
        .futex_addr = 0x8000,
    };

    assert(wake_modeled_futex(threads, &a, 0x4000, 1) == 1);
    assert(threads[0].state == THREAD_RUNNABLE);
    assert(threads[1].state == THREAD_BLOCKED);
    assert(threads[2].state == THREAD_BLOCKED);
    assert(threads[3].state == THREAD_BLOCKED);

    assert(wake_modeled_futex(threads, &a, 0x4000, 8) == 1);
    assert(threads[1].state == THREAD_RUNNABLE);
    assert(threads[2].state == THREAD_BLOCKED);
    assert(threads[3].state == THREAD_BLOCKED);

    assert(wake_modeled_futex(threads, &b, 0x4000, 8) == 1);
    assert(wake_modeled_futex(threads, &a, 0x8000, 8) == 1);
    return 0;
}
