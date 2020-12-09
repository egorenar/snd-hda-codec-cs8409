snd-hda-codec-cs8409-objs :=	patch_cs8409.o patch_cs8409-tables.o
obj-$(CONFIG_SND_HDA_CODEC_CS8409) += snd-hda-codec-cs8409.o

# debug build flags
#KBUILD_EXTRA_CFLAGS = "-DCONFIG_SND_DEBUG=1 -DMYSOUNDDEBUGFULL -DCONFIG_SND_HDA_RECONFIG=1 -Wno-unused-variable -Wno-unused-function"
# normal build flags
KBUILD_EXTRA_CFLAGS = "-DCONFIG_SND_HDA_RECONFIG=1 -Wno-unused-variable -Wno-unused-function"


ifdef KVER
KDIR := /lib/modules/$(KVER)
else
KDIR := /lib/modules/$(shell uname -r)
endif

all:
	make -C $(KDIR)/build CFLAGS_MODULE=$(KBUILD_EXTRA_CFLAGS) M=$(shell pwd) modules
clean:
	make -C $(KDIR)/build M=$(shell pwd) clean

install:
	mkdir -p $(KDIR)/updates/
	cp snd-hda-codec-cs8409.ko $(KDIR)/updates/
	depmod -a
