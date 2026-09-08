This card was prepared by MaryPi for a Raspberry Pi 5.

Files on this FAT partition:
  config.txt            Raspberry Pi firmware configuration (rpi5-uefi's file
                        plus a MaryPi block at the end)
  RPI_EFI.fd            EDK2 UEFI firmware for the Pi 5 (worproject/rpi5-uefi)
  bcm2712-rpi-5-b.dtb   Device tree for the Pi 5
  MARYPI.txt            What MaryPi put on this card and when
  EFI/BOOT/BOOTAA64.EFI ravynOS booter (only when a payload level >= 1 was available)
  ravynos/              ravynOS kernel and boot plist (payload level >= 1)

The second partition (HFS+, "ravynOS") is the root filesystem. It is empty
unless a full system (payload level 2) was available when the card was made.

Serial console: connect a 3.3 V USB-UART adapter to the Pi 5's 3-pin debug
header (GND, TX, RX) and open it at 115200 8N1.

The Pi 5 bootloader EEPROM must be recent enough for rpi5-uefi. If nothing
appears on HDMI or serial, update the EEPROM with Raspberry Pi Imager first.
MaryPi never touches the EEPROM.
