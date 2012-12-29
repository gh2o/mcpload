KMAKE = make -C /lib/modules/$(shell uname -r)/build M=$(PWD)

ccflags-y += -Wno-declaration-after-statement
obj-m += mcpload.o

all:
	$(KMAKE) modules

clean:
	rm -f *.o *.ko
