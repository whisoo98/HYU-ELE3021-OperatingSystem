#include "types.h"
#include "stat.h"
#include "user.h"

#define NUM_LOOP 100000
#define NUM_YIELD 20000
#define NUM_SLEEP 500

#define NUM_THREAD 4
#define MAX_LEVEL 3 // 3
#define PASSWORD 2018008240
int parent;

int fork_children()
{
  int i, p;
  for (i = 0; i < NUM_THREAD; i++) {
    p = fork();
    if (p == 0)
    {
      sleep(10);
      return getpid();
    }
  }
  return parent;
}


int fork_children2()
{
  int i, p;
  for (i = 0; i < NUM_THREAD; i++)
  {
    if ((p = fork()) == 0)
    {
      int pid = getpid();
      printf(1, "before sleep pid: %d, lv: %d\n", pid, getLevel());
      sleep(5);
      printf(1, "after sleep pid: %d, lv: %d\n", pid, getLevel());
      return getpid();
    }
    else
    {
      setPriority(p, i);
    }
  }
  return parent;
}

int max_level;

int fork_children3()
{
  int i, p;
  for (i = 0; i < NUM_THREAD; i++)
  {
    if ((p = fork()) == 0) // child sleep
    {
      sleep(10);
      max_level = i;
      return getpid();
    }
  }
  return parent;
}
void exit_children()
{
  if (getpid() != parent) {
    exit();
  }
  while (wait() != -1);
}

int main(int argc, char* argv[])
{
  int child = 5;
  parent = getpid();

  printf(1, "MLFQ test start\n");
  int i = 0, pid = 0;
  int count[MAX_LEVEL] = { 0, 0, 0 };
  char cmd = argv[1][0];
  switch (cmd) {
  case '1':
    printf(1, "[Test 1] default\n");
    pid = fork_children();

    if (pid != parent)
    {
      for (i = 0; i < NUM_LOOP; i++)
      {
        int x = getLevel();
        if (x < 0 || x > 2)
        {
          printf(1, "Wrong level: %d\n", x);
          exit();
        }
        count[x]++;
      }
      printf(1, "Process %d\n", pid);
      for (i = 0; i < MAX_LEVEL; i++)
        printf(1, "Process %d , L%d: %d\n", pid, i, count[i]);
    }
    exit_children();
    printf(1, "[Test 1] finished\n");
    break;
  case '2':
    printf(1, "[Test 2] priorities\n");
    pid = fork_children2();

    if (pid != parent)
    {
      for (i = 0; i < NUM_LOOP; i++)
      {
        int x = getLevel();
        if (x < 0 || x > 2)
        {
          printf(1, "Wrong level: %d\n", x);
          exit();
        }
        count[x]++;
      }
      printf(1, "Process %d\n", pid);
      for (i = 0; i < MAX_LEVEL; i++)
        printf(1, "Process %d , L%d: %d\n", pid, i, count[i]);

    }
    exit_children();
    printf(1, "[Test 2] finished\n");
    break;
  case '3':
    printf(1, "[Test 3] yield\n");
    pid = fork_children2();

    if (pid != parent)
    {
      for (i = 0; i < NUM_YIELD; i++)
      {
        int x = getLevel();
        if (x < 0 || x > 2)
        {
          printf(1, "Wrong level: %d\n", x);
          exit();
        }
        count[x]++;
        yield();
      }
      printf(1, "Process %d\n", pid);
      for (i = 0; i < MAX_LEVEL; i++)
        printf(1, "Process %d , L%d: %d\n", pid, i, count[i]);

    }
    exit_children();
    printf(1, "[Test 3] finished\n");
    break;
  case '4':
    printf(1, "[Test 4] sleep\n");
    pid = fork_children2();
    if (pid != parent)
    {
      int my_pid = getpid();
      printf(1, "pid: %d fork: %d\n", my_pid, getLevel());

      for (i = 0; i < NUM_SLEEP; i++)
      {
        int x = getLevel();
        if (x < 0 || x > 2)
        {
          printf(1, "Wrong level: %d\n", x);
          exit();
        }
        count[x]++;
        sleep(1);
      }
      sleep(my_pid * 3);
      printf(1, "Process %d\n", pid);
      for (i = 0; i < MAX_LEVEL; i++)
        printf(1, "Process %d , L%d: %d\n", pid, i, count[i]);

    }
    exit_children();
    printf(1, "[Test 4] finished\n");
    break;
  case '5':
    printf(1, "[Test 5] max level\n");
    pid = fork_children3();
    printf(1, "Process %d's max level is %d\n", getpid(), max_level);
    if (pid != parent) // childs
    {
      for (i = 0; i < NUM_LOOP; i++)
      {
        int x = getLevel();
        if (x < 0 || x > 2)
        {
          printf(1, "Wrong level: %d\n", x);
          exit();
        }
        count[x]++;
        if (x > max_level) {
          // if (pid == 5) {
          //   printf(1, "Process %d's max level is %d and Q level is %d\n", getpid(), max_level, x);
          // }
          yield();
        }
      }
      printf(1, "Process %d\n", pid);
      for (i = 0; i < MAX_LEVEL; i++)
        printf(1, "Process %d , L%d: %d\n", pid, i, count[i]);

    }
    exit_children();
    printf(1, "[Test 5] finished\n");
    break;
  case '6':
    printf(1, "[Test 6] setPriority return value\n");
    child = fork();

    if (child == 0)
    {
      // int r;
      int grandson;
      sleep(10);
      grandson = fork();
      if (grandson == 0)
      {
        int my_pid = getpid();
        printf(1, "Process %d set process %d's priority to %d\n",
          my_pid, my_pid - 2, 0);
        setPriority(my_pid - 2, 0);

        printf(1, "Process %d set process %d's priority to %d\n",
          my_pid, my_pid - 3, 0);
        setPriority(my_pid - 3, 0);
      }
      else
      {
        int my_pid = getpid();
        printf(1, "Process %d set process %d's priority to %d\n",
          my_pid, grandson, 0);
        setPriority(grandson, 0);

        printf(1, "Process %d set process %d's priority to %d\n",
          my_pid, grandson, 0);
        setPriority(my_pid + 1, 0);
      }
      sleep(20);
      wait();
    }
    else
    {
      int child2 = fork();
      sleep(20);
      if (child2 == 0)
        sleep(10);
      else
      {
        int my_pid = getpid();
        printf(1, "Process %d set process %d's priority to %d\n",
          my_pid, child, -1);
        setPriority(child, -1);

        printf(1, "Process %d set process %d's priority to %d\n",
          my_pid, child, 11);
        setPriority(child, 11);

        printf(1, "Process %d set process %d's priority to %d\n",
          my_pid, child, 10);
        setPriority(child, 10);

        printf(1, "Process %d set process %d's priority to %d\n",
          my_pid, child + 1, 10);
        setPriority(child + 1, 10);

        printf(1, "Process %d set process %d's priority to %d\n",
          my_pid, child + 2, 10);
        setPriority(child + 2, 10);

        printf(1, "Process %d set process %d's priority to %d\n",
          my_pid, -1, 2);
        setPriority(-1, 2);

        printf(1, "Process %d set process %d's priority to %d\n",
          my_pid, child2 + 10, 2);
        setPriority(child2 + 10, 2);
      }
    }

    exit_children();
    printf(1, "done\n");
    printf(1, "[Test 6] finished\n");
    break;
  case '7':
    printf(1, "[Test 7] schedulerLock / Unlock Normal Case\n");
    child = fork();
    if (child == 0) { //child
      int my_pid = getpid();
      printf(1, "Process %d schedluer Lock\n", my_pid);
      schedulerLock(PASSWORD);
      printf(1, "Process %d in Q level %d\n", my_pid, getLevel());
      for (int i = 0;i < NUM_LOOP;i++) {
        if (i % (NUM_LOOP / 10) == 0)
          printf(1, "Process %d in Q level %d\n", my_pid, getLevel());
      }
      schedulerUnlock(PASSWORD);
      printf(1, "Process %d schedluer Unlock\n", my_pid);
    }
    else { // parent
      int my_pid = getpid();
      printf(1, "Process %d schedluer Lock\n", my_pid);
      schedulerLock(PASSWORD);
      printf(1, "Process %d in Q level %d\n", my_pid, getLevel());
      for (int i = 0;i < NUM_LOOP;i++) {
        if (i % (NUM_LOOP / 10) == 0)
          printf(1, "Process %d in Q level %d\n", my_pid, getLevel());
      }
      schedulerUnlock(PASSWORD);
      printf(1, "Process %d schedluer Unlock\n", my_pid);
    }
    exit_children();
    printf(1, "[Test 7] finished\n");

    break;
  case '8':

    printf(1, "[Test 8] schedulerLock / Unlock Wrong Case1 : PASSWORD ERROR\n");
    child = fork();
    if (child == 0) { //child
      int my_pid = getpid();
      printf(1, "Process %d schedluer Lock\n", my_pid);
      printf(1, "This procedure intends to scheduler Lock Error for PASSWORD\n");

      schedulerLock(PASSWORD + 1);
      // It will not be executed under below.
      printf(1, "FROM THIS EXECUTION WILL NOT BE EXECUTED!!\n");
      printf(1, "Process %d in Q level %d\n", my_pid, getLevel());
      schedulerUnlock(PASSWORD + 1);
      printf(1, "Process %d schedluer Unlock\n", my_pid);
    }
    else { // parent
      int child2 = fork();
      if (child2 == 0) {
        int my_pid = getpid();
        printf(1, "Process %d schedluer Lock\n", my_pid);
        schedulerLock(PASSWORD);
        printf(1, "Process %d in Q level %d\n", my_pid, getLevel());
        printf(1, "This procedure intends to scheduler Unlock Error for PASSWORD\n");
        schedulerUnlock(PASSWORD + 1);
        // It will not be executed under below.
        printf(1, "FROM THIS EXECUTION WILL NOT BE EXECUTED!!\n");
        printf(1, "Process %d schedluer Unlock\n", my_pid);
      }
    }
    exit_children();
    printf(1, "[Test 8] finished\n");

    break;
  case '9':

    printf(1, "[Test 9] schedulerLock / Unlock Wrong Case2 : duplication ERROR\n");
    child = fork();
    if (child == 0) { //child
      int my_pid = getpid();
      printf(1, "Process %d schedluer Lock\n", my_pid);

      schedulerLock(PASSWORD);
      printf(1, "Process %d in Q level %d\n", my_pid, getLevel());
      printf(1, "This procedure intends to scheduler Lock Error for duplication\n");
      schedulerLock(PASSWORD);

      // It will not be executed under below.
      printf(1, "FROM THIS EXECUTION WILL NOT BE EXECUTED!!\n");
      printf(1, "Process %d schedluer Unlock\n", my_pid);
    }
    else { // parent
      int child2 = fork();
      if (child2 == 0) {
        int my_pid = getpid();
        printf(1, "Process %d schedluer Lock\n", my_pid);
        schedulerLock(PASSWORD);
        printf(1, "Process %d in Q level %d\n", my_pid, getLevel());
        printf(1, "This procedure intends to scheduler Unlock Error for duplication\n");
        schedulerUnlock(PASSWORD);
        printf(1, "Process %d schedluer Unlock\n", my_pid);
        schedulerUnlock(PASSWORD);

        // It will be executed under below.
        printf(1, "FROM THIS EXECUTION SHOULD BE EXECUTED!!\n");
        printf(1, "Process %d schedluer Unlock Again\n", my_pid);

      }
    }
    exit_children();
    printf(1, "[Test 9] finished\n");
    break;
  case 'A':
    printf(1, "[Test 10] schedulerLock / Unlock Interrupt Call Test\n");
    printf(1, "Process %d schedluer Lock\n", getpid());
    __asm__("int $129");
    printf(1, "Process %d in Q level %d\n", getpid(), getLevel());

    printf(1, "Process %d schedluer Unlock\n", getpid());
    __asm__("int $130");
    printf(1, "Process %d in Q level %d\n", getpid(), getLevel());
    printf(1, "[Test 10] finished\n");
    break;
  default:
    printf(1, "WRONG CMD\n");
    break;
  }


  printf(1, "done\n");
  exit();
}

