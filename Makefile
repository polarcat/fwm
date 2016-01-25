cc = $(CROSS_COMPILE)gcc
out = yawm
src = yawm.c

ldflags = $(LDFLAGS) -lm -g
ldflags += -lxcb -lxcb-util -lxcb-keysyms
ldflags += $(shell pkg-config --libs xft) -lX11 -lX11-xcb
ccflags += $(CFLAGS) $(shell pkg-config --cflags xft)

.PHONY: all clean distclean
all: $(out)

$(out):
	$(cc) -o $(out) $(src) $(ccflags) $(ldflags)
	@echo "(==) $(out) compilation finished" | grep --color -E "^...."

clean:
	-rm -f $(out)

distclean: clean
