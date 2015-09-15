CC=gcc
OPT=-O0 
#OPT=-O0 -g
# Notice that -fno-omit-frame-pointer is needed so that when compiler optimization
# is on, %rbp will be used and so it is possible to calculate stack frame size.
GCC_OPT=-fno-asynchronous-unwind-tables -fno-omit-frame-pointer
#GCC_OPT=-fno-omit-frame-pointer
CFLAGS=$(GCC_OPT) $(OPT) -Wall -pedantic -Wextra

SRC=test.c

ASM=$(SRC:.c=.s)

TARGET=$(SRC:.c=)

all: $(TARGET)

%.s: %.c
	$(CC) -S $(CFLAGS) $<  

%: %.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f *.o *.s $(TARGET)
