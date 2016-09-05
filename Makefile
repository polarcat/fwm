cc = $(CROSS_COMPILE)gcc
wmout = yawm
wmsrc = yawm.c
dout = yawmd
dsrc = yawmd.c

ldflags = $(LDFLAGS) -lm -g
ldflags += -lxcb -lxcb-randr -lxcb-util -lxcb-keysyms
ldflags += $(shell pkg-config --libs xft) -lX11 -lX11-xcb
ccflags += $(CFLAGS) $(shell pkg-config --cflags xft)

.PHONY: all clean distclean
all: $(wmout) $(dout)

$(wmout):
	$(cc) -o $(wmout) $(wmsrc) $(ccflags) $(ldflags)
	@echo "(==) $(wmout) compilation finished" | grep --color -E "^...."

$(dout):
	$(cc) -o $(dout) $(dsrc) $(CFLAGS) -lxcb
	@echo "(==) $(dout) compilation finished" | grep --color -E "^...."

clean:
	-rm -f $(wmout) $(dout)

distclean: clean
