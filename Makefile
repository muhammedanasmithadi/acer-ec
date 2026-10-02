# acer-ec — out-of-tree kernel modules
#
# Command-line build:
#   make [KDIR=/lib/modules/<ver>/build] [W=1]
#   sudo make modules_install
#   make check    (checkpatch + shellcheck, skips missing tools)
#
# DKMS drives the kbuild half of this file directly
# (dkms.conf MAKE=), so the defaults below must stay sane
# when invoked from /lib/modules/<ver>/build with M=<src>.

ifneq ($(KERNELRELEASE),)

obj-m += acer_ec_core.o
acer_ec_core-objs := src/acer_ec_core.o

obj-m += acer_fanctl.o
acer_fanctl-objs := src/acer_fanctl.o

obj-m += acer_ec_debug.o
acer_ec_debug-objs := src/acer_ec_debug.o

obj-m += acer_wmi_extras.o
acer_wmi_extras-objs := src/acer_wmi_extras.o

ccflags-y := -I$(src)/src

INSTALL_MOD_DIR := extra

else

KDIR ?= /lib/modules/$(shell uname -r)/build
SRC := $(CURDIR)

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(SRC) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(SRC) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(SRC) clean

help:
	$(MAKE) -C $(KDIR) M=$(SRC) help

# Static gates. Missing tools are skipped, never fatal.
# Runs every tool over every file, then fails if anything reported.
check:
	@fail=0; \
	if test -x $(KDIR)/scripts/checkpatch.pl; then \
		for f in src/*.c; do \
			echo "== checkpatch: $$f"; \
			$(KDIR)/scripts/checkpatch.pl --no-tree --file $$f || fail=1; \
		done; \
	else \
		echo "checkpatch.pl not found under $(KDIR), skipping"; \
	fi; \
	if command -v shellcheck >/dev/null 2>&1; then \
		shellcheck -S warning install.sh uninstall.sh src/acer-ec.sh scripts/charging-probe.sh || fail=1; \
	else \
		echo "shellcheck not found, skipping"; \
	fi; \
	exit $$fail

.PHONY: all modules modules_install clean help check

endif
