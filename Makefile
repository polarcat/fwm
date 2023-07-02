common = build/Makefile.common
cc = $(CROSS_COMPILE)gcc
cflags = -Wall -Wunused-function
cflags += $(CFLAGS)
ldflags = $(LDFLAGS)
ftcflags = $(shell pkg-config --cflags freetype2)
ftldflags = $(shell pkg-config --libs freetype2) -lxcb-image
xftcflags = $(shell pkg-config --cflags xft)
xftldflags = $(shell pkg-config --libs xft)
target = $@
destdir = $(DESTDIR)
homedir = $(DESTDIR)$(HOME)
basedir = $(homedir)/.fwm
bindir = $(basedir)/bin
keysdir = $(basedir)/keys

ifdef $(SCREEN)
defscr = $(SCREEN)
else
defscr = 0
endif

dockdir = $(basedir)/screens/$(defscr)/dock

export

makecmd = make -f build/Makefile.$@

ifneq (,$(findstring clean,$(MAKECMDGOALS)))
makecmd += clean
else ifneq (,$(findstring install,$(MAKECMDGOALS)))
makecmd += install
else
makecmd += $@
endif

second = $(word 2,$(MAKECMDGOALS))

.PHONY: FORCE dirs all install

dirs:
	@mkdir -p bin
	@mkdir -p $(basedir)
	@mkdir -p $(bindir)
	@mkdir -p $(keysdir)
	@mkdir -p $(dockdir)

ifeq ($(second),)
install: dirs
	@for target in $(all); do \
		make -f build/Makefile.$$target install; \
	done
endif

all = fwm menu clock cpumon dock netlink rtlink tools icons sudoers

all: $(all)

fwm: FORCE dirs
	$(makecmd)

menu: FORCE dirs
	$(makecmd)

clock: FORCE dirs
	$(makecmd)

cpumon: FORCE dirs
	$(makecmd)

dock: FORCE dirs
	$(makecmd)

netlink: FORCE dirs
	$(makecmd)

rtlink: FORCE dirs
	$(makecmd)

tools: FORCE dirs
	$(makecmd)

icons: FORCE dirs
	$(makecmd)

sudoers: FORCE dirs
	$(makecmd)
