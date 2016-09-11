cc = $(CROSS_COMPILE)gcc
wmout = yawm
wmsrc = yawm.c
wmdout = yawmd
wmdsrc = yawmd.c
deskout = deskd
desksrc = deskd.c misc.c

ldflags = $(LDFLAGS) -lm -g
ldflags += -lxcb -lxcb-randr -lxcb-util -lxcb-keysyms
ldflags += $(shell pkg-config --libs xft) -lX11 -lX11-xcb
ldflags += -Wall -Wunused-function
ccflags += $(CFLAGS) $(shell pkg-config --cflags xft)
ccflags += -Wall -Wunused-function

.PHONY: all clean distclean
all: $(wmout) $(wmdout) $(deskout)

$(wmout):
	$(cc) -o $(wmout) $(wmsrc) $(ccflags) $(ldflags)
	@echo "(==) $(wmout) compilation finished"

clean$(wmout):
	-rm -f $(wmout)

install$(wmout):
	-mkdir -p $(HOME)/bin
	-unlink $(HOME)/bin/$(wmout)
	-cp -v $(wmout) $(HOME)/bin/$(wmout)
	-chmod 755 $(HOME)/bin/$(wmout)

$(wmdout):
	$(cc) -o $(wmdout) $(wmdsrc) $(CFLAGS) -lxcb
	@echo "(==) $(wmdout) compilation finished"

clean$(wmdout):
	-rm -f $(wmdout)

$(deskout):
	$(cc) -o $(deskout) $(desksrc) $(CFLAGS)
	@echo "(==) $(deskout) compilation finished"

clean$(deskout):
	-rm -f $(deskout)

install$(deskout):
	-mkdir -p $(HOME)/bin
	-unlink $(HOME)/bin/$(deskout)
	-cp -v $(deskout) $(HOME)/bin/$(deskout)
	-chmod 755 $(HOME)/bin/$(deskout)

clean:
	-rm -f $(wmout) $(wmdout) $(deskout)

distclean: clean
