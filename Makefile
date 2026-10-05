TARGET = psp-dualcore-actions
OBJS = main.o me_loop.o

CFLAGS = -Os -G0 -Wall -Wextra -fno-pic -std=c11
BUILD_PRX = 1
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSP Dual-Core Actions
LIBS = -lme-stask -lme-core-mapper -lpspkubridge -lpspdebug -lpsppower -lpspkernel -lc

include $(PSPSDK)/lib/build.mak
