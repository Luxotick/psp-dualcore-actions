TARGET = psp-dualcore-actions
OBJS = main.o me_loop.o

CFLAGS = -Os -G0 -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wstrict-prototypes -fno-pic -fno-lto -std=gnu11
CFLAGS += -isystem $(PSPDEV)/psp/include -isystem $(PSPSDK)/include
LDFLAGS = -Wl,-Map=psp-dualcore-actions.map
BUILD_PRX = 1
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSP Media Engine Two Operands
LIBS = -lme-stask -lme-core-mapper -lpspdebug -lpspctrl -lpspdisplay -lpspge -lpspaudio -lpspmp3 -lpspnet_resolver -lpspnet_inet -lpspnet_apctl -lpspnet -lpsppower -lpspaudiocodec -lpsputility -lpspwlan -lpspsdk -lc
SFOFLAGS = -s APP_VER=02.00
PSPSDK ?= $(shell psp-config --pspsdk-path)

include $(PSPSDK)/lib/build.mak
