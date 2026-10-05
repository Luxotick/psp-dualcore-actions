TARGET = psp-dualcore-actions
OBJS = main.o me_loop.o me_stub.o

CFLAGS = -O2 -G0 -Wall -Wextra -std=c11
ASFLAGS = $(CFLAGS)
BUILD_PRX = 1
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSP Dual-Core Actions

include $(PSPSDK)/lib/build.mak
