TARGET = psp-dualcore-actions
OBJS = main.o me_loop.o

CFLAGS = -Os -G0 -Wall -Wextra -fno-pic -std=gnu11
BUILD_PRX = 1
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSP Dual-Core Actions
LIBS = -lme-core-mapper -lme-stask -lpspdebug -lpspctrl -lpspdisplay -lpspge -lpsppower -lpspkernel -lpspaudiocodec -lpsputility -lpspkubridge -lc

include $(PSPSDK)/lib/build.mak
