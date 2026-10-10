TARGET = psp-dualcore-actions
OBJS = main.o me_loop.o spotify/shannon.o spotify/sha1.o spotify/dh.o spotify/handshake.o spotify/config.o spotify/login.o spotify/audiokey.o spotify/stream.o spotify/http.o spotify/log.o spotify/tls.o spotify/webapi.o spotify/audiodecrypt.o spotify/mercury.o spotify/player.o

CFLAGS = -Os -G0 -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wstrict-prototypes -fno-pic -fno-lto -std=gnu11 -I.
CFLAGS += -isystem $(PSPDEV)/psp/include -isystem $(PSPSDK)/include
LDFLAGS = -Wl,-Map=psp-dualcore-actions.map
BUILD_PRX = 1
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSP Media Engine Two Operands
LIBS = -lme-stask -lme-core-mapper -lpspdebug -lpspctrl -lpspdisplay -lpspge -lpspaudio -lpspmp3 -lpsprtc -lpspnet_resolver -lpspnet_inet -lpspnet_apctl -lpspnet -lpsppower -lpspaudiocodec -lpsputility -lpspwlan -lpspsdk -lc
SFOFLAGS = -s APP_VER=02.00
PSPSDK ?= $(shell psp-config --pspsdk-path)

include $(PSPSDK)/lib/build.mak
