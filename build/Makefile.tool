key = $(firstword $(subst :, ,$(target)))
pos = $(word 2,$(subst :, ,$(target)))
type = $(word 3,$(subst :, ,$(target)))
cmd = $(word 4,$(subst :, ,$(target)))
run = \#!/bin/sh\nexec $(cmd) &\n
note = key \033[0;31m$(keysdir)/$(key) \033[0malready exists, skip\n

.PHONY: FORCE clean install

install:
	@echo "install key=$(key) pos=$(pos) type=$(type) cmd=$(cmd)"
	@mkdir -p $(basedir)/popup; \
	if [ "$(type)" != "sys" ]; then \
		cp -v tools/$(cmd) $(bindir)/; \
		chmod 0755 $(bindir)/$(cmd); \
	elif [ $(type) = "popup" ]; then \
		touch $(basedir)/popup/$(cmd); \
	fi
	@if [ "$(pos)" != "null" ]; then \
		if [ "$(type)" = "dock" ]; then \
			mkdir -p $(dockdir)/$(pos); \
			touch $(dockdir)/$(pos)/$(cmd); \
		else \
			mkdir -p $(basedir)/$(pos); \
			touch $(basedir)/$(pos)/$(cmd); \
		fi \
	else \
		if [ "$(type)" = "dock" ]; then \
			touch $(dockdir)/$(cmd); \
		fi \
	fi
	@if [ "$(key)" != "null" ]; then \
		if [ -f $(keysdir)/$(key) ]; then \
			printf "$(note)"; \
		else \
			printf "$(run)" > $(keysdir)/$(key); \
			chmod 0755 $(keysdir)/$(key); \
		fi \
	fi
	@touch $(basedir)/center/menu; \
	touch $(basedir)/popup/menu; \
	touch $(basedir)/popup/popup; \
	cd $(basedir)/screens/$(defscr)/dock; \
	ln -sfv clock-dock right-anchor; \
	ln -sfv bat-dock left-anchor; \
	if pidof fwm && [ -p $(basedir)/.control$(DISPLAY) ]; then \
		echo "reload-keys" > $(basedir)/.control$(DISPLAY); \
	fi
