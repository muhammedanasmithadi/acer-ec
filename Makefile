obj-m += acer_ec_core.o
acer_ec_core-objs := src/acer_ec_core.o

obj-m += acer_fanctl.o
acer_fanctl-objs := src/acer_fanctl.o

obj-m += acer_ec_debug.o
acer_ec_debug-objs := src/acer_ec_debug.o

obj-m += acer_wmi_extras.o
acer_wmi_extras-objs := src/acer_wmi_extras.o

ccflags-y := -I$(src)/src

all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
