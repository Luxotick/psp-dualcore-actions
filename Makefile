TARGET = psp-dualcore-actions
OBJS = main.o me_loop.o spotify/shannon.o spotify/sha1.o spotify/dh.o spotify/handshake.o spotify/config.o spotify/login.o spotify/audiokey.o spotify/http.o spotify/log.o spotify/tls.o spotify/webapi.o spotify/audiodecrypt.o spotify/player.o spotify/me_vorbis.o spotify/me_trampoline.o spotify/json.o spotify/session.o spotify/gfx.o spotify/ui.o

CFLAGS = -Os -G0 -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wstrict-prototypes -fno-pic -fno-lto -std=gnu11 -I.
CFLAGS += -isystem $(PSPDEV)/psp/include -isystem $(PSPSDK)/include
LDFLAGS = -Wl,-Map=psp-dualcore-actions.map
BUILD_PRX = 1
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSPotify ME
LIBS = -lme-stask -lme-core-mapper -lpspdebug -lpspctrl -lpspdisplay -lpspge -lpspaudio -lpsprtc -lpspnet_resolver -lpspnet_inet -lpspnet_apctl -lpspnet -lpsppower -lpspaudiocodec -lpsputility -lpspwlan -lpspsdk -lc
SFOFLAGS = -s APP_VER=02.00
PSPSDK ?= $(shell psp-config --pspsdk-path)

include $(PSPSDK)/lib/build.mak
