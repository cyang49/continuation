#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <string.h>
#include <setjmp.h>

#include "ocr.h"

#define NUM_SLAVES (4)
#define DB_STACK_SIZE (256*1024*1024) // 256 MB for now
#define RED_ZONE_SIZE (128)

typedef struct {
    char*       originalbp;
    char*       originalsp;
    char*       continuebp;
    char*       continuesp;
    ocrGuid_t   args;
    ocrGuid_t   self_context_guid;
    jmp_buf     env;
    char        stack[DB_STACK_SIZE];
    long int    callee_saved[5];
} Context;

ocrGuid_t procEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t slaveEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]);

ocrGuid_t slaveEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    printf("map is executed\n");
    return NULL_GUID; // satisfies control dependency of the continuation codelet
}

void _fix_pointers(void* db_bp, void *thread_bp, void* max_thread_bp) {
    while(thread_bp < max_thread_bp) {
        size_t frame_size = *((void**)db_bp) - db_bp;
        *((void**)thread_bp) = thread_bp + frame_size;
        thread_bp += frame_size;
        db_bp += frame_size;
    }
}

int hta_map(int pid, Context* context)
{
    register char * const basepointer __asm("rbp");
    register char * const stackpointer __asm("rsp");
    context->continuesp = stackpointer;
    context->continuebp = basepointer;
    // create slave EDTs
    ocrGuid_t slaveEdt_template_guid;
    ocrGuid_t slaveEdts[NUM_SLAVES];
    ocrGuid_t slaveOutEvent[NUM_SLAVES];
    ocrGuid_t slaveInDBs[NUM_SLAVES];

    ocrEdtTemplateCreate(&slaveEdt_template_guid, slaveEdt, 0, 2);
    for(int i = 0; i < NUM_SLAVES; i++) {
        int *data;
        slaveOutEvent[i] = NULL_GUID;
        ocrDbCreate(&slaveInDBs[i], (void**) &data, sizeof(int), /*flags=*/DB_PROP_NONE, /*affinity=*/NULL_GUID, NO_ALLOC);
        ocrEdtCreate(&slaveEdts[i], slaveEdt_template_guid, /*paramc=*/0, /*paramv=*/(u64 *)NULL, /*depc=*/2, /*depv=*/NULL, /*properties=*/0 , /*affinity*/NULL_GUID,  &slaveOutEvent[i]);
        ocrAddDependence(slaveInDBs[i], slaveEdts[i], 1, DB_DEFAULT_MODE); // Immediately satisfy
        printf("slave %d EDT guid %lx\n", i, slaveEdts[i]);
        printf("slave %d out event guid %lx\n", i, slaveOutEvent[i]);
    }
    ocrEdtTemplateDestroy(slaveEdt_template_guid);

    // create continuation codelet procEdt
    ocrGuid_t procEdt_template_guid;
    ocrGuid_t procEdt_guid;
    ocrEdtTemplateCreate(&procEdt_template_guid, procEdt, 0, 3+NUM_SLAVES);

    ocrGuid_t depv[3+NUM_SLAVES];
    depv[0] = UNINITIALIZED_GUID;
    depv[1] = context->args;
    depv[2] = context->self_context_guid;
    for(int i = 0; i < NUM_SLAVES; i++)
        depv[3+i] = slaveOutEvent[i];
    ocrEdtCreate(&procEdt_guid, procEdt_template_guid, /*paramc=*/0, /*paramv=*/(u64 *)NULL, /*depc=*/EDT_PARAM_DEF, /*depv=*/depv, /*properties=*/0 , /*affinity*/NULL_GUID,  /*outputEvent*/NULL );

    // defer firing slave EDTs
    for(int i = 0; i < NUM_SLAVES; i++) {
        ocrAddDependence(NULL_GUID, slaveEdts[i], 0, DB_DEFAULT_MODE); //pure control dependence // Immediately satisfy
    }
    ocrEdtTemplateDestroy(procEdt_template_guid);
    
    if(!setjmp(context->env)) 
    {
        // switch back to thread stack
        // 1. compute the size that need to be copied (the growth of DB stack)
        size_t size_to_copy = (context->stack + DB_STACK_SIZE) - stackpointer;
        printf("stack size growth = 0x%x\n", size_to_copy);
        // 2. compute the start address of the thread stack
        char* originalbp = context->originalbp;
        char* threadsp = originalbp - size_to_copy;
        char* threadbp = threadsp + (basepointer-stackpointer);
        // 3. copy DB stack to overwrite thread stack
        printf("Enabling continuation codelet\n");
        printf("switching back to thread stack at (%p - %p) original bp (%p)\n", threadsp, threadbp, originalbp);
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
        ocrAddDependence(NULL_GUID, procEdt_guid, 0, DB_DEFAULT_MODE);

        return 1;
    }
    else
    {
        // Continuation should start from here
        printf("hta_map is continued\n");
        
        return 0;
    }
}


int hta_main(int argc, char** argv, int pid, Context* context)
{
    int some_stack_variable = -111;
    int other_stack_variable = 202;
    
    // call a parallel operation 
    if(hta_map(pid, context)) // slave codelets are created in here
        return 1;

    // some computation
    printf("some stack variable = %d, other stack variable = %d\n", some_stack_variable, other_stack_variable);
    
    // call a second parallel operation

    // the second continue
 
    printf("return from hta_main()\n");

    // must restore thread stack before going back to normal execution
    {
        register char * const basepointer __asm("rbp");
        register char * const stackpointer __asm("rsp");
        // switch back to thread stack
        // 1. compute the size that need to be copied (the growth of DB stack)
        size_t size_to_copy = (context->stack + DB_STACK_SIZE) - stackpointer;
        printf("stack size growth = 0x%x\n", size_to_copy);
        // 2. compute the start address of the thread stack
        char* originalbp = context->originalbp;
        char* threadsp = originalbp - size_to_copy;
        char* threadbp = threadsp + (basepointer-stackpointer);
        // 3. copy DB stack to overwrite thread stack
        printf("switching back to thread stack at (%p - %p) original bp (%p)\n", threadsp, threadbp, originalbp);
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
    if(depc > 1) { // it's a continuation
        //char *newsp, *newbp;
        Context *context = (Context*) depv[2].ptr;
        context->originalbp = basepointer;
        context->originalsp = stackpointer;
        // Store callee saved registers
        __asm volatile(
            "movq %%rbx, %0;"
            "movq %%r12, %1;"
            "movq %%r13, %2;"
            "movq %%r14, %3;"
            "movq %%r15, %4;"
            :"=r"(context->callee_saved[0]), "=r"(context->callee_saved[1]), "=r"(context->callee_saved[2]), "=r"(context->callee_saved[3]), "=r"(context->callee_saved[4])
            :
           );

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
        context->args = depv[0].guid;
        context->self_context_guid = context_guid;
        context->originalbp = basepointer;
        context->originalsp = stackpointer;

        int argc = getArgc(depv[0].ptr);
        char **argv = (char **) malloc(argc * sizeof(char *));
        for(int i = 0; i < argc; i++) {
            char *arg = getArgv(depv[0].ptr, i);
            argv[i] = arg;
        }
        printf("procEdt is executed\n");
        printf("procEdt frame address (0) %p\n", __builtin_frame_address(0));
        printf("procEdt rbp %p\n", basepointer);
        printf("procEdt rsp %p\n", stackpointer);
        printf("procEdt stack frame size = 0x%x\n", basepointer - stackpointer);
        printf("procEdt allocated DB stack %p - %p\n", context->stack, context->stack + DB_STACK_SIZE - 1);

        // =========================
        // Setup stack frame
        // =========================
        oldbp = basepointer;
        oldsp = stackpointer;

        // Change stack pointer / base pointer value to use the heap memory as stack
        newbp = (context->stack + DB_STACK_SIZE);
        newsp = newbp - (oldbp - oldsp);

        // Copy stack frame. This has to happen after the stack variable values are computed
        memcpy(newsp - RED_ZONE_SIZE, stackpointer - RED_ZONE_SIZE, basepointer - stackpointer + RED_ZONE_SIZE);
        printf("Stack frame dumped. Switching frame pointer and stack pointer\n");
        // After this line, stack variables should be read only to be safe
        __asm volatile("movq %0, %%rbp;"
            "movq %1, %%rsp;"
            :
            :"r"(newbp), "r"(newsp)
           );

        printf("Stack frame switched to data block space\n");
        printf("(db) procEdt frame address (0) %p\n", __builtin_frame_address(0));
        printf("(db) procEdt rbp %p\n", basepointer);
        printf("(db) procEdt rsp %p\n", stackpointer);
        printf("(db) procEdt stack frame size = 0x%x\n", basepointer - stackpointer);

        if(hta_main(argc, argv, 0, context) == 0) {
            printf("last procEdt. shutting down runtime\n");
            // Restore callee-saved registers FIXME: placement is not right
            __asm volatile(
                "movq %0, %%rbx;"
                "movq %1, %%r12;"
                "movq %2, %%r13;"
                "movq %3, %%r14;"
                "movq %4, %%r15;"
                :
                :"r"(context->callee_saved[0]), "r"(context->callee_saved[1]), "r"(context->callee_saved[2]), "r"(context->callee_saved[3]), "r"(context->callee_saved[4])
               );
            ocrShutdown();
        }
        else
            printf("procEdt single phase finished, will be continued\n");

    }
    return NULL_GUID;
}

// Program entry (No hacks here all normal calling flow)
ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    // Create proc EDT
    ocrGuid_t procEdt_template_guid;
    ocrGuid_t procEdt_guid;
    ocrEdtTemplateCreate(&procEdt_template_guid, procEdt, 0, 1);
    ocrEdtCreate(&procEdt_guid, procEdt_template_guid, /*paramc=*/0, /*paramv=*/(u64 *)NULL, /*depc=*/1, /*depv=*/NULL, /*properties=*/0 , /*affinity*/NULL_GUID,  /*outputEvent*/NULL );

    // Add proc EDT dependences
    ocrAddDependence(depv[0].guid, procEdt_guid, 0, DB_MODE_RW); // argc and argv // Immediately satisfy
    
    // Satisfy proc EDT dependences for it to start
    // Nothing for now

    ocrEdtTemplateDestroy(procEdt_template_guid);
    printf("mainEdt returned\n");
    // mainEdt returns OCR runtime is not shutdown here
    return NULL_GUID;
}

