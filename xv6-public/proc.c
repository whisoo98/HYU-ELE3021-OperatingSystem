#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

/*
TODO
*/
// ptable array for 3lv - MLFQ
// 0, 1 and 2 are corresponeded to L0, L1 and L2
// 3 is special ptable to handle process which calls schedulerLock()
// level represents level of Q
// level_time_quantum represents maximum time quantum that could be consumed in each Q level
// num_proc is # of processes in each Q
// Per-Scheduler state
struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

struct fq {
  int proc_idx[NPROC]; // Save index of ptable.proc[]
  int front;      // Front of Q
  int back;       // Back of Q
  int size;       // Size of Q
};

struct fq mlfq[3];

static struct proc* initproc;

int nextpid = 1;
uint global_ticks;
int scheduler_dictator = -1; // idx
int is_dictated = 0;

extern void forkret(void);
extern void trapret(void);
extern int sys_uptime(void);
static void wakeup1(void* chan);

// Init ptables
void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu() - cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
  mycpu(void)
{
  int apicid, i;

  if (readeflags() & FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
  myproc(void) {
  struct cpu* c;
  struct proc* p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

// PAGEBREAK: 32
//  Look in the process table for an UNUSED proc.
//  If found, change state to EMBRYO and initialize
//  state required to run in the kernel.
//  Otherwise return 0.
//  -----------------------------------------------
//  Allocate Process to L0.
static struct proc*
allocproc(void)
{
  struct proc* p;
  char* sp;

  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if ((p->kstack = kalloc()) == 0) {
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof * p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof * p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof * p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc* p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();

  initproc = p;
  if ((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  // Set process default property
  p->state = RUNNABLE;
  p->priority = 3;
  p->qlv = 0;
  p->consumed_tq = 0;
  int uproc_idx = p - (ptable.proc);
  // EnQ process to mlfq[0]
  enQ(0, uproc_idx);

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc* curproc = myproc();

  sz = curproc->sz;
  if (n > 0) {
    if ((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  else if (n < 0) {
    if ((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}


// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc* np;
  struct proc* curproc = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0) {
    return -1;
  }

  // Copy process state from proc.
  if ((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0) {
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for (i = 0; i < NOFILE; i++)
    if (curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  // Set process default property
  np->state = RUNNABLE;
  np->priority = 3;
  np->qlv = 0;
  np->consumed_tq = 0;
  int uproc_idx = np - (ptable.proc);
  // EnQ process to mlfq[0]
  enQ(0, uproc_idx);
  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc* curproc = myproc();
  struct proc* p;
  int fd;

  if (curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for (fd = 0; fd < NOFILE; fd++) {
    if (curproc->ofile[fd]) {
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->parent == curproc) {
      p->parent = initproc;
      if (p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  if (scheduler_dictator == curproc - ptable.proc) {
    is_dictated = 0;
    scheduler_dictator = -1;
  }
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc* p;
  int havekids, pid;
  struct proc* curproc = myproc();

  acquire(&ptable.lock);
  for (;;) {
    // Scan through table looking for exited children.
    havekids = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
      if (p->parent != curproc)
        continue;
      havekids = 1;
      if (p->state == ZOMBIE) {
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || curproc->killed) {
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}


//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc* p;
  struct cpu* c = mycpu();
  c->proc = 0;
  for (;;) {
    // Enable interrupts on this processor.
    sti();

    acquire(&ptable.lock);

    int found = 0;
    int lv = 0;
    int proc_idx = -1;

    // Scheduler is dictated by process that called schedulerLock().
    if (is_dictated) {
      // Choose dictator to schedule.
      proc_idx = scheduler_dictator;
    }
    else {
      // Loop over mlfq looking for process to run with respect to lv from 0 to 2.
      for (lv = 0;lv < 3;lv++) {
        proc_idx = pickProc(lv);
        if (proc_idx != -1) {
          found = 1;
          break;
        }
      }

      // There is no RUNNABLE process.
      if (!found) {
        release(&ptable.lock);
        continue;
      }
    }
    p = &ptable.proc[proc_idx];

    // Switch to chosen process.  It is the process's job
    // to release ptable.lock and then reacquire it
    // before jumping back to us.
    c->proc = p;
    switchuvm(p);
    p->state = RUNNING;
    swtch(&(c->scheduler), p->context);
    switchkvm();
    // Feedback
    p->consumed_tq++;
    if (!is_dictated) {

      // Adjust Qlv + 1 or priroriy - 1.
      if (p->consumed_tq >= lv * 2 + 4) {
        if (lv == 2) {
          p->priority =
            p->priority - 1 >= 0 ? p->priority - 1 : 0;
        }
        lv = lv + 1 < 2 ? lv + 1 : 2;
        p->qlv = lv;
        p->consumed_tq = 0;
      }
      if (p->state != ZOMBIE) {
        enQ(lv, proc_idx);
      }
    }

    // // Do prirority Boosting
    if (global_ticks >= 100) {
      global_ticks = 0;
      priorityBoost();
    }

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    release(&ptable.lock);
  }

}


// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc* p = myproc();

  if (!holding(&ptable.lock))
    panic("sched ptable.lock");
  if (mycpu()->ncli != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (readeflags() & FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void* chan, struct spinlock* lk)
{
  struct proc* p = myproc();

  if (p == 0)
    panic("sleep");

  if (lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if (lk != &ptable.lock) {  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if (lk != &ptable.lock) {  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void* chan)
{
  struct proc* p;

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void* chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc* p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid) {
      p->killed = 1;
      // Wake process from sleep if necessary.
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char* states[] = {
  [UNUSED] "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc* p;
  char* state;
  uint pc[10];

  cprintf("gtick: %d\n", global_ticks);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if (p->state == SLEEPING) {
      getcallerpcs((uint*)p->context->ebp + 2, pc);
      for (i = 0; i < 10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}


// Get Level of which process is running on mlfq.
// If the process is dictator, it will return -1.
// And this is intended result.
int getLevel(void) {
  struct proc* p = myproc();
  return p->qlv;
}

// Set arbitrary Priority to pid Process which is valid.
// If it is not vaild, then it do nothing.
void setPriority(int pid, int priority) {
  acquire(&ptable.lock);
  int found = 0;
  int proc_idx = -1;
  for (int lv = 0;lv < 3;lv++) {
    int sz = mlfq[lv].size;
    for (int i = 0;i < sz;i++) {
      proc_idx = deQ(lv);
      enQ(lv, proc_idx);
      if (ptable.proc[proc_idx].pid != pid)
        continue;
      found = 1;
      break;
    }
    if (found)
      break;
  }
  // NOT VALID condition
  if (priority < 0 || priority >4) {
    cprintf("Priority should be in range from 0 to 3\n");
    release(&ptable.lock);
    //exit();
  }
  // NOT VALID condition
  else if (!found) {
    cprintf("Not Running Process pid on Memory\n");
    release(&ptable.lock);
    //exit();
  }
  else {
    // VAILD condition
    ptable.proc[proc_idx].priority = priority;
    release(&ptable.lock);
  }
}

// SYSCALL to execute process with the most highest priority of others.

// It is called with interrupt 129.
// The process be excuted before MLFQ scheduler.

// Argument is password, student ID, to pass function.
// If it is NOT VALID, print (pid, time quantum, current queue level).
// And exit the process that calls schedulerLock().
// And MLFQ will do normally.
// If it is VALID, schedulerLock() will perform normally.

// TODO : What if this called again while Locked?

// When it starts, global ticks set to 0 w/o Priority Boosting
// If following conditions are satisfied, stop the execute process,
// go back to MLFQ scheduler process.

// Condition 1. call schedulerUnlock() (no matter what its validity is)
// In Condition 1, after sequances are up to schedulerUnlock()

// Condition 2. Global ticks become 100
// In Condition 2, executing process be posed to in front of L0.
// And arise Priority Boosting
void schedulerLock(int password) {
  struct proc* p = myproc();
  acquire(&tickslock);

  // NOT VALID condition
  if (is_dictated) { // Block trying to dicate again.
    cprintf("pid = %d, time quantum = %d, current queue level = %d\n",
      p->pid, p->consumed_tq, p->qlv);
    release(&tickslock);
    scheduler_dictator = -1; // dictator out.
    is_dictated = 0;
    exit();
  }
  else if (password != PASSWORD) {
    cprintf("pid = %d, time quantum = %d, current queue level = %d\n",
      p->pid, p->consumed_tq, p->qlv);
    release(&tickslock);
    exit();
  }
  else {
    // VAILD condition
    global_ticks = 0; // global ticks set to 0.
    scheduler_dictator = p - ptable.proc; // variable to schedule one process .
    is_dictated = 1; // variable to represent scheduler dictated.
    p->consumed_tq = 0;
    p->qlv = -1;     // Dictating scheduler can be condsidered that process is in the most highest MLFQ which level is -1.
    release(&tickslock);
  }
}

// SYSCALL to stop the sequence of execute process prior than MLFQ scheduler.

// It is called with interrupt 130.
// The process be excuted onto MLFQ scheduler.

// Argument is password, student ID, to pass function.
// If it is NOT VALID, print (pid, time quantum, current queue level).
// And exit the process that calls schedulerunLock().
// And MLFQ will do normally.
// If it is VALID, schedulerUnlock() will perform normally.

// What the schdulerUnlock does is
// 1. Executing process be posed to in front of L0.
// 2. The Process' priority goes to 3 and time quantum 0.

// Unlike schedulerLock does, 
// schedulerUnlock() can be called from the process 
// who has never called schedulerLock().
// In this case, it is considered as NOT VALID.
// Therefore, print (pid, time quantum, current queue level)
// and go back to MLFQ scheduler process.
void schedulerUnlock(int password) {
  struct proc* p = myproc();
  acquire(&ptable.lock);
  // NOT VALID condition
  if (password != PASSWORD || !is_dictated || scheduler_dictator != p - ptable.proc) {
    cprintf("pid = %d, time quantum = %d, current queue level = %d\n",
      p->pid, p->consumed_tq, p->qlv);
    release(&ptable.lock);
    exit();
  }
  else {
    // VALID condition
    // posed to in front of L0.
    p->priority = 3;
    p->qlv = 0;
    p->consumed_tq = 0;

    int lv = 0;
    int sz = mlfq[lv].size;
    enQ(lv, p - ptable.proc);
    for (int i = 0;i < sz;i++) {
      enQ(lv, deQ(lv));
    }

    scheduler_dictator = -1; // variable to schedule one process .
    is_dictated = 0; // variable to represent scheduler dictated.
    release(&ptable.lock);
  }
}

// Do priority Boosting
// All processes are pushed to L0.
// All processes's priority reset to 3.
// All processes's time quantum reset to 0.
// scheduler_dictator set to 0, 
// because priorityBoost() can be called when scheduler locked.
// When do boosting, it does not need to condiser 
// that to sort processes in descending the priority order in L2.
// Before call priorityBoost(), ptable should be locked.
void priorityBoost(void) {
  // If scheduler has been dictated, dictator should be enQed in front of mlfq[0].
  if (is_dictated) {
    int sz = mlfq[0].size;
    enQ(0, scheduler_dictator);
    for (int i = 0;i < sz;i++) {
      enQ(0, deQ(0));
    }
    is_dictated = 0;
    scheduler_dictator = -1;
  }
  //priorityBoost
  for (int lv = 0;lv <= 2;lv++) {
    int sz = mlfq[lv].size;
    for (int i = 0;i < sz;i++) {
      int boost_idx = deQ(lv);
      ptable.proc[boost_idx].qlv = 0;
      ptable.proc[boost_idx].priority = 3;
      ptable.proc[boost_idx].consumed_tq = 0;
      enQ(0, boost_idx);
    }
  }
}

// It is impossible that enQ() blocked because of full Q.
// int -> void?
int enQ(int lv, int idx) {
  mlfq[lv].proc_idx[mlfq[lv].back] = idx;
  mlfq[lv].back++;
  mlfq[lv].back %= NPROC;
  mlfq[lv].size++;
  return idx;
}

// It is impossible to execute deQ() when the Q[lv] is empty.
// If Q is empty, return -1.
// If Q is NOT empty, return its value & pop.
int deQ(int lv) {
  if (isEmpty(lv))
    return -1;
  int ret = mlfq[lv].proc_idx[mlfq[lv].front];
  mlfq[lv].front++;
  mlfq[lv].front %= NPROC;
  mlfq[lv].size--;
  return ret;
}

// Check Q is empty or not.
int isEmpty(int lv) {
  return mlfq[lv].size == 0;
}

// Pick process from mlfq[lv]
// Followings are sequance 
// that how to choose process to run.
// Case I. lv is 0 or 1.
// 1. DeQ from mlfq[lv] and check that is it RUNNABLE.
// 2. If it is, DONE.
// 3. Else, enQ it and go back to sequence 1.

// Case II. lv is 2.
// 0. rounding mlfq[lv], find RUNNABLE process's property 
// that(highest priority, fastest idx).
// 1. DeQ from mlfq[lv] and check that is it RUNNABLE.
// 2. If it is, go to sequence 4.
// 3. Else, enQ it and go back to sequence 1.
// 4. Check that it has right property 
// that I found in sequence 0.
// 5. If it is, DONE.
// 6. Else, enQ it and go back to sequence 1.

// If success, return process index 
// which represent ptable.proc[].
// Else, return -1;
int pickProc(int lv) {
  if (isEmpty(lv)) return -1;

  int sz = mlfq[lv].size;

  if (lv == 2) {
    int proc_priority = USELESS;
    int mlfq_idx = 0;
    // Look through mlfq[2] to find process 
    // who has highest priority.
    for (int i = 0;i < sz;i++) {
      int proc_idx = deQ(lv);
      enQ(lv, proc_idx);
      if (ptable.proc[proc_idx].state != RUNNABLE) {
        continue;
      }
      if (proc_priority > ptable.proc[proc_idx].priority) {
        proc_priority = ptable.proc[proc_idx].priority;
        mlfq_idx = i; // This will be picked.
      }
    }
    if (proc_priority == USELESS)
      return -1;
    int proc_idx = deQ(lv);
    for (int i = 0;i < mlfq_idx;i++) {
      enQ(lv, proc_idx);
      proc_idx = deQ(lv);
    }
    return proc_idx;
  }

  else {
    for (int i = 0;i < sz;i++) {
      int proc_idx = deQ(lv);
      if (ptable.proc[proc_idx].state != RUNNABLE) {
        enQ(lv, proc_idx);
        continue;
      }
      return proc_idx;
    }
  }
  return -1;
}