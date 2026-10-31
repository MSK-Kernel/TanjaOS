# OpenBSD-derived sources (bsd/src) and TanjaOS compatibility layer (bsd/compat)
BSD_SRC = $(sort $(wildcard bsd/src/*/*.c) $(wildcard bsd/src/*/*/*.c))
BSD_COMPAT_SRC = bsd/compat/bsd_core.c bsd/compat/bsd_mbuf.c bsd/compat/bsd_net.c bsd/compat/bsd_pci.c bsd/compat/bsd_wifi.c
