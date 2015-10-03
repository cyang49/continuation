#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEAP_STACK_SIZE (1024*1024)  // allocate 1KB block as stack


int func3(int x, int y) {
    register char * const basepointer __asm("rbp");
    register char * const stackpointer __asm("rsp");
    printf("In func3\n");
    //printf("func3 frame address (0) %p\n", __builtin_frame_address(0));
    //printf("func3 frame address (1) %p\n", __builtin_frame_address(1));
    printf("func3 rbp %p\n", basepointer);
    printf("func3 rsp %p\n", stackpointer);
    printf("func3 stack frame size = 0x%x\n", basepointer - stackpointer);
    return x + y;
}

int func2(int x, int y) {
    register char * const basepointer __asm("rbp");
    register char * const stackpointer __asm("rsp");
    printf("In func2\n");
    //printf("func2 frame address (0) %p\n", __builtin_frame_address(0));
    //printf("func2 frame address (1) %p\n", __builtin_frame_address(1));
    printf("func2 rbp %p\n", basepointer);
    printf("func2 rsp %p\n", stackpointer);
    printf("func2 stack frame size = 0x%x\n", basepointer - stackpointer);
    return func3(x, y);
}

int func1(int x, int y) {
    int a, b;
    register char * const basepointer __asm("rbp");
    register char * const stackpointer __asm("rsp");
    printf("In func1\n");
    //printf("func1 frame address (0) %p\n", __builtin_frame_address(0));
    //printf("func1 frame address (1) %p\n", __builtin_frame_address(1));
    //printf("func1 rbp %p\n", basepointer);
    //printf("func1 rsp %p\n", stackpointer);
    //printf("func1 stack frame size = 0x%x\n", basepointer - stackpointer);
    printf("&a = %p, &b = %p\n", &a, &b);
    return func2(x, y);
}

int main(int argc, char** argv) {
    register char * const basepointer __asm("rbp");
    register char * const stackpointer __asm("rsp");
    int ret1;
    int x = 3;
    int y = 5;
    char * newsp, * oldsp; // FIXME:use volatile to prevent compiler optimization?
    char * newbp, * oldbp;
    
    printf("main frame address (0) %p\n", __builtin_frame_address(0));
    printf("main frame address (1) %p\n", __builtin_frame_address(1));
    printf("main rbp %p\n", basepointer);
    printf("main rsp %p\n", stackpointer);
    printf("main stack frame size = 0x%x\n", basepointer - stackpointer);

    // Allocate heap memory as stack
    char * mystack = malloc(HEAP_STACK_SIZE);
    printf("Allocated heap space starts at %p\n", mystack);

    // Copy current stack frame
    // Store registers
    oldbp = basepointer;
    oldsp = stackpointer;

    // Change stack pointer / base pointer value to use the heap memory as stack
    newbp = (mystack + HEAP_STACK_SIZE);
    newsp = newbp - (oldbp - oldsp);

    // Copy stack frame. This has to happen after the stack variable values are computed
    memcpy(newsp, stackpointer, basepointer - stackpointer);
    printf("Stack frame dumped. Switching frame pointer and stack pointer\n");
    // After this line, stack variables should be read only to be safe
    __asm volatile("movq %0, %%rbp;"
        "movq %1, %%rsp;"
        :
        :"r"(newbp), "r"(newsp)
       );

    printf("Stack frame switched to heap memory\n");
    //printf("(heap) main frame address (0) %p\n", __builtin_frame_address(0));
    //printf("(heap) main frame address (1) %p\n", __builtin_frame_address(1));
    //printf("(heap) main rbp %p\n", basepointer);
    //printf("(heap) main rsp %p\n", stackpointer);
    //printf("(heap) main stack frame size = 0x%x\n", basepointer - stackpointer);
    
    // OK to use stack variables because the stack frame is copied
    // Call the function
    printf("Calling func1\n");
    ret1 = func1(x, y);

    // Restore stack pointer / base pointer value
    //basepointer = oldbp;
    //stackpointer = oldsp;
    printf("Returned from func1, restoring stack frame\n");
    __asm volatile(
        "movq %0, %%rbp;"
        "movq %1, %%rsp;"
        :
        :"r"(oldbp), "r"(oldsp)
       );

    // The return value from function one is not recovered
    // A memory copy is required
    memcpy(oldsp, newsp, basepointer - stackpointer);
    printf("ret1 = %d\n", ret1);
    free(mystack);

    return 0;
}
