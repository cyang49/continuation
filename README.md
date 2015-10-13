# continuation
Experimental continuation implementation

This project is created to experiment on continuation implementation.
The first approach we are trying right now is to store stack frames
in some heap memory space and restore when continuation happens.

test.c: Experiment setting base pointer rbp and satck pointer rsp
        to point to heap storage and restore it later

test_ocr.c: The prototype of one process split-phase execution


To enable guid labeling in OCR, modify the OCR configuration
file, and replace PTR with LABELED.

