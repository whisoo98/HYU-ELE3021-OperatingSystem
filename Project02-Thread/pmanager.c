#include "types.h"
#include "stat.h"
#include "user.h"

#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS 10

int fork1(void);  // Fork but panics on failure.
void panic(char*);

int nbuf = 250;

void strSplit(char* str, char* buf, int* idx) {
    int st = 0;
    for (int i = *idx; i < nbuf;i++) {
        if (buf[i] == ' ' || buf[i] == '\n') {
            *idx = i + 1;
            str[st++] = '\0';
            break;
        }
        str[st++] = buf[i];
    }
}

int main(int argc, char* argv[]) {
    printf(2, "Hello Pmanager!\n");
    while (1) {
        printf(2, "> ");

        char buf[nbuf];
        char* argv[MAXARGS];

        gets(buf, nbuf); // read line

        // Parsing cmd
        char cmd[10];
        int idx = 0;
        char cpid[20];
        char cstacksz[20];
        char cmemlim[20];
        char cpath[55];
        int pid;
        int memlim;
        int stacksz;
        strSplit(cmd, buf, &idx);

        if (!strcmp(cmd, "list")) { // cmd == list
            // Listing info of all the processes executing.
            // Info : process name, pid, # of stack pages, 
            // size of allocated memory, memory limitation.
            // Info must includes thread info.
            // Thread's info should not be printed separatly.
            // It is up to me that define any system call to get infos of process.
            procInfo();
        }
        else if (!strcmp(cmd, "kill")) { // cmd == kill
            // Kill the process with pid.
            // Use "kill" syscall.
            // Print weather it is success or not.
            strSplit(cpid, buf, &idx);

            printf(2, "KILL Process : %s\n", cpid);
            pid = atoi(cpid);
            if (kill(pid) < 0) {
                printf(2, "kill Process : %d failed\n", pid);
            }
            else {
                printf(2, "kill Process : %d succeed\n", pid);
            }
        }
        else if (!strcmp(cmd, "execute")) { // cmd == execute
            // Execute program that is located in <path>, 
            // with stack pages which is amount of <stacksize>.
            // A argument parsed to program is 0-th parameter, <path>.
            // pmanager is still executing, does not wait about terminating program executed.
            // Do not print any message when it succeed, otherwise print.
            if (fork1() == 0) {
                if (fork1() == 0) {
                    strSplit(cpath, buf, &idx);
                    strSplit(cstacksz, buf, &idx);
                    argv[0] = cpath;
                    stacksz = atoi(cstacksz);
                    exec2(argv[0], argv, stacksz);
                }
                else {
                    exit();
                }
                printf(2, "EXECUTE Process in %s failed\n", cpath);
                exit();
            }
            wait();
        }
        else if (!strcmp(cmd, "memlim")) { // cmd == memlimit
            // Set memory limit of process with <pid> to <limit>.
            // <limit> is integer greater or equal to 0.
            // If it is 0, there is no limit.
            // Otherwise, it has limit with same amount of <limit>.
            // Process's memory should be considered thread's memory.
            // Print weather it is success or not.

            // setmemomylimit syscall
            strSplit(cpid, buf, &idx);
            strSplit(cmemlim, buf, &idx);
            pid = atoi(cpid);
            memlim = atoi(cmemlim);
            if (setmemorylimit(pid, memlim) < 0) { // failure
                printf(2, "set process %d's memory limit : %d failed\n", pid, memlim);
            }
            else {
                printf(2, "set process %d's memory limit : %d succeed\n", pid, memlim);
            }
        }
        else if (!strcmp(cmd, "exit")) { // cmd == kill
            // Terminate pmanage.
            printf(2, "Goodbye Pmanager!\n");
            exit();
        }
        else {
            printf(1, "Wrong Command : %s\n", cmd);
        }
    }
    exit();
}

int
fork1(void)
{
    int pid;

    pid = fork();
    if (pid == -1)
        panic("fork");
    return pid;
}

void
panic(char* s)
{
    printf(2, "%s\n", s);
    exit();
}