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

wmdout = yawmd
wmdsrc = yawmd.c
wmdcflags = $(cflags)
wmdldflags = -lxcb

$(wmdout):
	$(cc) -o $(wmdout) $(wmdsrc) $(wmdcflags) $(wmdldflags)
	@echo "(==) $(wmdout) done"

clean-$(wmdout):
	-rm -f $(wmdout)

install-$(wmdout):
	-mkdir -p $(HOME)/bin
	-unlink $(HOME)/bin/$(wmdout)
	-cp -v $(wmdout) $(HOME)/bin/$(wmdout)
	-chmod 755 $(HOME)/bin/$(wmdout)

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
	-rm -f $(wmout) $(wmdout) $(clockout)

distclean: clean

.PHONY: all clean distclean
all: $(wmout) $(wmdout) $(clockout)
