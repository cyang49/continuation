/*
 * This example is created to test the guid labeling mechanism
 * that will be used in HTA library implementation.
 *
 * Three types of Mapping are required
 * 1. Map of procEdt phases to EDT guids: (pid, phase)
 * 2. Map of dependence slots of a procEdt phase to event guids: (pid, phase, slot_index)
 * 3. Map of output event guids of a procEdt phase: (pid, phase)
 *
 * Not sure: Slave EDTs? Not required if there are no dependences among slaves
 *           that belong to different procEdt. *
 */
#include <stdio.h>
#define ENABLE_EXTENSION_LABELING
#include "ocr.h"
#include "extensions/ocr-labeling.h"
#include "ocr-std.h"

#include "stdlib.h"

#define MAX_NUM_PHASE (256) 
#define MAX_NUM_SLOTS (64)

// Mapping function for type 1 & 2: (pid, phase)
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

ocrGuid_t dummyEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    PRINTF("Starting mainEdt\n");
    u32 input;
    u32 argc = getArgc(depv[0].ptr);
    if((argc != 2)) {
        PRINTF("Usage: test_guid_labeling <num>, defaulting to 4\n");
        input = 4;
    } else {
        input = atoi(getArgv(depv[0].ptr, 1));
    }


    // Create GuidMap for type 1
    s64 dimension_type1[2] = {input, MAX_NUM_PHASE};
    ocrGuid_t type1Map = NULL_GUID;
    ocrGuidMapCreate(&type1Map, 2, procPhaseMapFunc, dimension_type1, input * MAX_NUM_PHASE, GUID_USER_EDT);

    // Create GuidMap for type 2
    s64 dimension_type2[3] = {input, MAX_NUM_PHASE, MAX_NUM_SLOTS};
    ocrGuid_t type2Map = NULL_GUID;
    ocrGuidMapCreate(&type2Map, 3, procSlotMapFunc, dimension_type2, input * MAX_NUM_PHASE * MAX_NUM_SLOTS, GUID_USER_EVENT_ONCE);

    // Create GuidMap for type 3
    s64 dimension_type3[2] = {input, MAX_NUM_PHASE};
    ocrGuid_t type3Map = NULL_GUID;
    ocrGuidMapCreate(&type3Map, 2, procPhaseMapFunc, dimension_type1, input * MAX_NUM_PHASE, GUID_USER_EDT);
   
    // Test label to guid mapping
    // Type 1
    //ocrGuid_t proc00Guid = NULL_GUID;
    //ocrGuid_t proc14Guid = NULL_GUID;
    //ocrGuid_t proc82Guid = NULL_GUID; // exceeds default number of procs

    //s64 tuple00[] = {0, 0};
    //s64 tuple14[] = {1, 4};
    //s64 tuple82[] = {8, 2};
    //ocrGuidFromLabel(&proc00Guid, type1Map, tuple00);
    //ocrGuidFromLabel(&proc14Guid, type1Map, tuple14);
    //ocrGuidFromLabel(&proc82Guid, type1Map, tuple82);

    //printf("type 1 (0, 0) guid = %lx\n", (u64)proc00Guid);
    //printf("type 1 (1, 4) guid = %lx\n", (u64)proc14Guid);
    //printf("type 1 (8, 2) guid = %lx\n", (u64)proc82Guid);

    // Type 2
    ocrGuid_t proc003Guid = NULL_GUID;
    ocrGuid_t proc142Guid = NULL_GUID;
    ocrGuid_t proc821Guid = NULL_GUID; // exceeds default number of procs

    s64 tuple003[] = {0, 0, 3};
    s64 tuple142[] = {1, 4, 2};
    s64 tuple821[] = {8, 2, 1};
    ocrGuidFromLabel(&proc003Guid, type2Map, tuple003);
    ocrGuidFromLabel(&proc142Guid, type2Map, tuple142);
    ocrGuidFromLabel(&proc821Guid, type2Map, tuple821); // If default num procs is used, this call returns a zero value guid

    printf("type 2 (0, 0, 3) guid = %lx\n", (u64)proc003Guid);
    printf("type 2 (1, 4, 2) guid = %lx\n", (u64)proc142Guid);
    printf("type 2 (8, 2, 1) guid = %lx\n", (u64)proc821Guid);

    // Type 3
    //ocrGuid_t proc00OutGuid = NULL_GUID;
    //ocrGuid_t proc14OutGuid = NULL_GUID;
    //ocrGuid_t proc82OutGuid = NULL_GUID; // exceeds default number of procs

    //ocrGuidFromLabel(&proc00OutGuid, type3Map, tuple00);
    //ocrGuidFromLabel(&proc14OutGuid, type3Map, tuple14);
    //ocrGuidFromLabel(&proc82OutGuid, type3Map, tuple82);

    //printf("type 3 (0, 0) guid = %lx\n", (u64)proc00OutGuid);
    //printf("type 3 (1, 4) guid = %lx\n", (u64)proc14OutGuid);
    //printf("type 3 (8, 2) guid = %lx\n", (u64)proc82OutGuid);

    // !! labeling EDT and EDT output not implemented !!
    // Test creating OCR objects with guids
    //ocrGuid_t dummyTemplateGuid;
    //ocrEdtTemplateCreate(&dummyTemplateGuid, dummyEdt, 0, 0);
    //ocrEdtCreate(&proc00Guid, dummyTemplateGuid, 0, NULL, 0, NULL, EDT_PROP_NONE | GUID_PROP_IS_LABELED, NULL_GUID, &proc00OutGuid);
    //printf("created (0, 0) edt guid = %lx\n", (u64)proc00Guid);
    //printf("created (0, 0) out event guid = %lx\n", (u64)proc00OutGuid);
    //ocrEdtTemplateDestroy(dummyTemplateGuid);
    // !! labeling EDT and EDT output not implemented !!

    ocrEventCreate(&proc003Guid, OCR_EVENT_ONCE_T, GUID_PROP_IS_LABELED | EVT_PROP_NONE);
    printf("created (0, 0, 3) event guid = %lx\n", (u64)proc003Guid);
    ocrEventCreate(&proc142Guid, OCR_EVENT_ONCE_T, GUID_PROP_IS_LABELED | EVT_PROP_NONE);
    printf("created (1, 4, 2) event guid = %lx\n", (u64)proc142Guid);
    ocrEventCreate(&proc821Guid, OCR_EVENT_ONCE_T, GUID_PROP_IS_LABELED | EVT_PROP_NONE); // Exception if proc821Guid == 0
    printf("created (8, 2, 1) event guid = %lx\n", (u64)proc821Guid);

    // These events should be used when creating procEdt and associate with
    // the slots of the procEdt.
    
    // A sender should use ocrGuidFromLabel to acquire the guids of the events
    // and then it calls ocrEventSatisfy and supply a data block guid to send
    // the data block created dynamically
    ocrShutdown();
    return NULL_GUID;
}
