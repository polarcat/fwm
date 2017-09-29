common = build/Makefile.common
cc = $(CROSS_COMPILE)gcc
cflags = -Wall -Wunused-function
cflags += $(CFLAGS)
ldflags = $(LDFLAGS)
xftcflags = $(shell pkg-config --cflags xft)
xftldflags = $(shell pkg-config --libs xft)

export

makecmd = make -f build/Makefile.$@

ifneq (,$(findstring clean,$(MAKECMDGOALS)))
makecmd += clean
else ifneq (,$(findstring install,$(MAKECMDGOALS)))
makecmd += install
else
makecmd += $@
endif

.PHONY: FORCE all clean install

all:
	@printf "\nUsage: make [clean|install] <yawm|menu|clock|cpumon|dock>\n\n"

yawm: FORCE
	$(makecmd)

menu: FORCE
	$(makecmd)

clock: FORCE
	$(makecmd)

cpumon: FORCE
	$(makecmd)

dock: FORCE
	$(makecmd)

netlink: FORCE
	$(makecmd)
