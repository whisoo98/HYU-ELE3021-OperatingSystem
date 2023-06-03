// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context* scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc* proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;     // Register to save the dest data address
  uint esi;     // Register to save the source data address
  uint ebx;     // Register to save memory address
  uint ebp;     // Register of Frame Pointer
  uint eip;     // Program Counter(PC)
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE, THZOMBIE };

// Per-process state
// struct that respresent Process (Main Thread)
struct proc {
  // Shared Variable
  pde_t* pgdir;               // Page table
  char name[55];              // Process name (debugging)
  struct proc* parent_proc;   // Parent process who calls fork()
  struct pthread_t*
    parent_pthread;           // Parent pthread who calls fork()
  struct inode* cwd;          // Current directory
  struct file* ofile[NOFILE]; // Open files
  uint memlimit;              // Memory limitation (default 0)
  uint thread_assign;         // Thread ID to Assign (start to 1)
  uint stacksz;               // # of Stack Page (default 1)

  // process Variable
  uint freed_sz[NTHRD];       // Queue for freed Stack. LOL
  int front;                  // front of Q
  int back;                   // back of Q
  int freed;                  // size of Q
  int pid;                    // Process ID
  uint sz;                    // Size of Thread memory (bytes)
  int main_thread_idx;        // pthread table index of main thread

  // Thread Variable
  uint th_sz;                 // Size of Thread memory (bytes)
  char* kstack;               // Bottom of kernel stack for this process
  enum procstate state;       // Process state
  void* chan;                 // If non-zero, sleeping on chan 
  struct trapframe* tf;       // Trap frame for current syscall
  struct context* context;    // swtch() here to run process
  int killed;                 // If non-zero, have been killed
  thread_t thread_id;         // thread ID
  struct pthread_t*
    joiner_pthread;           // Parent pthread who calls thread_create()
  int thread_idx;             // Thread table index
};


struct pthread_t {
  uint th_sz;                 // Size of Thread memory (bytes)
  int pid;                    // process ID owned thread
  char* kstack;               // Bottom of kernel stack for this process
  enum procstate state;       // Process state
  void* chan;                 // If non-zero, sleeping on chan
  struct trapframe* tf;       // Trap frame for current syscall
  struct context* context;    // swtch() here to run process
  int ptable_idx;             // Proc table index
  thread_t thread_id;         // thread ID
  struct pthread_t*
    joiner_pthread;           // Parent pthread who calls thread_create()
  void* retval;               // Save retval to return in thread_join
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
