CC=gcc -std=c99
OPT=-O0 -g
# Notice that -fno-omit-frame-pointer is needed so that when compiler optimization
# is on, %rbp will be used and so it is possible to calculate stack frame size.
GCC_OPT=-fno-asynchronous-unwind-tables -fno-omit-frame-pointer
#GCC_OPT=-fno-omit-frame-pointer
CFLAGS=$(GCC_OPT) $(OPT) -Wall -Wextra

OCR_FLAGS=-L${OCR_INSTALL}/lib -I${OCR_INSTALL}/include 
OCR_LDFLAGS=-locr -lpthread

SRC=test.c test_ocr.c

ASM=$(SRC:.c=.s)

TARGET=$(SRC:.c=)

all: $(TARGET)

%.s: %.c
	$(CC) -S $(OCR_FLAGS) $(OCR_LDFLAGS) $(CFLAGS) $<  

%: %.c
	$(CC) $(CFLAGS) $(OCR_FLAGS) $(OCR_LDFLAGS) $< -o $@

clean:
	rm -f *.o *.s $(TARGET)
