#include "types.h"
#include "stat.h"
#include "user.h"


#define NTHREADS 10

int c = 10;
thread_t threads[NTHREADS];

void* thread_func(void* arg) {
    // int c = 10;
    // int thread_id = *(int*)arg;
    // printf(1, " BEGIN Passed arg %d, Thread %d is running\n", *(int*)arg, threads[thread_id]);
    c -= 1;
    printf(1, "%d\n", c);
    sleep(10);
    int* ret;
    // printf(1, " END Passed arg %d, Thread %d is running\n", *(int*)arg, threads[thread_id]);
    thread_exit((void*)(&ret));
    printf(1, "IT MUST NO BE PRINTTED\n");
    return ret;
}

int main() {
    int i;
    int thread_args[NTHREADS];
    // Create threads
    for (i = 0; i < NTHREADS; i++) {
        thread_args[i] = i;
        printf(1, "for loop : %d\n", thread_args[i]);
        if (thread_create(&threads[i], thread_func, (void*)&thread_args[i]) != 0) {
            printf(1, "Failed to create thread\n");
            exit();
        }
        sleep(10);
    }
    // // Wait for threads to finish
    for (i = 0; i < NTHREADS; i++) {
        // printf(1, "for loop : %d\n", thread_args[i]);
        if (thread_join(threads[i], 0) != 0) {
            printf(1, "Failed to join thread\n");
            exit();
        }
    }
    printf(1, "EXIT\n");
    exit();
}
