include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=mt7510-eth
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/mt7510-eth
  SUBMENU:=Network Devices
  TITLE:=EcoNet MT7510 PDMA Ethernet driver
  DEPENDS:=@TARGET_econet_en751221
  FILES:=$(PKG_BUILD_DIR)/mt7510-eth.ko
  AUTOLOAD:=$(call AutoProbe,mt7510-eth)
endef

define KernelPackage/mt7510-eth/description
  Ethernet driver for the MT7510 frame engine in PDMA mode (LAN/GDMA1).
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,mt7510-eth))
