SAFERUN_GROUP ?= saferun

SafeRun: SafeRun.c
	gcc -Wall -Wno-parentheses -pthread -o $@ $^

install: SafeRun
	cp SafeRun /sbin/
	chgrp $(SAFERUN_GROUP) /sbin/SafeRun
	chmod 04750 /sbin/SafeRun
