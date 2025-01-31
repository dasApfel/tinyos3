
#include <assert.h>
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_streams.h"
#include "tinyos.h"


/* 
 The process table and related system calls:
 - Exec
 - Exit
 - WaitPid
 - GetPid
 - GetPPid

 */

/* The process table */
PCB PT[MAX_PROC];
unsigned int process_count;

PCB* get_pcb(Pid_t pid)
{
  return PT[pid].pstate==FREE ? NULL : &PT[pid];
}

Pid_t get_pid(PCB* pcb)
{
  return pcb==NULL ? NOPROC : pcb-PT;
}

/* Initialize a PCB */
static inline void initialize_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->argl = 0;
  pcb->args = NULL;

  for(int i=0;i<MAX_FILEID;i++)
    pcb->FIDT[i] = NULL;

  rlnode_init(& pcb->children_list, NULL);
  rlnode_init(& pcb->exited_list, NULL);
  rlnode_init(& pcb->children_node, pcb);
  rlnode_init(& pcb->exited_node, pcb);
  pcb->child_exit = COND_INIT;

  //Addition below - 3 lines

  rlnode_new(&pcb->thread_list); // each PCB should now point to a list of PTCBs
  pcb->aCond = COND_INIT;
  pcb->threadCount = 0;
}


static PCB* pcb_freelist;

void initialize_processes()
{
  /* initialize the PCBs */
  for(Pid_t p=0; p<MAX_PROC; p++) {
    initialize_PCB(&PT[p]);
  }

  /* use the parent field to build a free list */
  PCB* pcbiter;
  pcb_freelist = NULL;
  for(pcbiter = PT+MAX_PROC; pcbiter!=PT; ) {
    --pcbiter;
    pcbiter->parent = pcb_freelist;
    pcb_freelist = pcbiter;
  }

  process_count = 0;

  /* Execute a null "idle" process */
  if(Exec(NULL,0,NULL)!=0)
    FATAL("The scheduler process does not have pid==0");
}


/*
  Must be called with kernel_mutex held
*/
PCB* acquire_PCB()
{
  PCB* pcb = NULL;

  if(pcb_freelist != NULL) {
    pcb = pcb_freelist;
    pcb->pstate = ALIVE;
    pcb_freelist = pcb_freelist->parent;
    process_count++;
  }

  return pcb;
}

/*
  Must be called with kernel_mutex held
*/
void release_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->parent = pcb_freelist;
  pcb_freelist = pcb;
  process_count--;
}


/*
 *
 * Process creation
 *
 */

/*
	This function is provided as an argument to spawn,
	to execute the main thread of a process.
*/
void start_main_thread()
{
  PTCB* aptcb = (PTCB *)sys_ThreadSelf();
  int exitval;

  Task call =  aptcb->main_task;
  int argl = aptcb->argl;
  void* args = aptcb->args;

  exitval = call(argl,args);
  Exit(exitval);
}


/*
	System call to create a new process.
 */
Pid_t sys_Exec(Task call, int argl, void* args)
{
  PCB *curproc, *newproc;
  
  /* The new process PCB */
  newproc = acquire_PCB();

  if(newproc == NULL) goto finish;  /* We have run out of PIDs! */

  if(get_pid(newproc)<=1) {
    /* Processes with pid<=1 (the scheduler and the init process) 
       are parentless and are treated specially. */
    newproc->parent = NULL;
  }
  else
  {
    /* Inherit parent */
    curproc = CURPROC;

    /* Add new process to the parent's child list */
    newproc->parent = curproc;
    rlist_push_front(& curproc->children_list, & newproc->children_node);

    /* Inherit file streams from parent */
    for(int i=0; i<MAX_FILEID; i++) {
       newproc->FIDT[i] = curproc->FIDT[i];
       if(newproc->FIDT[i])
          FCB_incref(newproc->FIDT[i]);
    }
  }


  /* Set the main thread's function */
  newproc->main_task = call;

  /* Copy the arguments to new storage, owned by the new process */
  newproc->argl = argl;

  if(args!=NULL) {
    newproc->args = malloc(argl);
    memcpy(newproc->args, args, argl);
  }
  else
    newproc->args=NULL;

  /* 
    Create and wake up the thread for the main function. This must be the last thing
    we do, because once we wakeup the new thread it may run! so we need to have finished
    the initialization of the PCB.
   */
 
  //Addition below - 20 lines . Implemented the support to PTCB in initialisation.

  newproc->threadCount ++; //Increase the count of Threads assosciated with this process.

  PTCB *ptcb = (PTCB *)malloc(sizeof(PTCB)); //dynamically allocate space for a PTCB object
  ptcb->main_task = call;
  ptcb->argl = argl;
  ptcb->isDetached=0;
  ptcb->hasExited=0;
  ptcb->refCounter=0;
  ptcb->cVar = COND_INIT;
  ptcb->exitFlag=1; //define the flag for the main_thread

  //Copy the arguments to storage under PTCB's control.

  if(args != NULL) 
  {
    ptcb->args = malloc(argl);  //allocate storage
    memcpy(ptcb->args, args, argl); //copy content
  }
  else
  { 
    ptcb->args=NULL;
  }

  rlnode *ptcbNode= rlnode_init(&ptcb->aNode, ptcb);
  rlist_push_back(&newproc->thread_list, ptcbNode);

  if(call != NULL) {
    ptcb->thread =spawn_thread(newproc,start_main_thread);
    wakeup(ptcb->thread);
  }


finish:
  return get_pid(newproc);
}


/* System call */
Pid_t sys_GetPid()
{
  return get_pid(CURPROC);
}


Pid_t sys_GetPPid()
{
  return get_pid(CURPROC->parent);
}


static void cleanup_zombie(PCB* pcb, int* status)
{
  if(status != NULL)
    *status = pcb->exitval;

  rlist_remove(& pcb->children_node);
  rlist_remove(& pcb->exited_node);

  release_PCB(pcb);
}


static Pid_t wait_for_specific_child(Pid_t cpid, int* status)
{

  /* Legality checks */
  if((cpid<0) || (cpid>=MAX_PROC)) {
    cpid = NOPROC;
    goto finish;
  }

  PCB* parent = CURPROC;
  PCB* child = get_pcb(cpid);
  if( child == NULL || child->parent != parent)
  {
    cpid = NOPROC;
    goto finish;
  }

  /* Ok, child is a legal child of mine. Wait for it to exit. */
  while(child->pstate == ALIVE)
    kernel_wait(& parent->child_exit, SCHED_USER);
  
  cleanup_zombie(child, status);
  
finish:
  return cpid;
}


static Pid_t wait_for_any_child(int* status)
{
  Pid_t cpid;

  PCB* parent = CURPROC;

  /* Make sure I have children! */
  if(is_rlist_empty(& parent->children_list)) {
    cpid = NOPROC;
    goto finish;
  }

  while(is_rlist_empty(& parent->exited_list)) {
    kernel_wait(& parent->child_exit, SCHED_USER);
  }

  PCB* child = parent->exited_list.next->pcb;
  assert(child->pstate == ZOMBIE);
  cpid = get_pid(child);
  cleanup_zombie(child, status);

finish:
  return cpid;
}


Pid_t sys_WaitChild(Pid_t cpid, int* status)
{
  /* Wait for specific child. */
  if(cpid != NOPROC) {
    return wait_for_specific_child(cpid, status);
  }
  /* Wait for any child */
  else {
    return wait_for_any_child(status);
  }

}


/*
Refactored to act as a footstep - caller to sys_ThreadExit which will actually handle the exit process
-"Such a pitty,those threads were good kids, never deserved to be called zombies after that! :("

*/

void sys_Exit(int exitval)
{
  /* Right here, we must check that we are not the boot task. If we are, 
     we must wait until all processes exit. */
  if(sys_GetPid()==1) {
    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);
  }


  //just assign the exitval to the current process
  CURPROC->exitval = exitval;

  //then call the threadExit to play with each thread's exit, this function is our bully
  sys_ThreadExit(exitval);
}


//Define an info block to use later on

static file_ops info_module = {
    .Open = NULL,
    .Read = info_read,
    .Write = NULL,
    .Close = info_close
};




//read an info block

int info_read(void *infoCB, char *buf, unsigned int size) 
{


  unsigned int pos=0;

  //caching and stuff like that
  InfoCB *infocb = (InfoCB *) infoCB;

 
  while (pos<size && infocb->read_pos < infocb->write_pos)
  {
        buf[pos] = infocb->infoTable[infocb->read_pos];
        pos++;
        infocb->read_pos++;
  }
  return pos;

}




//close the stream

int info_close(void *infoCB) 
{
    free(infoCB);
  return 0;
}


//syscall

Fid_t sys_OpenInfo()
{


  Fid_t fid;
  FCB *fcb;


  if (!FCB_reserve(1, &fid, &fcb))
  {
    return NOFILE;
  }

  //allocate control block space and initialize

  InfoCB *infoCB = (InfoCB *) xmalloc(sizeof(InfoCB));
  infoCB->read_pos = 0;
  infoCB->write_pos = 0;
  fcb->streamobj = infoCB;
  fcb->streamfunc = &info_module;

  procinfo *info = (procinfo *) xmalloc(sizeof(procinfo));
  

  for (int i = 0; i < MAX_PROC; i++) {
    PCB *pcb = &PT[i];
    if (pcb->pstate == ALIVE || pcb->pstate == ZOMBIE) 
    {
         info->pid = get_pid(&PT[i]);
         info->ppid = get_pid(pcb->parent);
         info->alive = pcb->pstate == ALIVE;
         info->thread_count = pcb->threadCount ;
         info->main_task = pcb->main_task;
         info->argl = pcb->argl;
    
         for (int j = 0; j < info->argl; j++) 
         {
            info->args[j] = ((char *) pcb->args)[j];
         }
         
         memcpy(&infoCB->infoTable[infoCB->write_pos], info, sizeof(procinfo));
         
         infoCB->write_pos = infoCB->write_pos + sizeof(procinfo);
    }
  }
  return fid;
}


