#!/bin/sh

set -e
CMD=${1:-check}
DEV=$2
ROOT=$(cd "$(dirname "$0")/.." && pwd)

if [ -z "$DEV" ]; then
    for d in /sys/bus/pci/devices/*; do
        [ "$(cat $d/class 2>/dev/null)" = "0x028000" ] && { DEV=$(basename $d); break; }
    done
fi
[ -n "$DEV" ] || { echo "no Wi-Fi-class PCI device found; pass its address (lspci -nn)"; exit 1; }
SYS=/sys/bus/pci/devices/$DEV
[ -d "$SYS" ] || { echo "$DEV: no such PCI device"; exit 1; }

need_root() { [ "$(id -u)" = 0 ] || { echo "run this as root (sudo)"; exit 1; }; }
drv() { [ -e $SYS/driver ] && basename "$(readlink $SYS/driver)" || echo "(none)"; }

case "$CMD" in
check)
    echo "device : $DEV  $(lspci -nns ${DEV#0000:} 2>/dev/null | cut -d' ' -f2-)"
    echo "driver : $(drv)"
    if [ -d /sys/kernel/iommu_groups ] && [ -n "$(ls /sys/kernel/iommu_groups 2>/dev/null)" ]; then
        G=$(basename "$(readlink $SYS/iommu_group)")
        echo "IOMMU  : on, group $G contains:"
        for m in /sys/kernel/iommu_groups/$G/devices/*; do echo "           $(basename $m)  $(lspci -nns ${m##*/} 2>/dev/null | cut -d' ' -f2- | cut -c1-70)"; done
        echo "         (best case: only the Wi-Fi card, maybe a PCIe bridge)"
    else
        echo "IOMMU  : OFF. Enable VT-d / AMD-Vi in the BIOS, then add  intel_iommu=on iommu=pt  (AMD: amd_iommu=on)"
        echo "         to GRUB_CMDLINE_LINUX_DEFAULT in /etc/default/grub, run update-grub and reboot."
    fi
    command -v qemu-system-i386 >/dev/null && echo "qemu   : found" || echo "qemu   : missing (apt install qemu-system-x86)"
    [ -f "$ROOT/arch/x86/boot/tanja-base" ] && echo "image  : built" || echo "image  : not built yet (run make)"
    ls $ROOT/home/lib/firmware/*.ucode >/dev/null 2>&1 && echo "firmware: $(ls $ROOT/home/lib/firmware/*.ucode | xargs -n1 basename | tr '\n' ' ')" || echo "firmware: none in home/lib/firmware (make copies it from /lib/firmware if present)"
    ;;
bind)
    need_root
    modprobe vfio-pci
    [ -e $SYS/driver ] && echo $DEV > $SYS/driver/unbind
    echo vfio-pci > $SYS/driver_override
    echo $DEV > /sys/bus/pci/drivers_probe
    echo "$DEV now uses: $(drv)"
    ;;
run)
    need_root
    [ "$(drv)" = "vfio-pci" ] || { echo "bind the card first:  $0 bind $DEV"; exit 1; }
    exec qemu-system-i386 -kernel "$ROOT/arch/x86/boot/tanja-base" -m 512 \
        -nic user,model=e1000 -device vfio-pci,host=$DEV -serial stdio
    ;;
unbind)
    need_root
    [ -e $SYS/driver ] && echo $DEV > $SYS/driver/unbind
    echo > $SYS/driver_override
    echo $DEV > /sys/bus/pci/drivers_probe
    echo "$DEV now uses: $(drv)"
    ;;
*) sed -n 2,13p "$0"; exit 1 ;;
esac
