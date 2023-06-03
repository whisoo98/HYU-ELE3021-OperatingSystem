#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

// Process is A cloth for Theads.
struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

// pthread table that contains Actual information of Threads.
struct {
  struct spinlock lock;
  struct pthread_t pthread[NTHRD];
} pthread_table;

static struct proc* initproc;

int nextpid = 1;
// uint nextthid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void* chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  initlock(&pthread_table.lock, "pthread_table");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu() - cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu* mycpu(void)
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
struct proc* myproc(void) {
  struct cpu* c;
  struct proc* p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

// Look in the pthread table for an UNUSED thread.
// If found, change state to EMBRYO and initialize
// context and trap frame to default.
// Otherwise return 0.
// Other thread informations be initialized on caller.
static struct pthread_t*
allocthread(void) {
  struct pthread_t* th;
  char* sp;

  for (th = pthread_table.pthread; th < &pthread_table.pthread[NTHRD]; th++)
    if (th->state == UNUSED)
      goto found;

  return 0;

found:

  th->state = EMBRYO;

  // Allocate kernel stack.
  if ((th->kstack = kalloc()) == 0) {
    th->state = UNUSED;
    return 0;
  }
  sp = th->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof * th->tf;
  th->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof * th->context;
  th->context = (struct context*)sp;
  memset(th->context, 0, sizeof * th->context);
  th->context->eip = (uint)forkret;
  th->retval = 0; // set retval to zero;

  return th;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc* p;
  struct pthread_t* th;
  char* sp;

  acquire(&ptable.lock);
  // Allocate New Process.
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == UNUSED)
      goto find_thread;

  release(&ptable.lock);
  return 0;

find_thread:
  // Allocate Thread.
  if ((th = allocthread()) != null)
    goto found;

  release(&ptable.lock);
  return 0;

found:

  p->state = EMBRYO;
  p->pid = nextpid++;
  th->pid = p->pid;

  // assign process kernel stack with thread kernel stack.
  if ((p->kstack = th->kstack) == 0) {
    // Impossible Condition
    p->state = UNUSED;
    release(&ptable.lock);
    return 0;
  }

  // set process information as default.
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
  th->context->eip = p->context->eip;
  p->sz = 0;
  p->memlimit = 0;
  p->thread_assign = 1;

  // Write A name of Thread to figure out
  // who wear this process.
  p->thread_idx = th - pthread_table.pthread;
  // The thread who wear process first is
  // owner of process.
  p->thread_id = MAINTH;
  p->main_thread_idx = p->thread_idx;
  p->stacksz = 1;
  p->front = 0;
  p->back = 0;
  p->freed = 0;
  p->parent_proc = 0;
  p->parent_pthread = 0;
  p->joiner_pthread = th;
  memset(p->freed_sz, 0, sizeof(p->freed_sz));
  release(&ptable.lock);
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

  int ptable_idx = p - ptable.proc;
  p->th_sz = p->sz;
  p->parent_proc = p;
  p->parent_pthread = &pthread_table.pthread[p->thread_idx];
  p->joiner_pthread = &pthread_table.pthread[p->thread_idx];

  memset(p->freed_sz, 0, sizeof(p->freed_sz));

  // Thread undress process.
  mvProcToThread(ptable_idx, &pthread_table.pthread[p->thread_idx]);
  // Dust off process.
  clearProc(p);
  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return oldsz on success, -1 on failure.
// It is used for sbrk, 
// expanding heap that can be shared with threads.
// PROBLEM
// I can't solve a problem of allocating free space between stacks.
// It is quite hard to handle contigous free pages allocated to heap.
// Therefore, there is a situtation that
// despite of a lot of free pages in memory,
// OS can not allocate heap.
int
growproc(int n)
{
  uint sz;

  acquire(&ptable.lock);
  struct proc* curproc = myproc();
  if (curproc->memlimit != 0 && curproc->sz + n > curproc->memlimit) {
    release(&ptable.lock);
    return -1;
  }
  uint oldsz = curproc->sz;
  sz = curproc->sz;
  if (n > 0) {
    if ((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0) {
      release(&ptable.lock);
      return -1;
    }
  }
  else if (n < 0) {
    if ((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0) {
      release(&ptable.lock);
      return -1;
    }
  }
  curproc->sz = sz;

  switchuvm(curproc);
  release(&ptable.lock);
  return oldsz;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
// If fork is called with MAINTH thread, process owner,
// it runs process fork that copy every PCB including threads.
// Otherwise it runs thread fork that copy only infos about itself.
int
fork(void)
{

  int i, pid;
  struct proc* np;
  struct proc* curproc = myproc();
  acquire(&pthread_table.lock);

  // Fork Process.
  if ((np = allocproc()) == 0) {
    release(&pthread_table.lock);
    return -1;
  }

  // Copy process state from proc.
  if ((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0) {
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    release(&pthread_table.lock);
    return -1;
  }

  np->parent_proc = curproc;
  np->parent_pthread = &(pthread_table.pthread[curproc->thread_idx]);
  *np->tf = *curproc->tf;
  np->sz = curproc->sz;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for (i = 0; i < NOFILE; i++)
    if (curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  // MAINTHREAD COPY
  // pgdir copied
  // name copied
  // parent setted
  // cwd copied
  // ofile copied
  np->memlimit = curproc->memlimit;
  np->thread_assign = curproc->thread_assign;
  np->stacksz = curproc->stacksz;
  // pid setted
  // thread_idx is special
  // kstack copied
  np->th_sz = curproc->th_sz;
  np->state = RUNNABLE;
  np->chan = curproc->chan;
  // tf copied.
  // context setted
  np->killed = curproc->killed;
  np->thread_id = curproc->thread_id; // MAINTH
  np->front = curproc->front;
  np->back = curproc->back;
  np->freed = curproc->freed;
  memset(np->freed_sz, 0, sizeof(np->freed_sz));
  for (int i = 0;i < np->freed;i++) {
    np->freed_sz[(np->front + i) % NTHRD] =
      curproc->freed_sz[(curproc->front + i) % NTHRD];
  }

  // Only fork() thread.
  if (curproc->thread_id != MAINTH) {

    np->thread_assign = 1;
    np->thread_id = MAINTH;
    np->joiner_pthread = &(pthread_table.pthread[curproc->thread_idx]);
    mvProcToThread(np - ptable.proc, &pthread_table.pthread[np->thread_idx]);
    clearProc(np);
    release(&ptable.lock);
    release(&pthread_table.lock);

    return curproc->pid;
  }
  mvProcToThread(np - ptable.proc, &pthread_table.pthread[np->thread_idx]);
  // Copy Threads.
  struct pthread_t* th;
  for (th = pthread_table.pthread; th < &pthread_table.pthread[NTHRD];th++) {
    if (th->pid == curproc->pid) {
      if (th->thread_id == MAINTH)
        continue;
      struct pthread_t* nth;
      if ((nth = allocthread()) == null) {
        np->killed = 1;
        pid = -1;
        break;
      }
      nth->th_sz = th->th_sz;
      nth->pid = np->pid;
      safestrcpy(nth->kstack, th->kstack, sizeof(th->kstack));
      nth->state = th->state;
      nth->chan = th->chan;
      *nth->context = *th->context;
      *nth->tf = *th->tf;
      nth->ptable_idx = np - ptable.proc;
      nth->thread_id = th->thread_id;
      nth->joiner_pthread = &pthread_table.pthread[np->main_thread_idx];
      nth->retval = th->retval;
    }
  }
  release(&ptable.lock);
  release(&pthread_table.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
// Whether who calls exit, it will terminate entire process.
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
  acquire(&pthread_table.lock);
  acquire(&ptable.lock);

  wakeup1(curproc->parent_pthread);

  // Pass abandoned children to init.
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->parent_proc == curproc) {
      p->parent_proc = initproc;
      p->parent_pthread = initproc->parent_pthread;
      if (p->state == ZOMBIE)
        wakeup1(initproc->parent_pthread);
    }
  }

  curproc->state = ZOMBIE;

  struct pthread_t* th;
  for (th = pthread_table.pthread; th < &pthread_table.pthread[NTHRD]; th++) {
    if (th->pid == curproc->pid) {
      th->state = THZOMBIE;
    }
  }
  release(&pthread_table.lock);
  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
// Clear Child Process & Thread Memory.
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
      if (p->parent_pthread != &(pthread_table.pthread[curproc->thread_idx]))
        continue;
      havekids = 1;
      // Found one.
      if (p->state == ZOMBIE) {
        pid = p->pid;
        // Find Threads.
        // Release Threads memory that unexpectly quited.
        struct pthread_t* th;
        for (th = pthread_table.pthread; th < &pthread_table.pthread[NTHRD]; th++) {
          if (th->pid != p->pid) {
            continue;
          }
          th->th_sz = 0;
          th->pid = 0;
          kfree(th->kstack);
          th->kstack = 0;
          th->state = UNUSED;
          th->chan = 0;
          th->ptable_idx = 0;
          th->thread_id = 0;
          th->joiner_pthread = 0;
          th->retval = 0;
        }
        freevm(p->pgdir);
        p->name[0] = 0;
        p->parent_pthread = 0;
        p->parent_proc = 0;
        p->joiner_pthread = 0;
        p->memlimit = 0;
        p->thread_assign = 0;
        p->stacksz = 0;
        p->pid = 0;
        p->thread_idx = 0;
        p->sz = 0;
        p->state = UNUSED;
        p->chan = 0;
        p->killed = 0;
        p->thread_id = 0;
        p->front = 0;
        p->back = 0;
        p->freed = 0;
        memset(p->freed_sz, 0, sizeof(p->freed_sz));
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
    sleep(&(pthread_table.pthread[curproc->thread_idx]), &ptable.lock);  //DOC: wait-sleep
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
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
      if (p->state != RUNNABLE) {
        continue;
      }
      struct pthread_t* th;
      for (th = pthread_table.pthread; th < &pthread_table.pthread[NTHRD]; th++) {
        if (th->pid == p->pid && th->state == RUNNABLE) {

          mvThreadToProc(th - pthread_table.pthread, p);
          // Switch to chosen process.  It is the process's job
          // to release ptable.lock and then reacquire it
          // before jumping back to us.
          c->proc = p;
          switchuvm(p);
          p->state = RUNNING;
          swtch(&(c->scheduler), p->context);
          switchkvm();

          // Process is done running for now.
          // It should have changed its p->state before coming back.
          mvProcToThread(p - ptable.proc, th);
          if (p->state == ZOMBIE) {
            break;
          }

        }
      }
      c->proc = 0;
      clearProc(p);
    }
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
  if (mycpu()->ncli != 1) {
    cprintf("PROCESS %s,%d %d\n", p->name, p->pid, p->thread_id);
    panic("sched locks");
  }
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
  struct pthread_t* th;
  for (th = pthread_table.pthread; th < &pthread_table.pthread[NTHRD]; th++)
    if (th->state == SLEEPING && th->chan == chan)
      th->state = RUNNABLE;
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
// kill =1 때문에 문제생기스늗ㅅ

int
kill(int pid)
{
  struct proc* p;
  struct pthread_t* th;
  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid) {
      p->killed = 1;
      // Wake process from sleep if necessary.
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      for (th = pthread_table.pthread; th < &pthread_table.pthread[NTHRD];th++) {
        if (th->pid == pid) {
          th->state = THZOMBIE;
          if (th->thread_id == MAINTH) {
            th->state = RUNNABLE;
          }
        }
      }

      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// Set the limitation of memory allocation to process with {pid}
// User process can be allocated memory unless it is not exceeded limit.
// When the process allocated, there is no limitation.
// By system call, limitation has been setted.
// {pid} : process's pid to set the memory limit.
// {limit} : represent memory limit afford to process with byte unit.
// It is greater or equal to 0. If it is 0, then there is no limit.
// Otherwise, it is represent memory limit.
// This will return -1, if {limit} is less than allocated memory to process.
// Also there is no process with {pid}.
// Otherwise, return 0.
int setmemorylimit(int pid, int limit) {
  if (limit < 0) {
    cprintf("Can not be memory limit negative : %d\n", limit);
    return -1;
  }

  acquire(&ptable.lock);
  struct proc* p;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid != pid) {
      continue;
    }
    if (p->sz > (uint)limit) {
      cprintf("memory limit %d is less than allocated memory size : %d\n", limit, p->sz);
      release(&ptable.lock);
      return -1;
    }
    p->memlimit = (uint)limit;
    release(&ptable.lock);
    return 0;
  }
  cprintf("There is no process with %d\n", pid);
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
  [ZOMBIE]    "zombie",
  [THZOMBIE]    "thread_zombie"
  };
  int i;
  struct proc* p;
  char* state;
  char* th_state;
  uint pc[10];

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s\n", p->pid, state, p->name);

    for (struct pthread_t* th = pthread_table.pthread; th < &pthread_table.pthread[NTHRD];th++) {
      if (th->state == UNUSED || th->pid != p->pid)
        continue;
      if (th->state >= 0 && th->state < NELEM(states) && states[th->state])
        th_state = states[th->state];
      else
        th_state = "???";
      cprintf("%d %d %s %s\n", th->pid, th->thread_id, th_state, p->name);
      if (th->state == SLEEPING) {
        getcallerpcs((uint*)p->context->ebp + 2, pc);
        for (i = 0; i < 10 && pc[i] != 0; i++)
          cprintf(" %p", pc[i]);
      }
      cprintf("\n");
    }

    if (p->state == SLEEPING) {
      getcallerpcs((uint*)p->context->ebp + 2, pc);
      for (i = 0; i < 10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

// Do I really need ptable_idx?
void mvProcToThread(int ptable_idx, struct pthread_t* th) {
  th->th_sz = ptable.proc[ptable_idx].th_sz;
  th->pid = ptable.proc[ptable_idx].pid;
  th->kstack = ptable.proc[ptable_idx].kstack;
  th->state = ptable.proc[ptable_idx].state;
  th->chan = ptable.proc[ptable_idx].chan;
  th->tf = (ptable.proc[ptable_idx].tf);
  th->context = (ptable.proc[ptable_idx].context);
  th->thread_id = ptable.proc[ptable_idx].thread_id;
  th->joiner_pthread = ptable.proc[ptable_idx].joiner_pthread;
  th->ptable_idx = ptable_idx;
}

void clearProc(struct proc* p) {
  if (p->state != ZOMBIE)
    p->state = RUNNABLE;
  p->chan = 0;
}

// Do I really need thread_table_idx?
void mvThreadToProc(int thread_table_idx, struct proc* p) {
  p->th_sz = pthread_table.pthread[thread_table_idx].th_sz;
  p->pid = pthread_table.pthread[thread_table_idx].pid;
  p->kstack = pthread_table.pthread[thread_table_idx].kstack;
  p->state = pthread_table.pthread[thread_table_idx].state;
  p->chan = pthread_table.pthread[thread_table_idx].chan;
  p->tf = (pthread_table.pthread[thread_table_idx].tf);
  p->context = (pthread_table.pthread[thread_table_idx].context);
  p->thread_id = pthread_table.pthread[thread_table_idx].thread_id;
  p->joiner_pthread = pthread_table.pthread[thread_table_idx].joiner_pthread;
  p->thread_idx = thread_table_idx;
}



// fork used thread.
struct pthread_t* pthread_fork(void)
{

  struct pthread_t* nth;
  struct proc* curproc = myproc();

  // Allocate space for thread.
  if ((nth = allocthread()) == 0) {
    return null;
  }
  nth->pid = curproc->pid;
  *nth->tf = *curproc->tf;
  nth->thread_id = ++(curproc->thread_assign);
  nth->ptable_idx = curproc - ptable.proc;
  // Clear %eax so that fork returns 0 in the child.
  nth->tf->eax = 0;

  return nth;
}

int pthread_exec(struct pthread_t* th, thread_t* thread, void* (*start_routine)(void*), void* arg)
{
  uint sz, sp, ustack[1 + MAXARG + 3];
  pde_t* pgdir;
  struct proc* curproc = myproc();

  uint stacksize = curproc->stacksz;
  pgdir = curproc->pgdir;

  // TODO : Stack Array Search
  sz = curproc->sz;
  if (curproc->freed) {
    sz = (curproc->freed_sz[curproc->front]);
  }

  // Allocate two pages at the next page boundary.
  // Make the first inaccessible. Use the second as the user stack.
  sz = PGROUNDUP(sz);
  if (curproc->memlimit != 0 && sz + (stacksize + 1) * PGSIZE > curproc->memlimit) {
    cprintf("exceed memlim : %d\n", curproc->memlimit);
    return -1;
  }
  if ((sz = allocuvm(pgdir, sz, sz + (stacksize + 1) * PGSIZE)) == 0) {
    // revert allocuvm
    return -1;
  }
  clearpteu(pgdir, (char*)(sz - (stacksize + 1) * PGSIZE));
  sp = sz;
  ustack[0] = 0xffffffff;
  sp -= 4;

  ustack[1] = (uint)arg;
  sp -= 4;

  if (copyout(pgdir, sp, ustack, 2 * 4) < 0) {
    return -1;
  }

  if (curproc->freed) {
    // pop Q
    curproc->freed_sz[curproc->front] = 0;
    curproc->freed--;
    curproc->front++;
    curproc->front %= NTHRD;
    curproc->sz = curproc->sz;
  }
  else {
    curproc->sz = sz;
  }
  th->th_sz = sz;
  curproc->pgdir = pgdir;
  th->tf->eip = (uint)start_routine;
  th->tf->esp = sp;
  // The process doesn't have to return to the instruction
  // after exec() when it gets the CPU next,
  // but instead must start executing the new executable it
  // just loaded from dist.
  // exec() changes the return address in the trap frame
  // to point to the entry address of the binary.
  return 0;
}

// Create new thread with {thread}.
// {thread} : dictate thread id.
// {start_routine} : dictate function that is start point of thread.
// {arg} : An argument that passed to {start_routine}.
// return 0, if thread is made successively, otherwise -1.
// fork First, exec Later.
int thread_create(thread_t* thread, void* (*start_routine)(void*), void* arg) {
  struct proc* curproc = myproc();
  acquire(&pthread_table.lock);
  // Allocate Stack for thread Start.
  struct pthread_t* new_thread = pthread_fork();
  if (new_thread == null) {
    release(&pthread_table.lock);
    return -1;
  }
  // Allocate Stack for thread End.
  acquire(&ptable.lock);
  if (pthread_exec(new_thread, thread, start_routine, arg) == -1) {
    goto bad;
  }

  // Setting Thread Variable
  new_thread->pid = curproc->pid;
  // kstack DONE in pthread_fork
  new_thread->state = RUNNABLE;
  // new_thread->chan is not need to be setted.
  new_thread->tf->eax = 0;
  new_thread->tf->eip = (uint)(start_routine);
  // new_thread->context is already setted.
  // new_thread->killed is not need to be setted.
  new_thread->ptable_idx = curproc - ptable.proc;

  new_thread->joiner_pthread = &pthread_table.pthread[curproc->thread_idx];

  *thread = new_thread->thread_id;
  release(&ptable.lock);
  release(&pthread_table.lock);

  return 0;
bad:
  // Unfork thread
  new_thread->state = UNUSED;
  release(&ptable.lock);
  release(&pthread_table.lock);
  return -1;
}

// static void
// thread_wakeup1(void* chan)
// {
//   struct pthread_t* th;
//   for (th = pthread_table.pthread; th < &pthread_table.pthread[NTHRD]; th++)
//     if (th->state == SLEEPING && th->chan == chan)
//       th->state = RUNNABLE;
// }
// Terminate thread and return.
// Every thread should be terminated by this.
// It is not the exit condition that
// thread reached to the end of function.
// {retval} : after terminating thread,
// it will be carried by thread_join.
// If Main Thread calls thread_exit instead of exit(),
// then call kill() to kill process.
void thread_exit(void* retval) {
  struct proc* curproc = myproc();
  acquire(&pthread_table.lock);
  if (curproc->thread_id == MAINTH) {
    kill(curproc->pid);
  }
  else {
    pthread_table.pthread[curproc->thread_idx].retval = retval;
    curproc->state = THZOMBIE;
  }
  acquire(&ptable.lock);
  release(&pthread_table.lock);
  wakeup1(curproc->joiner_pthread);

  sched();
  panic("thread zombie exit");
}

// Wait for teminating {thread},
// return {retval} returned through thread_exit.
// If it was already  terminated, immediately return it.
// After thread is terminated, clean up resources that is allocated to thread.
// {thread} : thread id to be joined.
// {retval} : save the value returned by thread.
// return 0, if successively joined, otherwise -1.
// thread_join can be only called by main thread. => NAH
// Main thread can not be jointed.
int thread_join(thread_t thread, void** retval) {
  struct pthread_t* th;
  int havekids;
  struct proc* curproc = myproc();
  acquire(&pthread_table.lock);
  // // thread_join can be only called by main thread.
  // // Main thread can not be jointed.
  // if (curproc->thread_id != MAINTH || thread == MAINTH) {
  //   release(&pthread_table.lock);
  //   return -1;
  // }
  // Main thread can not be jointed.
  if (thread == MAINTH) {
    release(&pthread_table.lock);
    return -1;
  }
  for (;;) {
    // Scan through table looking for exited children.
    havekids = 0;
    for (th = pthread_table.pthread; th < &pthread_table.pthread[NTHRD]; th++) {
      if (th->pid != curproc->pid || th->thread_id != thread
        || th->joiner_pthread->thread_id != curproc->thread_id)
        continue;
      havekids = 1;
      // Found one.
      if (th->state == THZOMBIE) {
        // Find Threads.
        // Release Threads memory that unexpectly quited.
        th->pid = 0;
        kfree(th->kstack);
        th->kstack = 0;
        th->state = UNUSED;
        th->chan = 0;
        th->ptable_idx = 0;
        th->thread_id = 0;
        th->joiner_pthread = 0;
        *retval = th->retval;
        // push Q
        curproc->freed_sz[curproc->back] = (th->th_sz - (curproc->stacksz + 1) * PGSIZE);
        curproc->back++;
        curproc->back %= NTHRD;
        (curproc->freed)++;
        deallocuvm(curproc->pgdir, th->th_sz, th->th_sz - (curproc->stacksz + 1) * PGSIZE);
        th->th_sz = 0;

        release(&pthread_table.lock);
        return 0;
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || curproc->killed) {
      release(&pthread_table.lock);
      return -1;
    }

    sleep(curproc->joiner_pthread, &pthread_table.lock);
  }
}

// delete Thread.
void deleteThread(struct pthread_t* th) {
  th->th_sz = 0;
  th->pid = 0;
  kfree(th->kstack);
  th->kstack = 0;
  th->state = UNUSED;
  th->chan = 0;
  th->ptable_idx = 0;
  th->thread_id = 0;
  th->joiner_pthread = 0;
  th->retval = 0;
}

// delete Threads left only one.
void deleteThreads(struct proc* p) {
  struct pthread_t* th;
  for (th = pthread_table.pthread; th < &pthread_table.pthread[NTHRD]; th++) {
    // Find the threads that is in process group except who calls exec().
    if (th->pid != p->pid || th->thread_id == p->thread_id)
      continue;
    for (struct proc* child = ptable.proc; child < &ptable.proc[NPROC]; child++) {
      if (child->parent_pthread != th)
        continue;
      // adopt other child process of other sibling threads 
      child->parent_pthread = &(pthread_table.pthread[p->thread_idx]);
    }
    deleteThread(th);
  }
  p->joiner_pthread = &(pthread_table.pthread[p->thread_idx]);
}

void lock(void) {
  acquire(&pthread_table.lock);
}
void unlock(void) {
  release(&pthread_table.lock);
}

void procInfo(void) {
  acquire(&ptable.lock);
  int i;
  struct proc* p;
  int proc_idx[NPROC];
  int num_proc = 0;

  for (i = 0;i < NPROC;i++) { // save ptable index that contains proc.
    if (ptable.proc[i].pid != 0 && (ptable.proc[i].state == RUNNABLE || ptable.proc[i].state == RUNNING || ptable.proc[i].state == SLEEPING)) {
      proc_idx[num_proc++] = i;
    }
  }

  for (i = 0;i < num_proc;i++) {
    for (int j = 1;j < num_proc;j++) {
      if (ptable.proc[proc_idx[j - 1]].pid > ptable.proc[proc_idx[j]].pid) {
        int temp = proc_idx[j - 1];
        proc_idx[j - 1] = proc_idx[j];
        proc_idx[j] = temp;
      }
    }
  }
  //          name: 18            pid 12      stacksz 15             pg 18           memlim 16
  cprintf("|------------------------------Process Information----------------------------------|\n");
  cprintf("|-------name-------|----pid-----|--stack pages--|--current memory--|--memory limit--|\n");
  cprintf("|------------------|------------|---------------|------------------|----------------|\n");

  int len;
  for (i = 0;i < num_proc;i++) {
    p = &(ptable.proc[proc_idx[i]]);
    cprintf("| ");

    char name[55];
    safestrcpy(name, p->name, 55);
    len = 0;
    cprintf("%s", name);
    for (int j = strlen(name) + 1;j < 18;j++) {
      cprintf(" ");
    }
    cprintf("| ");

    int pid = p->pid;
    if (pid == 0) {
      pid = 1;
    }
    len = 0;
    while (pid) {
      pid /= 10;
      len++;
    }
    cprintf("%d", p->pid);
    for (int j = len + 1;j < 12;j++) {
      cprintf(" ");
    }
    cprintf("| ");

    uint stacksz = p->stacksz;
    len = 0;
    while (stacksz) {
      stacksz /= 10;
      len++;
    }
    cprintf("%d", p->stacksz);
    for (int j = len + 1;j < 15;j++) {
      cprintf(" ");
    }
    cprintf("| ");

    uint sz = p->sz;
    len = 0;
    while (sz) {
      sz /= 10;
      len++;
    }

    cprintf("%d", p->sz);
    for (int j = len + 1;j < 18;j++) {
      cprintf(" ");
    }
    cprintf("| ");

    uint memlimit = p->memlimit;
    if (memlimit == 0) {
      memlimit = 1;
    }
    len = 0;
    while (memlimit) {
      memlimit /= 10;
      len++;
    }

    cprintf("%d", p->memlimit);
    for (int j = len + 1;j < 16;j++) {
      cprintf(" ");
    }
    cprintf("| ");
    cprintf("\n");
  }
  cprintf("|------------------|------------|---------------|------------------|----------------|\n");

  release(&ptable.lock);
}