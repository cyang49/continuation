#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <string.h>
#include <setjmp.h>

#include "ocr.h"
#define ENABLE_EXTENSION_RTITF
#include "extensions/ocr-runtime-itf.h"
#define ENABLE_EXTENSION_LABELING
#include "extensions/ocr-labeling.h"
#include "ocr-std.h"

#define NP (4)
#define NUM_SLAVES (4)
#define DB_STACK_SIZE (8*1024*1024) // 8 MB for now
#define RED_ZONE_SIZE (0)
#define NUM_CALLEE_SAVED_REGS (6)

#define MAX_NUM_PHASE (256) 
#define MAX_NUM_SLOTS (64)

#define HTA_OP_FINISHED (0)
#define HTA_OP_TO_BE_CONTINUED (1)


#define HTA_PARALLEL_OP(func_call) \
    if((func_call) != HTA_OP_FINISHED) { \
        printf("hta operation split\n"); \
        return 1; \
    }

#define MYRANK ((u64) ocrElsUserGet(0))

#ifdef __cplusplus
extern "C" {
    ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]);
}
#endif

typedef struct {
    char        stack[DB_STACK_SIZE];                       // the stack
    unsigned int phase;                                     // the new phase number
    char*       originalbp;                                 // the original thread base pointer
    char*       originalsp;                                 // the original thread stack pointer
    ocrGuid_t   args;                                       // the command line arguments
    ocrGuid_t   self_context_guid;                          // the DB guid of the context 
    ocrGuid_t   next_phase_edt_guid;                        // store the guid of the next phase for deferred activation
    ocrGuid_t   shutdown_edt_guid;                          // guid of the shutdown EDT
    ocrGuid_t   type1Map;                                   // map of a procEdt phase to an EDT guid 
    ocrGuid_t   type2Map;                                   // map slot of a procEdt phase to an event guid
    unsigned int comm_event[NP*NP];                         // to calculate communication events between two EDTs
    unsigned int DBs_cont[NP];                              // number of DBs stored in procEDT (global view)
    unsigned int DBs_recv;                                  // number of DBs received (continuation)
    ocrEdtDep_t  DBs[MAX_NUM_PHASE];                        // DBs 
    jmp_buf     env;                                        // the env data structure used in setjmp/longjmp
    long int    callee_saved[NUM_CALLEE_SAVED_REGS];        // separate storage location for callee saved register values
} Context;

ocrGuid_t procEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t slaveEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]);

ocrGuid_t slaveEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    printf("map is executed\n");
    return NULL_GUID; // satisfies control dependency of the continuation codelet
}

// This function fix pointers that points to thread stack addresses
void _fix_pointers(void* db_bp, void *thread_bp, void* max_thread_bp) {
    while(thread_bp < max_thread_bp) {
        size_t frame_size = *((char**)db_bp) - (char*)db_bp;
        *((void**)thread_bp) = thread_bp + frame_size;
        thread_bp += frame_size;
        db_bp += frame_size;
    }
}

int hta_map(int pid, Context* context)
{
    register char * const basepointer __asm("rbp");
    register char * const stackpointer __asm("rsp");

    //======================================================================
    // perform map
    //======================================================================

    // create slave EDTs
    ocrGuid_t slaveEdt_template_guid;
    ocrGuid_t slaveEdts[NUM_SLAVES];
    ocrGuid_t slaveOutEvent[NUM_SLAVES];
    ocrGuid_t slaveInDBs[NUM_SLAVES];

    ocrEdtTemplateCreate(&slaveEdt_template_guid, slaveEdt, 0, 2);
    printf("(%lu) slaveEDT template guid %lx\n", MYRANK, slaveEdt_template_guid);
    for(int i = 0; i < NUM_SLAVES; i++) {
        int *data;
        slaveOutEvent[i] = NULL_GUID;
        ocrDbCreate(&slaveInDBs[i], (void**) &data, sizeof(int), /*flags=*/DB_PROP_NO_ACQUIRE, /*affinity=*/NULL_GUID, NO_ALLOC);
        ocrEdtCreate(&slaveEdts[i], slaveEdt_template_guid, /*paramc=*/0, /*paramv=*/(u64 *)NULL, /*depc=*/2, /*depv=*/NULL, 
		     /*properties=*/0 , /*affinity*/NULL_GUID,  &slaveOutEvent[i]);
        ocrAddDependence(slaveInDBs[i], slaveEdts[i], 1, DB_DEFAULT_MODE); // Immediately satisfy
        printf("(%lu) slave %d EDT guid %lx\n", MYRANK, i, slaveEdts[i]);
        printf("(%lu) slave %d out event guid %lx\n", MYRANK, i, slaveOutEvent[i]);
    }
    ocrEdtTemplateDestroy(slaveEdt_template_guid);

    //======================================================================
    // Create continuation to wait for slave EDTs to finish
    //======================================================================
    unsigned int DBslots = context->DBs_cont[MYRANK];
    ocrGuid_t procEdt_template_guid;
    ocrGuid_t procEdt_guid;
    ocrEdtTemplateCreate(&procEdt_template_guid, procEdt, 1, 3+NUM_SLAVES);
    
    u64 rank = MYRANK; 
    ocrGuid_t depv[3+NUM_SLAVES];
    depv[0] = UNINITIALIZED_GUID;
    depv[1] = context->args;
    depv[2] = context->self_context_guid;
    for(int i = 0; i < DBslots; i++) {
	int *data = (int *)context->DBs[i].ptr;
	printf("(%lu) DB guid %lx in continuation procEDT - value %d\n", MYRANK, context->DBs[i].guid, *data);
    }
    for(int i = 0; i < NUM_SLAVES; i++)
	depv[3+i] = slaveOutEvent[i];
    
    ocrEdtCreate(&procEdt_guid, procEdt_template_guid, /*paramc=*/1, /*paramv=*/(u64 *)&rank, /*depc=*/EDT_PARAM_DEF, 
		 /*depv=*/depv, /*properties=*/0 , /*affinity*/NULL_GUID,  /*outputEvent*/NULL );
    printf("(%lu) continuation procEDT template guid %lx\n", MYRANK, procEdt_template_guid);
    printf("(%lu) continuation procEDT guid %lx\n", MYRANK, procEdt_guid);
    printf("(%lu) continuation procEDT DB slots %x\n", MYRANK, DBslots);
    
    // defer firing slave EDTs
    for(int i = 0; i < NUM_SLAVES; i++) {
        ocrAddDependence(NULL_GUID, slaveEdts[i], 0, DB_DEFAULT_MODE); //pure control dependence // Immediately satisfy
    }
    ocrEdtTemplateDestroy(procEdt_template_guid);
    
    // setjmp call creates a continuation point
    if(!setjmp(context->env)) 
    {
        // switch back to thread stack
        // 1. compute the size that need to be copied (the growth of DB stack)
        size_t size_to_copy = (context->stack + DB_STACK_SIZE) - stackpointer;
        printf("(%lu) db stack (%p - %p) stack size growth = 0x%x\n", MYRANK, stackpointer, context->stack+DB_STACK_SIZE-1, size_to_copy);
        // 2. compute the start address of the thread stack
        char* originalbp = context->originalbp;
        char* threadsp = originalbp - size_to_copy;
        char* threadbp = threadsp + (basepointer-stackpointer);
        // 3. copy DB stack to overwrite thread stack
        printf("(%lu) Enabling continuation codelet\n", MYRANK);
        printf("(%lu) switching back to thread stack at (%p - %p) original bp (%p)\n", MYRANK, threadsp, threadbp, originalbp);
        memcpy(threadsp - RED_ZONE_SIZE, stackpointer - RED_ZONE_SIZE, size_to_copy + RED_ZONE_SIZE);
        // 4. fix frame link addresses 
        _fix_pointers(basepointer, threadbp, originalbp);
        // 5. store the next phase EDT guid for deferred activation
        context->next_phase_edt_guid = procEdt_guid;
        // 6. set rsp and rbp to point to thread stack. Stop writing to context->stack
        __asm volatile(
            "movq %0, %%rbp;"
            "movq %1, %%rsp;"
            :
            :"r"(threadbp), "r"(threadsp)
           );

        printf("(%lu) ==hta_map splited==\n", MYRANK);
        return HTA_OP_TO_BE_CONTINUED;
    }
    else
    {
        // Continuation should start from here
        printf("(%lu) hta_map is continued\n", MYRANK);       
        // Stack pointer/base pointer are not changed here because it will
        // keep using context->stack
        return HTA_OP_FINISHED;
    }
}

int comm_send(int src, void *data, int size, int dest, Context* context)
{
    register char * const basepointer __asm("rbp");
    register char * const stackpointer __asm("rsp");
       
    if(MYRANK == src) { // Sender
	
	// Get event guid to send data
	ocrGuid_t EventSendOut_guid = NULL_GUID;
	s64 tuple[] = {src, dest, context->comm_event[src*NP+dest]};
	ocrGuidFromLabel(&EventSendOut_guid, context->type2Map, tuple);
	printf("(%lu) Sending event guid %lx\n", MYRANK, EventSendOut_guid);
	printf("(%lu) Map1 guid %lx\n", MYRANK, context->type2Map);  
	
	int *buffer;
 	ocrGuid_t sendDB;
 	ocrDbCreate(&sendDB, (void**) &buffer, size * sizeof(int), /*flags=*/DB_DEFAULT_MODE, 
		    /*affinity=*/NULL_GUID, NO_ALLOC);
 	
	*buffer = *(int *)data;
	printf("(%lu) Sending DB guid %lx - value %d\n", MYRANK, sendDB, *buffer);
	
	// create next-event to communicate with dest
	context->comm_event[src*NP+dest]++;
	ocrGuid_t nextEventSendOut_guid = NULL_GUID;
	tuple[0] = src;
	tuple[1] = dest;
	tuple[2] = context->comm_event[src*NP+dest];
	ocrGuidFromLabel(&nextEventSendOut_guid, context->type2Map, tuple);
	ocrEventCreate(&nextEventSendOut_guid, OCR_EVENT_STICKY_T, GUID_PROP_IS_LABELED | EVT_PROP_NONE);
	printf("(%lu) Next sending event guid %lx was created\n", MYRANK, nextEventSendOut_guid);
	
	context->DBs_cont[dest]++; // a new DB will be sent, update counter
	context->phase++;
	
	// satisfy current communication event to send DB 
	ocrEventSatisfy(EventSendOut_guid, sendDB);
	
	printf("(%lu) ==========Phase %d starts===========\n", MYRANK, context->phase);
	    
	return HTA_OP_FINISHED;
	
    }
    else if (MYRANK == dest) { // Receiver
	ocrGuid_t EventSendOut_guid = NULL_GUID;
	s64 tuple[] = {src, dest, context->comm_event[src*NP+dest]};
	ocrGuidFromLabel(&EventSendOut_guid, context->type2Map, tuple);
 	printf("(%lu) Receiving event guid %lx\n", MYRANK, EventSendOut_guid);
	printf("(%lu) Map1 guid %lx\n", MYRANK, context->type2Map);
	
	//======================================================================
	// Create continuation to wait for DB sent
	//======================================================================
//	unsigned int DBslots = context->DBs_cont[MYRANK];
	ocrGuid_t procEdt_template_guid;
	ocrGuid_t procEdt_guid;
	ocrEdtTemplateCreate(&procEdt_template_guid, procEdt, 1, 4);
	
	u64 rank = MYRANK; 
	ocrGuid_t depv[4];
	depv[0] = UNINITIALIZED_GUID;
	depv[1] = context->args;
	depv[2] = context->self_context_guid;
	depv[3] = EventSendOut_guid;
	
	context->DBs_recv = 1;     // new DB sent, update counter continuation	
	context->comm_event[src*NP+dest]++; // move to the next communication event
	
 	ocrEdtCreate(&procEdt_guid, procEdt_template_guid, /*paramc=*/1, /*paramv=*/(u64 *)&rank, /*depc=*/EDT_PARAM_DEF, 
 		    /*depv=*/depv, /*properties=*/0 , /*affinity*/NULL_GUID,  /*outputEvent*/NULL);
	printf("(%lu) continuation procEDT template guid %lx\n", MYRANK, procEdt_template_guid);
	printf("(%lu) continuation procEDT guid %lx\n", MYRANK, procEdt_guid);
//	printf("(%lu) continuation procEDT DB slots %x\n", MYRANK, DBslots);
	
	ocrEdtTemplateDestroy(procEdt_template_guid);
	
	// setjmp call creates a continuation point
	if(!setjmp(context->env)) 
	{
	    // switch back to thread stack
	    // 1. compute the size that need to be copied (the growth of DB stack)
	    size_t size_to_copy = (context->stack + DB_STACK_SIZE) - stackpointer;
	    printf("(%lu) db stack (%p - %p) stack size growth = 0x%x\n", MYRANK, stackpointer, context->stack+DB_STACK_SIZE-1, size_to_copy);
	    // 2. compute the start address of the thread stack
	    char* originalbp = context->originalbp;
	    char* threadsp = originalbp - size_to_copy;
	    char* threadbp = threadsp + (basepointer-stackpointer);
	    // 3. copy DB stack to overwrite thread stack
	    printf("(%lu) Enabling continuation codelet\n", MYRANK);
	    printf("(%lu) switching back to thread stack at (%p - %p) original bp (%p)\n", MYRANK, threadsp, threadbp, originalbp);
	    memcpy(threadsp - RED_ZONE_SIZE, stackpointer - RED_ZONE_SIZE, size_to_copy + RED_ZONE_SIZE);
	    // 4. fix frame link addresses 
	    _fix_pointers(basepointer, threadbp, originalbp);
	    // 5. store the next phase EDT guid for deferred activation
	    context->next_phase_edt_guid = procEdt_guid;
	    // 6. set rsp and rbp to point to thread stack. Stop writing to context->stack
	    __asm volatile(
		"movq %0, %%rbp;"
		"movq %1, %%rsp;"
		:
		:"r"(threadbp), "r"(threadsp)
	      );

	    printf("(%lu) ==comm_send splited==\n", MYRANK);
	    return HTA_OP_TO_BE_CONTINUED;
	}
	else
	{
	    // Destroy previously used communication event
	    s64 tuple2[] = {src, dest, context->comm_event[src*NP+dest]-1};
	    ocrGuidFromLabel(&EventSendOut_guid, context->type2Map, tuple2);
	    ocrEventDestroy(EventSendOut_guid);
	    // Continuation should start from here
	    printf("(%lu) comm_send is continued\n", MYRANK);
	    // Stack pointer/base pointer are not changed here because it will
	    // keep using context->stack
	    return HTA_OP_FINISHED;
      }
    }
    else { // Others
      context->DBs_cont[dest]++; // a new DB will be sent, update counter
      context->phase++;
      
      printf("(%lu) ==========Phase %d starts===========\n", MYRANK, context->phase);
      
      return HTA_OP_FINISHED;
    }
}

int hta_main(int argc, char** argv, int pid, Context* context)
{
    int some_stack_variable = -111;
    int other_stack_variable = 202;

    
    // call a parallel operation 
    HTA_PARALLEL_OP(hta_map(pid, context)); // slave codelets are created in here

    printf("(%lu) some stack variable = %d, other stack variable = %d\n", MYRANK, some_stack_variable, other_stack_variable);
    // some computation
    some_stack_variable = -222;
    other_stack_variable = 404;
    
    // send data  0 --> 1
    HTA_PARALLEL_OP(comm_send(0, &some_stack_variable, 1, 1, context)); 
   
    // send data  2 --> 3
    HTA_PARALLEL_OP(comm_send(2, &other_stack_variable, 1, 3, context));
   
    // call a second parallel operation
    HTA_PARALLEL_OP(hta_map(pid, context)); // slave codelets are created in here

    printf("(%lu) some stack variable = %d, other stack variable = %d\n", MYRANK, some_stack_variable, other_stack_variable);
    // some computation
    some_stack_variable = -333;
    other_stack_variable = 808;
    
    // send data  0 --> 1
    HTA_PARALLEL_OP(comm_send(0, &other_stack_variable, 1, 1, context));
    
    // send data  2 --> 3
    HTA_PARALLEL_OP(comm_send(2, &some_stack_variable, 1, 3, context));
    
    // call a second parallel operation
    HTA_PARALLEL_OP(hta_map(pid, context)); // slave codelets are created in here

    printf("(%lu) some stack variable = %d, other stack variable = %d\n", MYRANK, some_stack_variable, other_stack_variable);

    printf("(%lu) hta_main() finishing\n", MYRANK);

    // must restore thread stack before going back to normal execution
    {
        register char * const basepointer __asm("rbp");
        register char * const stackpointer __asm("rsp");
        // switch back to thread stack
        // 1. compute the size that need to be copied (the growth of DB stack)
        size_t size_to_copy = (context->stack + DB_STACK_SIZE) - stackpointer;
        printf("(%lu) stack size growth = 0x%x\n", MYRANK, size_to_copy);
        // 2. compute the start address of the thread stack
        char* originalbp = context->originalbp;
        char* threadsp = originalbp - size_to_copy;
        char* threadbp = threadsp + (basepointer-stackpointer);
        // 3. copy DB stack to overwrite thread stack
        printf("(%lu) switching back to thread stack at (%p - %p) original bp (%p)\n", MYRANK, threadsp, threadbp, originalbp);
        memcpy(threadsp - RED_ZONE_SIZE, stackpointer - RED_ZONE_SIZE, size_to_copy + RED_ZONE_SIZE);
        // 4. fix frame link addresses 
        _fix_pointers(basepointer, threadbp, originalbp);
        // 5. set rsp and rbp to point to thread stack. Stop writing to context->stack
        __asm volatile(
            "movq %0, %%rbp;"
            "movq %1, %%rsp;"
            :
            :"r"(threadbp), "r"(threadsp)
           );
    }
    return 0;
}


ocrGuid_t procEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    register char * const basepointer __asm("rbp");
    register char * const stackpointer __asm("rsp");
    u64 rank = paramv[0];
    ocrElsUserSet(0, (ocrGuid_t) rank);
    if(depc > 1) { // it's a continuation
        Context *context = (Context*) depv[2].ptr;
        context->phase++;
        int phase = context->phase;
        printf("(%lu) ==========Phase %d starts===========\n", MYRANK, phase);
        context->originalbp = basepointer;
        context->originalsp = stackpointer;
        // Store callee saved registers
        __asm volatile(
            "movq -24(%%rbp), %0;" /* rbx */
            "movq -16(%%rbp), %1;" /* r12 */
            "movq  -8(%%rbp), %2;" /* r13 */
            "movq %%r14, %3;"
            "movq %%r15, %4;"
            "movq %%rbp, %5;"
            :"=r"(context->callee_saved[0]), "=r"(context->callee_saved[1]), "=r"(context->callee_saved[2]), "=r"(context->callee_saved[3]), "=r"(context->callee_saved[4]), "=r"(context->callee_saved[5])
            :
           );
        printf("(%lu/%d) saving rbx = 0x%012lx\n", MYRANK, phase, context->callee_saved[0]);
        printf("(%lu/%d) saving r12 = 0x%012lx\n", MYRANK, phase, context->callee_saved[1]);
        printf("(%lu/%d) saving r13 = 0x%012lx\n", MYRANK, phase, context->callee_saved[2]);
        printf("(%lu/%d) saving r14 = 0x%012lx\n", MYRANK, phase, context->callee_saved[3]);
        printf("(%lu/%d) saving r15 = 0x%012lx\n", MYRANK, phase, context->callee_saved[4]);
        printf("(%lu/%d) saving rbp = 0x%012lx\n", MYRANK, phase, context->callee_saved[5]);

	// Update context in case new DBs arrived
	if(context->DBs_recv > 0) {
	  int slot = context->DBs_cont[rank];
	  int blocks = context->DBs_recv;
	  printf("(%lu/%d) updating DBs context - depc %u\n", MYRANK, phase, depc); 
	  printf("(%lu/%d) values: DBs_cont %d, DBs_recv %d\n", MYRANK, phase, slot, blocks);
	  for(int i = 0; i < blocks; i++) {
	    context->DBs[slot+i] = depv[3+i];
	    printf("(%lu/%d) new DBs %lx stored in context\n", MYRANK, phase, context->DBs[slot+i].guid); 
	    int *data = (int *)context->DBs[slot+i].ptr;
	    printf("(%lu/%d) DB value: %d\n", MYRANK, phase, *data);
	  }
	  context->DBs_recv = 0;
	  context->DBs_cont[rank] = slot + blocks;
	}
	
        longjmp(context->env, 0);
        // will never reach here
        assert(0 && "Program execution should never reach here");
    } else {
        char * newsp, * oldsp; // FIXME:use volatile to prevent compiler optimization?
        char * newbp, * oldbp;
        // create a data block to store program context
        ocrGuid_t context_guid;
        Context *context;
        ocrDbCreate(&context_guid, (void**) &context, sizeof(Context), /*flags=*/DB_PROP_NONE, /*affinity=*/NULL_GUID, NO_ALLOC);
        printf("(%lu) context DB guid %lx\n", MYRANK, context_guid);
        context->phase = 0;
        int phase = context->phase;
        context->args = depv[0].guid;
        context->self_context_guid = context_guid;
        context->shutdown_edt_guid = paramv[1];
	context->type1Map = paramv[2];
	context->type2Map = paramv[3];	
        context->originalbp = basepointer;
        context->originalsp = stackpointer;
	context->DBs_recv = 0;
	for(int i = 0; i < NP; i++) {
	  context->DBs_cont[i] = 0;
	  for(int j = 0; j < NP; j++) context->comm_event[i*NP+j] = 0; // events to communicate
	}
	
        int argc = getArgc(depv[0].ptr);
        char **argv = (char **) malloc(argc * sizeof(char *));
        for(int i = 0; i < argc; i++) {
            char *arg = getArgv(depv[0].ptr, i);
            argv[i] = arg;
        }
        printf("(%lu/%d) procEdt is executed\n", MYRANK, phase);
        printf("(%lu/%d) procEdt frame address (0) %p\n", MYRANK, phase, __builtin_frame_address(0));
        printf("(%lu/%d) procEdt rbp %p\n", MYRANK, phase, basepointer);
        printf("(%lu/%d) procEdt rsp %p\n", MYRANK, phase, stackpointer);
        printf("(%lu/%d) procEdt stack frame size = 0x%x\n", MYRANK, phase, basepointer - stackpointer);
        printf("(%lu/%d) procEdt allocated DB stack %p - %p\n", MYRANK, phase, context->stack, context->stack + DB_STACK_SIZE - 1);

        // =========================
        // Setup stack frame
        // =========================
        oldbp = basepointer;
        oldsp = stackpointer;

        // Change stack pointer / base pointer value to use the heap memory as stack
        newbp = (context->stack + DB_STACK_SIZE);
        newsp = newbp - (oldbp - oldsp);

        // Store callee saved registers
        __asm volatile(
            "movq -24(%%rbp), %0;" /* rbx */
            "movq -16(%%rbp), %1;" /* r12 */
            "movq  -8(%%rbp), %2;" /* r13 */
            "movq %%r14, %3;"
            "movq %%r15, %4;"
            "movq %%rbp, %5;"
            :"=r"(context->callee_saved[0]), "=r"(context->callee_saved[1]), "=r"(context->callee_saved[2]), "=r"(context->callee_saved[3]), "=r"(context->callee_saved[4]), "=r"(context->callee_saved[5])
            :
           );
        printf("(%lu/%d) saving rbx = 0x%012lx\n", MYRANK, phase, context->callee_saved[0]);
        printf("(%lu/%d) saving r12 = 0x%012lx\n", MYRANK, phase, context->callee_saved[1]);
        printf("(%lu/%d) saving r13 = 0x%012lx\n", MYRANK, phase, context->callee_saved[2]);
        printf("(%lu/%d) saving r14 = 0x%012lx\n", MYRANK, phase, context->callee_saved[3]);
        printf("(%lu/%d) saving r15 = 0x%012lx\n", MYRANK, phase, context->callee_saved[4]);
        printf("(%lu/%d) saving rbp = 0x%012lx\n", MYRANK, phase, context->callee_saved[5]);
        // Copy stack frame. This has to happen after the stack variable values are computed
        memcpy(newsp - RED_ZONE_SIZE, stackpointer - RED_ZONE_SIZE, basepointer - stackpointer + RED_ZONE_SIZE);
        printf("(%lu) Stack frame dumped. Switching frame pointer and stack pointer\n", MYRANK);
        // After this line, stack variables should be read only to be safe
        __asm volatile("movq %0, %%rbp;"
            "movq %1, %%rsp;"
            :
            :"r"(newbp), "r"(newsp)
           );

        printf("(%lu/%d) Stack frame switched to data block space\n", MYRANK, phase);
        printf("(%lu/%d) (db) procEdt frame address (0) %p\n", MYRANK, phase, __builtin_frame_address(0));
        printf("(%lu/%d) (db) procEdt rbp %p\n", MYRANK, phase, basepointer);
        printf("(%lu/%d) (db) procEdt rsp %p\n", MYRANK, phase, stackpointer);
        printf("(%lu/%d) (db) procEdt stack frame size = 0x%x\n", MYRANK, phase, basepointer - stackpointer);

        if(hta_main(argc, argv, 0, context) == 0) {
            phase = context->phase;
            printf("(%lu/%d) last procEdt. shutting down runtime\n", MYRANK, phase);
            printf("(%lu/%d) Restore callee saved registers before returning to OCR runtime\n", MYRANK, phase);
            printf("(%lu/%d) Restoring rbx = 0x%012lx\n", MYRANK, phase, context->callee_saved[0]);
            printf("(%lu/%d) Restoring r12 = 0x%012lx\n", MYRANK, phase, context->callee_saved[1]);
            printf("(%lu/%d) Restoring r13 = 0x%012lx\n", MYRANK, phase, context->callee_saved[2]);
            printf("(%lu/%d) Restoring r14 = 0x%012lx\n", MYRANK, phase, context->callee_saved[3]);
            printf("(%lu/%d) Restoring r15 = 0x%012lx\n", MYRANK, phase, context->callee_saved[4]);
            printf("(%lu/%d) Restoring rbp = 0x%012lx\n", MYRANK, phase, context->callee_saved[5]);
            __asm volatile(
                "movq %0, -24(%%rbp);" /* rbx */
                "movq %1, -16(%%rbp);" /* r12 */
                "movq %2,  -8(%%rbp);" /* r13 */
                "movq %3, %%r14;"
                "movq %4, %%r15;"
                "movq %5, %%rbp;"
                :
                :"r"(context->callee_saved[0]), "r"(context->callee_saved[1]), "r"(context->callee_saved[2]), "r"(context->callee_saved[3]), "r"(context->callee_saved[4]), "r"(context->callee_saved[5])
               );
	    printf("(%lu) shutdown guid %lx\n", MYRANK, context->shutdown_edt_guid);
            ocrEventSatisfySlot(context->shutdown_edt_guid, NULL_GUID, MYRANK); // Satisfy shutdown slots
            return NULL_GUID;
        }
        else {
            phase = context->phase;
            printf("(%lu/%d) procEdt single phase finished, will be continued\n", MYRANK, phase);
        }

        printf("(%lu/%d) Restore callee saved registers before returning to OCR runtime\n", MYRANK, phase);
        printf("(%lu/%d) Restoring rbx = 0x%012lx\n", MYRANK, phase, context->callee_saved[0]);
        printf("(%lu/%d) Restoring r12 = 0x%012lx\n", MYRANK, phase, context->callee_saved[1]);
        printf("(%lu/%d) Restoring r13 = 0x%012lx\n", MYRANK, phase, context->callee_saved[2]);
        printf("(%lu/%d) Restoring r14 = 0x%012lx\n", MYRANK, phase, context->callee_saved[3]);
        printf("(%lu/%d) Restoring r15 = 0x%012lx\n", MYRANK, phase, context->callee_saved[4]);
        printf("(%lu/%d) Restoring rbp = 0x%012lx\n", MYRANK, phase, context->callee_saved[5]);
        __asm volatile(
            "movq %0, -24(%%rbp);" /* rbx */
            "movq %1, -16(%%rbp);" /* r12 */
            "movq %2,  -8(%%rbp);" /* r13 */
            "movq %3, %%r14;"
            "movq %4, %%r15;"
            "movq %5, %%rbp;"
            :
            :"r"(context->callee_saved[0]), "r"(context->callee_saved[1]), "r"(context->callee_saved[2]), "r"(context->callee_saved[3]), "r"(context->callee_saved[4]), "r"(context->callee_saved[5])
           );
        ocrAddDependence(NULL_GUID, context->next_phase_edt_guid, 0, DB_DEFAULT_MODE); // the continuation is activated here   
    }
    return NULL_GUID;
}

ocrGuid_t shutdownEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
     printf("Shutting down! Bye!\n");
     ocrShutdown();
     
     return NULL_GUID;
}

// Mapping function for type 1 & 3: (pid, phase)
ocrGuid_t procPhaseMapFunc(ocrGuid_t startGuid, u64 stride, s64* params, s64* tuple) {
    const s64 dim0 = params[0], dim1 = params[1];
    const s64 idx0 = tuple[0], idx1 = tuple[1];

    // boundary check
    for(int i = 0; i < 2; i++) {
        if(!(tuple[i] >= 0 && tuple[i] < params[i]))
            return NULL_GUID;
    }

    s64 index = idx0 * dim1 + idx1;

    return (ocrGuid_t)(index*stride + startGuid);
}

// Mapping function for type 2: (pid, phase, slot_index)
ocrGuid_t procSlotMapFunc(ocrGuid_t startGuid, u64 stride, s64* params, s64* tuple) {
    const s64 dim0 = params[0], dim1 = params[1], dim2 = params[2];
    const s64 idx0 = tuple[0], idx1 = tuple[1], idx2 = tuple[2];

    // boundary check
    for(int i = 0; i < 3; i++) {
        if(!(tuple[i] >= 0 && tuple[i] < params[i]))
            return NULL_GUID;
    }

    s64 index = idx0 * dim1 * dim2 + idx1 * dim2 + idx2;

    return (ocrGuid_t)(index*stride + startGuid);
}

// Program entry (No hacks here all normal calling flow)
ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
   
    // Create GuidMap for type 1 --- For communication events 
    s64 dimension_type1[2] = {NP, MAX_NUM_PHASE};
    ocrGuid_t type1Map = NULL_GUID;
    ocrGuidMapCreate(&type1Map, 2, procPhaseMapFunc, dimension_type1, NP * MAX_NUM_PHASE, GUID_USER_EDT);
    
    // Create GuidMap for type 2
    s64 dimension_type2[3] = {NP, NP, MAX_NUM_PHASE};
    ocrGuid_t type2Map = NULL_GUID;
    ocrGuidMapCreate(&type2Map, 3, procSlotMapFunc, dimension_type2, NP * NP * MAX_NUM_PHASE, 
		     GUID_USER_EVENT_STICKY);

    // Create shutdown EDT
    ocrGuid_t shutdownEdt_template_guid;
    ocrEdtTemplateCreate(&shutdownEdt_template_guid, shutdownEdt, 0, NP);
    ocrGuid_t shutdownEdt_guid;
    ocrEdtCreate(&shutdownEdt_guid, shutdownEdt_template_guid, /*paramc=*/0, /*paramv=*/NULL, /*depc=*/NP, 
		 /*depv=*/NULL, /*properties=*/0 , /*affinity*/NULL_GUID,  /*outputEvent*/NULL );
    printf("shutdownEDT guid %lx\n", shutdownEdt_guid);
   
    // Create communication events
    ocrGuid_t EventSendOut_guid = NULL_GUID;
    s64 tuple[] = {0, 0, 0}; 
    for(int i = 0; i < NP; i++) {
      tuple[0] = i;
      for(int j = 0; j < NP; j++) {
	tuple[1] = j;
	ocrGuidFromLabel(&EventSendOut_guid, type2Map, tuple);
	ocrEventCreate(&EventSendOut_guid, OCR_EVENT_STICKY_T, GUID_PROP_IS_LABELED | EVT_PROP_NONE);
	printf("Communication event (%d --> %d) guid %lx\n", i, j, EventSendOut_guid); 
	//printf("Map2 guid %lx\n", type2Map);
      }
    }
       
    // Create proc EDT
    ocrGuid_t procEdt_template_guid;
    ocrEdtTemplateCreate(&procEdt_template_guid, procEdt, 4, 1);
    
    for(int i = 0; i < NP; i++) {
        ocrGuid_t procEdt_guid;
        u64 rank[4];
	rank[0] = i; 
	rank[1] = shutdownEdt_guid; 	// passed as argument
	rank[2] = type1Map;		// passed as argument
	rank[3] = type2Map;		// passed as argument
        ocrEdtCreate(&procEdt_guid, procEdt_template_guid, /*paramc=*/4, /*paramv=*/rank, /*depc=*/1, 
		     /*depv=*/NULL, /*properties=*/0 , /*affinity*/NULL_GUID,  /*outputEvent*/NULL );
        printf("procEDT template guid %lx\n", procEdt_template_guid);
        printf("procEDT guid %lx\n", procEdt_guid);

        // Add proc EDT dependences
        ocrAddDependence(depv[0].guid, procEdt_guid, 0, DB_MODE_RW); // argc and argv // Immediately satisfy
    }
    
    // Satisfy proc EDT dependences for it to start
    // Nothing for now

	
    ocrEdtTemplateDestroy(procEdt_template_guid);
    printf("mainEdt returned\n");
    // mainEdt returns OCR runtime is not shutdown here
    return NULL_GUID;
}

