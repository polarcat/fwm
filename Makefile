cc = $(CROSS_COMPILE)gcc

cflags = -Wall -Wunused-function

xftcflags = $(shell pkg-config --cflags xft)
xftldflags = $(shell pkg-config --libs xft)

.PHONY: all clean distclean
all: $(wmout) $(wmdout) $(deskout) $(clockout)

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

wmdout = yawmd
wmdsrc = yawmd.c
wmdcflags = $(cflags)
wmdldflags = -lxcb

$(wmdout):
	$(cc) -o $(wmdout) $(wmdsrc) $(wmdcflags) $(wmdldflags)
	@echo "(==) $(wmdout) done"

clean-$(wmdout):
	-rm -f $(wmdout)

deskout = deskd
desksrc = deskd.c misc.c
deskcflags = $(cflags) $(CFLAGS)

$(deskout):
	$(cc) -o $(deskout) $(desksrc) $(deskcflags)
	@echo "(==) $(deskout) done"

clean-$(deskout):
	-rm -f $(deskout)

install-$(deskout):
	-mkdir -p $(HOME)/bin
	-unlink $(HOME)/bin/$(deskout)
	-cp -v $(deskout) $(HOME)/bin/$(deskout)
	-chmod 755 $(HOME)/bin/$(deskout)

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

clean:
	-rm -f $(wmout) $(wmdout) $(deskout) $(clockout)

distclean: clean
