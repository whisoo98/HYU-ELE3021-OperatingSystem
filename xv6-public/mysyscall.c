#include "types.h"
#include "defs.h"
#include "param.h"
// #include "stat.h"
#include "mmu.h"
#include "proc.h"
// #include "fs.h"
// #include "spinlock.h"
// #include "sleeplock.h"

int sys_yield(void) {
    yield();
    return 1;
}
int sys_getlevel(void) {
    return getLevel();
}
int sys_setpriority(void) {
    int pid;
    int priority;

    if (argint(0, &pid) < 0 || argint(1, &priority) < 0)
        return -1;
    setPriority(pid, priority);
    return 1;
}
int sys_lock(void) {
    int password;

    if (argint(0, &password) < 0)
        return -1;
    schedulerLock(password);
    return 1;
}
int sys_unlock(void) {
    int password;

    if (argint(0, &password) < 0)
        return -1;
    schedulerUnlock(password);
    return 1;
}