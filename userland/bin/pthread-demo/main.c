#include <pthread.h>
#include <stdio.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int counter;

static void *worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 100000; ++i) {
        pthread_mutex_lock(&lock);
        ++counter;
        pthread_mutex_unlock(&lock);
    }
    return 0;
}

int main(void) {
    enum { THREADS = 8 };
    pthread_t threads[THREADS];
    for (int i = 0; i < THREADS; ++i) {
        if (pthread_create(&threads[i], 0, worker, 0) != 0) {
            puts("threads=8 counter=0 FAIL");
            return 1;
        }
    }
    for (int i = 0; i < THREADS; ++i) {
        if (pthread_join(threads[i], 0) != 0) {
            puts("threads=8 counter=0 FAIL");
            return 1;
        }
    }
    printf("threads=8 counter=%d %s\n", counter, counter == 800000 ? "PASS" : "FAIL");
    return counter == 800000 ? 0 : 1;
}
