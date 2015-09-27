#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <string.h>

#include "ocr.h"

ocrGuid_t procEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    printf("procEdt is executed\n");


    ocrShutdown();
    return NULL_GUID;
}

// Program entry
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

