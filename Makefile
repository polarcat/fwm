common = build/Makefile.common
cc = $(CROSS_COMPILE)gcc
cflags = -Wall -Wunused-function
cflags += $(CFLAGS)
ldflags = $(LDFLAGS)
xftcflags = $(shell pkg-config --cflags xft)
xftldflags = $(shell pkg-config --libs xft)
target = $@

export

makecmd = make -f build/Makefile.$@

ifneq (,$(findstring clean,$(MAKECMDGOALS)))
makecmd += clean
else ifneq (,$(findstring install,$(MAKECMDGOALS)))
makecmd += install
else
makecmd += $@
endif

.PHONY: FORCE bin all clean install

bin:
	@mkdir -p bin

all:
	@printf "\nUsage: make [clean|install] <fwm|menu|clock|cpumon|dock|netlink>\n\n"

fwm: FORCE bin
	$(makecmd)

menu: FORCE bin
	$(makecmd)

clock: FORCE bin
	$(makecmd)

cpumon: FORCE bin
	$(makecmd)

dock: FORCE bin
	$(makecmd)

netlink: FORCE bin
	$(makecmd)
