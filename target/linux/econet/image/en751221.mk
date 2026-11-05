TRX_ENDIAN := be

# ASUS stock firmware compares the TrendChip "Model" field (0x5c) with
# /usr/etc/trx_config and stores the product name at 0x80.
DEVICE_VARS += ASUS_TRX_MODEL ASUS_PRODUCTID

define Build/asus-tclinux-header
	printf '%s\n' '$(ASUS_TRX_MODEL)' | \
		dd of=$@ bs=1 seek=$$((0x5c)) conv=notrunc 2>/dev/null
	printf '%s' '$(ASUS_PRODUCTID)' | \
		dd of=$@ bs=1 seek=$$((0x80)) conv=notrunc 2>/dev/null
endef

# The DSL-N16 bootloader LZMA-decompresses at most 0x1a0000 - 0x20100 bytes
# of flash, far less than vmlinuz. So it only unpacks a small lzma-loader,
# which then decompresses the OKLI-tagged kernel from memory-mapped flash.
define Build/econet-okli-loader
	rm -rf $@.src
	$(MAKE) -C lzma-loader \
		PKG_BUILD_DIR="$@.src" \
		TARGET_DIR="$(dir $@)" LOADER_NAME="$(notdir $@).loader" \
		SUBTARGET=en751221 \
		LZMA_TEXT_START=$(LOADER_TEXT_START) LOADADDR=0x80002000 \
		FLASH_START=0x1c000000 FLASH_OFFS=$(LOADER_FLASH_OFFS) FLASH_MAX=0 \
		DEBUG_LED_GPIO=$(LOADER_DEBUG_LED_GPIO) \
		compile loader.bin
	rm -rf $@.src
	# The bootloader skips its cache flush on MT7510; pad so the loader code is
	# evicted from the D-cache to RAM before the bootloader jumps to it.
	truncate -s 1M $@.loader.bin
	$(STAGING_DIR_HOST)/bin/lzma e $@.loader.bin $@.loader.lzma
	[ $$(stat -c%s $@.loader.lzma) -le $$(($(LOADER_FLASH_OFFS) - $(LOADER_TRX_OFFS))) ]
	dd if=$@.loader.lzma of=$@.new bs=$$(($(LOADER_FLASH_OFFS) - $(LOADER_TRX_OFFS))) \
		count=1 conv=sync 2>/dev/null
	cat $@ >> $@.new
	mv $@.new $@
	rm -f $@.loader.bin $@.loader.lzma
endef

DEVICE_VARS += LOADER_TEXT_START LOADER_FLASH_OFFS LOADER_TRX_OFFS LOADER_DEBUG_LED_GPIO

define Device/asus_dsl-n16
  DEVICE_VENDOR := ASUS
  DEVICE_MODEL := DSL-N16
  DEVICE_DTS := en751221_asus_dsl-n16
  LOADER_TEXT_START := 0x82000000
  # TRX at 0x20000, its 0x100 header, loader up to 0x30000, OKLI kernel at 0x30000
  LOADER_TRX_OFFS := 0x20100
  LOADER_FLASH_OFFS := 0x30000
  KERNEL := kernel-bin | append-dtb | lzma | uImage lzma -M 0x4f4b4c49 | \
    econet-okli-loader
  KERNEL_INITRAMFS := kernel-bin | append-dtb
  TRX_LOADADDR := 0x80002000
  ASUS_TRX_MODEL := 3 6035 122 0
  ASUS_PRODUCTID := DSL-N16
  IMAGE_SIZE := 16000k
  IMAGES := factory.trx sysupgrade.bin
  IMAGE/factory.trx := append-kernel | tclinux-trx | \
    asus-tclinux-header | check-size 14976k
  IMAGE/sysupgrade.bin := append-kernel | tclinux-trx | \
    asus-tclinux-header | check-size $$(IMAGE_SIZE) | append-metadata
  DEVICE_PACKAGES := kmod-mt7510-eth -kmod-econet-eth kmod-mt7603 wpad-basic-mbedtls
endef
TARGET_DEVICES += asus_dsl-n16

define Device/chinamobile_gs3101
  DEVICE_VENDOR := ChinaMobile
  DEVICE_MODEL := GS3101
  DEVICE_DTS := en7526f_chinamobile_gs3101
  KERNEL_SIZE := 4096k
  NAND_SIZE := 256m
  KERNEL := kernel-bin | append-dtb | lzma | kernel-trx
  KERNEL_INITRAMFS := kernel-bin | append-dtb
  IMAGES := tclinux.trx sysupgrade.bin
  IMAGE/tclinux.trx := append-kernel | pad-to $$$$(KERNEL_SIZE) | append-ubi
  IMAGE/sysupgrade.bin := sysupgrade-tar | append-metadata
  DEVICE_PACKAGES := kmod-usb3 kmod-mt7603
endef
TARGET_DEVICES += chinamobile_gs3101

define Device/en751221_generic
  DEVICE_VENDOR := EN751221 Family
  DEVICE_MODEL := Initramfs Image
  DEVICE_TITLE := EN751221 Initramfs Image
  DEVICE_DESCRIPTION := In-memory build for testing and recovery of EN751221 SoCs
  DEVICE_DTS := en751221_generic
endef
TARGET_DEVICES += en751221_generic

define Device/huawei_hg2821t-u
  DEVICE_VENDOR := Huawei
  DEVICE_MODEL := HG2821T-U
  DEVICE_DTS := en751221_huawei_hg2821t-u
  KERNEL_DECOMPRESSED_SIZE := 7672k
  KERNEL_SIZE := 4096k
  IMAGE_SIZE := 57344k
  FACTORY_SIZE := 40960k
  NAND_SIZE := 256m
  KERNEL := kernel-bin | append-dtb | tclinux-free-bootbase-jump | \
    check-size $$(KERNEL_DECOMPRESSED_SIZE) | lzma
  KERNEL_INITRAMFS := kernel-bin | append-dtb
  IMAGES := kernel.bin rootfs.bin sysupgrade.bin
  IMAGE/kernel.bin := append-kernel
  IMAGE/rootfs.bin := append-ubi | check-size $$(FACTORY_SIZE)
  IMAGE/sysupgrade.bin := sysupgrade-tar | append-metadata
  DEVICE_PACKAGES := kmod-usb3
endef
TARGET_DEVICES += huawei_hg2821t-u

define Device/nokia_g240g-e
  DEVICE_VENDOR := Nokia
  DEVICE_MODEL := G-240G-E
  DEVICE_DTS := en751221_nokia_g240g-e
  IMAGES := tclinux.trx
  IMAGE/tclinux.trx := append-kernel | lzma | tclinux-trx
  DEVICE_PACKAGES := kmod-usb3
endef
TARGET_DEVICES += nokia_g240g-e

define Device/smartfiber_xp8421-b
  DEVICE_VENDOR := SmartFiber
  DEVICE_MODEL := XP8421-B
  DEVICE_DTS := en751221_smartfiber_xp8421-b
  IMAGES := tclinux.trx
  IMAGE/tclinux.trx := append-kernel | lzma | tclinux-trx
  DEVICE_PACKAGES := kmod-usb3 kmod-mt7603 kmod-mt76x2
endef
TARGET_DEVICES += smartfiber_xp8421-b

# NOTE: This will not work for upgrading from factory because it requires a cryptographic signature
#       however, it it can be flashed, then it will boot correctly.
define Device/tplink_archer-vr1200v-v2
  DEVICE_VENDOR := TP-Link
  DEVICE_MODEL := Archer vr1200v
  DEVICE_VARIANT := v2
  TPLINK_FLASHLAYOUT := 16Mmtk
  TPLINK_HWID := 0x0b473502
  TPLINK_HWREV := 0x0006007c
  TPLINK_HWREVADD := 0x0
  TPLINK_HVERSION := 3
  DEVICE_DTS := en751221_tplink_archer-vr1200v-v2
  DEVICE_PACKAGES := kmod-mt7603 kmod-mt7615e kmod-mt7663-firmware-ap
  IMAGES := sysupgrade.bin
  IMAGE/sysupgrade.bin := append-kernel | lzma | pad-to 4193792 | append-rootfs | \
    tplink-v2-header -R 0x400000
endef
TARGET_DEVICES += tplink_archer-vr1200v-v2

define Device/zyxel_pmg5617ga
  DEVICE_VENDOR := Zyxel
  DEVICE_MODEL := PMG5617GA
  DEVICE_DTS := en751221_zyxel_pmg5617ga
  IMAGES := tclinux.trx
  IMAGE/tclinux.trx := append-kernel | lzma | tclinux-trx
  DEVICE_PACKAGES := kmod-usb3 kmod-mt7603 kmod-mt76x2
endef
TARGET_DEVICES += zyxel_pmg5617ga
