#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <string.h>
#include <setjmp.h>

#include "ocr.h"
#define ENABLE_EXTENSION_RTITF
#include "extensions/ocr-runtime-itf.h"

#define NUM_SLAVES (4)
#define DB_STACK_SIZE (8*1024*1024) // 8 MB for now
#define RED_ZONE_SIZE (0)
#define NUM_CALLEE_SAVED_REGS (6)

#define HTA_OP_FINISHED (0)
#define HTA_OP_TO_BE_CONTINUED (1)


#define HTA_PARALLEL_OP(func_call) \
    if((func_call) != HTA_OP_FINISHED) { \
        printf("hta_main split\n"); \
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
    ocrGuid_t   shutdown_edt_guid;                          // guid of the shutdown Edt
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
        ocrEdtCreate(&slaveEdts[i], slaveEdt_template_guid, /*paramc=*/0, /*paramv=*/(u64 *)NULL, /*depc=*/2, /*depv=*/NULL, /*properties=*/0 , /*affinity*/NULL_GUID,  &slaveOutEvent[i]);
        ocrAddDependence(slaveInDBs[i], slaveEdts[i], 1, DB_DEFAULT_MODE); // Immediately satisfy
        printf("(%lu) slave %d EDT guid %lx\n", MYRANK, i, slaveEdts[i]);
        printf("(%lu) slave %d out event guid %lx\n", MYRANK, i, slaveOutEvent[i]);
    }
    ocrEdtTemplateDestroy(slaveEdt_template_guid);

    //======================================================================
    // Create continuation to wait for slave EDTs to finish
    //======================================================================
    ocrGuid_t procEdt_template_guid;
    ocrGuid_t procEdt_guid;
    ocrEdtTemplateCreate(&procEdt_template_guid, procEdt, 1, 3+NUM_SLAVES);

    u64 rank = MYRANK;
    ocrGuid_t depv[3+NUM_SLAVES];
    depv[0] = UNINITIALIZED_GUID;
    depv[1] = context->args;
    depv[2] = context->self_context_guid;
    for(int i = 0; i < NUM_SLAVES; i++)
        depv[3+i] = slaveOutEvent[i];
    ocrEdtCreate(&procEdt_guid, procEdt_template_guid, /*paramc=*/1, /*paramv=*/(u64 *)&rank, /*depc=*/EDT_PARAM_DEF, /*depv=*/depv, /*properties=*/0 , /*affinity*/NULL_GUID,  /*outputEvent*/NULL );
    printf("(%lu) continuation procEDT template guid %lx\n", MYRANK, procEdt_template_guid);
    printf("(%lu) continuation procEDT guid %lx\n", MYRANK, procEdt_guid);

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
    
    // call a second parallel operation
    HTA_PARALLEL_OP(hta_map(pid, context)); // slave codelets are created in here

    printf("(%lu) some stack variable = %d, other stack variable = %d\n", MYRANK, some_stack_variable, other_stack_variable);
    // some computation
    some_stack_variable = -333;
    other_stack_variable = 808;
    
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
        context->originalbp = basepointer;
        context->originalsp = stackpointer;

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

// Program entry (No hacks here all normal calling flow)
ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    int NP = 4;
   
    // Create shutdown EDT
    ocrGuid_t shutdownEdt_template_guid;
    ocrEdtTemplateCreate(&shutdownEdt_template_guid, shutdownEdt, 0, NP);
    ocrGuid_t shutdownEdt_guid;
    ocrEdtCreate(&shutdownEdt_guid, shutdownEdt_template_guid, /*paramc=*/0, /*paramv=*/NULL, /*depc=*/NP, 
		 /*depv=*/NULL, /*properties=*/0 , /*affinity*/NULL_GUID,  /*outputEvent*/NULL );
    printf("shutdownEDT guid %lx\n", shutdownEdt_guid);
   
    // Create proc EDT
    ocrGuid_t procEdt_template_guid;
    ocrEdtTemplateCreate(&procEdt_template_guid, procEdt, 2, 1);
    
    int i = 0;
    for(i = 0; i < NP; i++) {
        ocrGuid_t procEdt_guid;
        u64 rank[2];
	rank[0] = i; 
	rank[1] = shutdownEdt_guid; // passed as argument
        ocrEdtCreate(&procEdt_guid, procEdt_template_guid, /*paramc=*/2, /*paramv=*/rank, /*depc=*/1, 
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

