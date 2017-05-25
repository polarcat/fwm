cc = $(CROSS_COMPILE)gcc

cflags = -Wall -Wunused-function
cflags += $(CFLAGS)

xftcflags = $(shell pkg-config --cflags xft)
xftldflags = $(shell pkg-config --libs xft)

wmout = yawm
wmsrc = yawm.c
wmcflags = $(cflags) $(xftcflags) $(CFLAGS)
wmldflags = -lm -lxcb -lxcb-randr -lxcb-util -lxcb-keysyms
wmldflags += -lX11 -lX11-xcb $(xftldflags)

$(wmout):
	$(cc) -o $(wmout) $(wmsrc) $(wmcflags) $(wmldflags)
	@echo "(==) $(wmout) done"

clean-$(wmout):
	-rm -f $(wmout)

install-$(wmout):
	-mkdir -p $(HOME)/bin
	-unlink $(HOME)/bin/$(wmout)
	-cp -v $(wmout) $(HOME)/bin/$(wmout)
	-chmod 755 $(HOME)/bin/$(wmout)

clockout = clock
clocksrc = clock.c
clockcflags = $(xftcflags) $(cflags)
clockldflags = $(xftldflags) -lxcb -lX11 -lX11-xcb

$(clockout):
	$(cc) -o $(clockout) $(clocksrc) $(clockcflags) $(clockldflags)

clean-$(clockout):
	-rm -f $(clockout)

install-$(clockout):
	-mkdir -p $(HOME)/bin
	-unlink $(HOME)/bin/$(clockout)
	-cp -v $(clockout) $(HOME)/bin/$(clockout)
	-chmod 755 $(HOME)/bin/$(clockout)

menuout = menu
menusrc = menu.c
menucflags = $(xftcflags) $(cflags)
menuldflags = $(xftldflags) -lxcb -lX11 -lX11-xcb -lxcb-keysyms
menuldflags += -lxkbcommon -lxkbcommon-x11

$(menuout):
	$(cc) -o $(menuout) $(menusrc) $(menucflags) $(menuldflags)

clean-$(menuout):
	-rm -f $(menuout)

install-$(menuout):
	-mkdir -p $(HOME)/bin
	-unlink $(HOME)/bin/$(menuout)
	-cp -v $(menuout) $(HOME)/bin/$(menuout)
	-chmod 755 $(HOME)/bin/$(menuout)

install-tools:
	-mkdir -p $(HOME)/bin
	-mkdir -p $(HOME)/.yawm/center
	-cp -v tools/yawm-screens $(HOME)/bin/
	-chmod 755 $(HOME)/bin/yawm-screens
	-touch $(HOME)/.yawm/center/yawm-screens
	-cp -v tools/yawm-clients $(HOME)/bin/
	-chmod 755 $(HOME)/bin/yawm-clients
	-touch $(HOME)/.yawm/center/yawm-clients
	-cp -v tools/yawm-clipboard $(HOME)/bin/
	-chmod 755 $(HOME)/bin/yawm-clipboard
	-touch $(HOME)/.yawm/center/yawm-clipboard
	-cp -v tools/yawm-apps $(HOME)/bin/
	-chmod 755 $(HOME)/bin/yawm-apps
	-touch $(HOME)/.yawm/center/yawm-apps
	-cp -v tools/yawm-run $(HOME)/bin/
	-chmod 755 $(HOME)/bin/yawm-run
	-touch $(HOME)/.yawm/center/yawm-run

clean:
	-rm -f $(wmout) $(clockout)

distclean: clean

.PHONY: all clean distclean
all: $(wmout) $(clockout)
