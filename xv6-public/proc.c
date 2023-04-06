#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

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

struct
{
  struct spinlock lock;
  int idx[NPROC];
  // Does these property need?
  int front;
  int back;
} procQ;

static struct proc* initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void* chan);

// Init ptables
void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  initlock(&procQ.lock, "procQ");
  procQ.front = 0;
  procQ.back = 0;
}

// Must be called with interrupts disabled
int cpuid()
{
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
  for (i = 0; i < ncpu; ++i)
  {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
  myproc(void)
{
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

// TODO : dynamically allocate ptable.proc
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

  // UNUSED proc struct found
  // Do not push to Q yet.
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

  p->state = RUNNABLE;
  p->priority = 3;
  p->Q_level = 0;
  p->consumed_time_quantum = 0;

  // Does it need to acquire procQ.lock?
  acquire(&procQ.lock);
  procQ.idx[procQ.back++] = p - ptable.proc;
  release(&procQ.lock);
  release(&ptable.lock);
}

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
  // If can't return -1
  if ((np = allocproc()) == 0) {
    return -1;
  }

  // Copy process state from proc.
  // If can't return -1
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

  np->state = RUNNABLE;
  np->priority = 3;
  np->Q_level = 0;
  np->consumed_time_quantum = 0;

  acquire(&procQ.lock);
  procQ.idx[procQ.back++] = np - ptable.proc;
  release(&procQ.lock);

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

  // userproc never be exited.
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
  // Set priority, Q_level, consumed_time_quantum to useless value.
  curproc->state = ZOMBIE;
  curproc->priority = -1;
  curproc->Q_level = -1;
  curproc->consumed_time_quantum = 105;

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
      // p is child of curproc
      // really QUIT child proc
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
        // set useless value to find bug
        p->priority = -1;
        p->Q_level = -1;
        p->consumed_time_quantum = 105;

        // Delete from procQ
        acquire(&procQ.lock);
        for (int i = 0; i < NPROC;i++) {
          int ptable_idx = procQ.idx[i];
          if (ptable.proc[ptable_idx].pid == pid) {
            procQ.idx[i] = -1;           // can't be idx == -1
            if (i == procQ.front) {
              procQ.front++;
              procQ.front %= NPROC;
            }
            else if (i == procQ.back) {
              procQ.back--;
              procQ.back += NPROC;
              procQ.back %= NPROC;
            }
            // If proc is between front and back
            // It will be handdled after
          }
        }
        release(&procQ.lock);
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

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);

    // Sched Infos
    int sched_priority = 3;
    int sched_Q_level = 0; // does it need?
    int procQ_idx; // does it need? => absolutly need

    // Find Process Infos that are highest priority with respect to 1.Q_level & 2.priority
    // Whenever scheduler is called, Find Process Infos
    // Q_level : from 0 to 2
    // priority : from 0 to 3 (with Q_level == 2)
    // TODO : consumed_time_quantum should be considered.
    for (int lv = 0; lv < 3;lv++) {
      int found = 0; // variable to check find process to be scheduled
      for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->state != RUNNABLE || p->Q_level != lv ||
          p->consumed_time_quantum >= lv * 2 + 4)
          continue; // p is not schedulable process
        found = 1;
        sched_Q_level = lv;
        if (lv == 2) {
          for (struct proc* q = ptable.proc; q < &ptable.proc[NPROC]; q++) {
            if (q->state != RUNNABLE || q->Q_level != 2 ||
              p->consumed_time_quantum >= 8)
              continue;
            sched_priority = sched_priority <= q->priority ? sched_priority : q->priority;
          }
        }
      }

      // It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      // Sched Infos : sched_priority & sched_Q_level
      // Using Sched Infos, find process in procQ.
      // Then, switch to chosen process.  
      if (found) {
        for (int i = 0;i < NPROC;i++) {
          int complete_consume = 0;
          int global_100 = 0;
          procQ_idx = (i + procQ.front) % NPROC;
          if (procQ.idx[procQ_idx] == -1) // empty spot
            continue;
          struct proc* sched_proc = &(ptable.proc[procQ.idx[procQ_idx]]);
          if (sched_proc->state != RUNNABLE ||
            sched_proc->priority != sched_priority || sched_proc->Q_level != sched_Q_level ||
            sched_proc->consumed_time_quantum >= sched_Q_level * 2 + 4)
            continue;

          // This process should be scheduled 
          // until global tick is 100 or consumed time quantum >= level * 2 + 4
          while (1) {
            // This process consumes every time quantum that is assigned to him.
            if (sched_proc->consumed_time_quantum >= sched_Q_level * 2 + 4) {
              complete_consume = 1;
              break;
            }

            // Is it out? or in?
            c->proc = p;
            switchuvm(p);
            p->state = RUNNING;

            swtch(&(c->scheduler), p->context);
            switchkvm();

            sched_proc->consumed_time_quantum++;
            // Global tick is 100
            // It will execute priority boosting
            if (ticks >= 100) {
              global_100 = 1;
              break;
            }
          }

          // Process is done running for now.
          // It should have changed its p->state before coming back.
          // also lv & shced_priority
          // lv for re-searching from L0
          // shced_priority for comparing with p->priority

          c->proc = 0;
          lv = -1; // for-loop starts to zero
          sched_priority = 3; // reset sched_priority 


          // This process consumes every time quantum that is assigned to him.
          // Another process will be scheduled.
          // Process's Q_level += 1 or priority -= 1 
          // and consumed time quantum set to 0.
          if (complete_consume) {
            sched_proc->consumed_time_quantum = 0;
            sched_proc->Q_level = sched_Q_level + 1 < 2 ? sched_Q_level + 1 : 2;
            sched_proc->priority = sched_Q_level == 2 ? sched_priority - 1 : sched_proc->priority;
          }

          // Global ticks is 100
          // It will execute priority boosting.
          if (global_100) {
            // TODO : Logic of priority boosting.
          }
        }
      }
    }
    // There is no RUNNABLE process.
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

int getLevel(void) {
  struct proc* p = myproc();
  int q_level = p->q_level;
  acquire(&ptable_list.lock);

  if (q_level == -1 || q_level == 3) {
    return -1; // Invalid Q level
  }
  release(&ptable_list.lock);
  return q_level;
}

// Set arbitrary Priority to pid Process which is valid
// If not, then It will do nothing
void setPriority(int pid, int priority) {
  if (priority < 0 || priority >4) return; // This is invalid priority
  struct proc* p;
  acquire(&ptable_list.lock);
  for (int i = 0; i < NUMQ; i++) {
    for (p = ptable_list.ptable[i].proc; p < &ptable_list.ptable[i].proc[NPROC]; p++) {
      if (p->pid != pid)
        continue;
      p->priority = priority;
    }
  }
  release((&ptable_list.lock));
}


void schedulerLock(int password) {
  struct proc* p = myproc();
  if (password != PASSWORD) { // Invalid PASSWORD
    kill(p->pid);
    cprintf("pid = %d, time quantum = %u, current queue level = %d",
      p->pid, p->consumed_time_quantum, p->q_level);
    return;
  }

  acquire(&ptable_list.lock);
  int cur_q_level = p->q_level;
  p->q_level = 3; // The highest priority of all
  int i = 0;
  for (struct proc* tmp = ptable_list.ptable[cur_q_level].proc; p < &ptable_list.ptable[cur_q_level].proc[NPROC]; p++, i++) {
    if (tmp->pid != p->pid)
      continue;
    ptable_list.ptable[3].proc[ptable_list.ptable[3].num_proc++] = *tmp; //  이게 들어간다고?
    makeProcClean(tmp);
    ptable_list.ptable[cur_q_level].num_proc--;
  }
  release((&ptable_list.lock));
}

void schedulerUnlock(int password) {
  struct proc* p = myproc();
  if (password != PASSWORD) { // Invalid PASSWORD
    kill(p->pid);
    cprintf("pid = %d, time quantum = %u, current queue level = %d",
      p->pid, p->consumed_time_quantum, p->q_level);
    return;
  }

  acquire(&ptable_list.lock);
  int cur_q_level = 3;
  int i = 0;
  for (struct proc* tmp = ptable_list.ptable[cur_q_level].proc; p < &ptable_list.ptable[cur_q_level].proc[NPROC]; p++, i++) {
    if (tmp->pid != p->pid)
      continue;
    p->priority = 3;
    p->q_level = 0;
    p->consumed_time_quantum = 0;
    ptable_list.ptable[3].num_proc--;

    for (int j = 0; j < NPROC - 1; j++) {
      struct proc* mv = &ptable_list.ptable[0].proc[j];
      tmp = &ptable_list.ptable[0].proc[j + 1];


    }
    ptable_list.ptable[0].num_proc++;
    release((&ptable_list.lock));
    return;
  }

  // Invalid call schedulerUnlock()
  kill(p->pid);
  cprintf("pid = %d, time quantum = %u, current queue level = %d",
    p->pid, p->consumed_time_quantum, p->q_level);
  return;
}

void makeProcClean(struct proc* p) {

  p->sz = 0;
  p->pgdir = 0;
  p->kstack = 0;
  p->state = UNUSED;
  p->pid = 0;
  p->parent = 0;
  p->tf = 0;
  p->context = 0;
  p->chan = 0;
  p->killed = 0;
  for (int i = 0;i < NOFILE;i++)
    p->ofile[i] = 0;
  p->cwd = 0;
  p->priority = -1;
  p->q_level = -1;
  p->consumed_time_quantum = 0;

}