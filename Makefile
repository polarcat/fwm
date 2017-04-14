cc = $(CROSS_COMPILE)gcc

cflags = -Wall -Wunused-function

xftcflags = $(shell pkg-config --cflags xft)
xftldflags = $(shell pkg-config --libs xft)

wmout = yawm
wmsrc = yawm.c
wmcflags = $(cflags) $(xftcflags) $(CFLAGS) -g
wmldflags = -lm -lxcb -lxcb-randr -lxcb-util -lxcb-keysyms -g
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

install-tools:
	-mkdir -p $(HOME)/bin
	-cp -v tools/yawm-screens $(HOME)/bin/
	-chmod 755 $(HOME)/bin/yawm-screens
	-touch $(HOME)/.yawm/center/yawm-screens
	-cp -v tools/yawm-clients $(HOME)/bin/
	-chmod 755 $(HOME)/bin/yawm-clients
	-touch $(HOME)/.yawm/center/yawm-clients
	-mkdir -p $(HOME)/.yawm/center
	-cp -v tools/dialogrc $(HOME)/.yawm/

clean:
	-rm -f $(wmout) $(clockout)

distclean: clean

.PHONY: all clean distclean
all: $(wmout) $(clockout)
