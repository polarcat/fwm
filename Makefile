cc = $(CROSS_COMPILE)gcc
out = yawm
src = main.c

ldflags = $(LDFLAGS) -lm -lxcb -lxcb-util -lxcb-icccm -g
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
