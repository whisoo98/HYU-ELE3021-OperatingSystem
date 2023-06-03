#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "defs.h"
#include "x86.h"
#include "elf.h"




int
exec(char* path, char** argv)
{
  char* s, * last;
  int i, off;
  uint argc, sz, sp, ustack[3 + MAXARG + 1];
  struct elfhdr elf;
  struct inode* ip;
  struct proghdr ph;
  pde_t* pgdir, * oldpgdir;
  int locked = 0;
  struct proc* curproc = myproc();
  begin_op();

  if ((ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);
  pgdir = 0;

  // curproc must be scheduled.
  curproc->state = RUNNING;

  // Check ELF header
  if (readi(ip, (char*)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if (elf.magic != ELF_MAGIC)
    goto bad;

  // allocate pgdir using kalloc() in setupkvm().
  // pgdir set to 0.
  if ((pgdir = setupkvm()) == 0)
    goto bad;

  // Load program into memory.
  sz = 0;
  for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
    if (readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if (ph.type != ELF_PROG_LOAD)
      continue;
    if (ph.memsz < ph.filesz)
      goto bad;
    if (ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    // expand pgidr size sz -> ph.vaddr + ph.memsz
    // and return ph.vaddr + ph.memsz
    if ((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    if (ph.vaddr % PGSIZE != 0)
      goto bad;
    // loaduvm to load executable binary code.
    if (loaduvm(pgdir, (char*)ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
    // ERROR
  }
  iunlockput(ip);
  end_op();
  // lock the exec().
  lock();
  locked = 1;
  ip = 0;
  // Allocate two pages at the next page boundary.
  // Make the first inaccessible.  Use the second as the user stack.
  sz = PGROUNDUP(sz);
  if ((sz = allocuvm(pgdir, sz, sz + 2 * PGSIZE)) == 0)
    goto bad;
  clearpteu(pgdir, (char*)(sz - 2 * PGSIZE));
  sp = sz;

  // Push argument strings, prepare rest of stack in ustack.
  for (argc = 0; argv[argc]; argc++) {
    if (argc >= MAXARG)
      goto bad;
    sp = (sp - (strlen(argv[argc]) + 1)) & ~3;
    if (copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[3 + argc] = sp;
  }
  ustack[3 + argc] = 0;

  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = argc;
  ustack[2] = sp - (argc + 1) * 4;  // argv pointer

  sp -= (3 + argc + 1) * 4;
  // copy ustack into pgdir
  if (copyout(pgdir, sp, ustack, (3 + argc + 1) * 4) < 0)
    goto bad;

  // Save program name for debugging.
  for (last = s = path; *s; s++)
    if (*s == '/')
      last = s + 1;
  safestrcpy(curproc->name, last, sizeof(curproc->name));

  // Clean up other sibling threads.
  deleteThreads(curproc);
  memset(curproc->freed_sz, 0, sizeof(curproc->freed_sz));
  curproc->front = 0;
  curproc->back = 0;
  curproc->freed = 0;

  // Commit to the user image.
  oldpgdir = curproc->pgdir;
  curproc->pgdir = pgdir;
  curproc->sz = sz;
  curproc->th_sz = sz; // thread_sz
  curproc->memlimit = 0;      // default memlimit
  curproc->thread_assign = 1; // default thread_assign
  curproc->stacksz = 1;       // default stacksz
  curproc->thread_id = MAINTH;
  curproc->main_thread_idx = curproc->thread_idx;
  // The process doesn't have to return to the instruction
  // after exec() when it gets the CPU next,
  // but instead must start executing the new executable it
  // just loaded from dist.
  // exec() changes the return address in the trap frame
  // to point to the entry address of the binary.
  curproc->tf->eip = elf.entry;
  curproc->tf->esp = sp;
  unlock();
  switchuvm(curproc);
  freevm(oldpgdir);
  return 0;

bad:
  if (pgdir)
    freevm(pgdir);
  if (ip) {
    iunlockput(ip);
    end_op();
  }
  if (locked)
    unlock();
  return -1;
}


// System call that make process be allocated several stack pages.
// {path}, {argv} is same with exec.
// {stacksize} : [1,100] in N.
// A guardpage should be allocated regardless to the {stacksize},
// be located under logical address of stack pages.
// TODO : clean up threads
int exec2(char* path, char** argv, int stacksize) {
  char* s, * last;
  int i, off;
  uint argc, sz, sp, ustack[3 + MAXARG + 1];
  struct elfhdr elf;
  struct inode* ip;
  struct proghdr ph;
  pde_t* pgdir, * oldpgdir;
  int locked = 0;
  struct proc* curproc = myproc();
  if (stacksize < 1 || stacksize>100) {
    cprintf("Execute Fail : stacksize should be in range of [1,100]\n");
    return -1;
  }
  begin_op();

  if ((ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);
  pgdir = 0;

  // curproc must be scheduled.
  curproc->state = RUNNING;

  // Check ELF header
  if (readi(ip, (char*)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if (elf.magic != ELF_MAGIC)
    goto bad;

  if ((pgdir = setupkvm()) == 0)
    goto bad;

  // Load program into memory.
  sz = 0;
  for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
    if (readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if (ph.type != ELF_PROG_LOAD)
      continue;
    if (ph.memsz < ph.filesz)
      goto bad;
    if (ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if ((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    if (ph.vaddr % PGSIZE != 0)
      goto bad;
    if (loaduvm(pgdir, (char*)ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  lock();
  locked = 1;
  ip = 0;

  // Allocate two pages at the next page boundary.
  // Make the first inaccessible.  Use the second as the user stack.
  sz = PGROUNDUP(sz);
  if ((sz = allocuvm(pgdir, sz, sz + (stacksize + 1) * PGSIZE)) == 0)
    goto bad;
  clearpteu(pgdir, (char*)(sz - (stacksize + 1) * PGSIZE));
  sp = sz;

  // Push argument strings, prepare rest of stack in ustack.
  for (argc = 0; argv[argc]; argc++) {
    if (argc >= MAXARG)
      goto bad;
    sp = (sp - (strlen(argv[argc]) + 1)) & ~3;
    if (copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[3 + argc] = sp;
  }
  ustack[3 + argc] = 0;

  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = argc;
  ustack[2] = sp - (argc + 1) * 4;  // argv pointer

  sp -= (3 + argc + 1) * 4;
  if (copyout(pgdir, sp, ustack, (3 + argc + 1) * 4) < 0)
    goto bad;

  // Save program name for debugging.
  for (last = s = path; *s; s++)
    if (*s == '/')
      last = s + 1;
  safestrcpy(curproc->name, last, sizeof(curproc->name));

  // Clean up other sibling threads.
  deleteThreads(curproc);
  memset(curproc->freed_sz, 0, sizeof(curproc->freed_sz));
  curproc->front = 0;
  curproc->back = 0;
  curproc->freed = 0;

  // Commit to the user image.
  oldpgdir = curproc->pgdir;
  curproc->pgdir = pgdir;
  curproc->sz = sz;    // proc_sz
  curproc->th_sz = sz; // thread_sz
  curproc->tf->eip = elf.entry;  // main
  curproc->tf->esp = sp;
  curproc->memlimit = 0; // newly exec
  curproc->thread_assign = 1; // default thread_assign
  curproc->stacksz = stacksize;       // default stackszuted process
  curproc->thread_id = MAINTH;
  curproc->main_thread_idx = curproc->thread_idx;
  unlock();
  switchuvm(curproc);
  freevm(oldpgdir);

  return 0;

bad:
  if (pgdir)
    freevm(pgdir);
  if (ip) {
    iunlockput(ip);
    end_op();
  }
  if (locked)
    unlock();
  return -1;
}

