#ifndef NVME_H
#define NVME_H
#include <stdint.h>

/* NVMe, read-only: first namespace of the first controller (kernel/nvme.c).
 * nvme_init() returns 0 when a usable namespace was found. Sectors are
 * presented as 512 bytes whatever the namespace's logical block size. */
int nvme_init(void);
const char* nvme_model(void);
uint64_t nvme_sectors(void);
int nvme_read(uint64_t lba, uint32_t count, void* buf);

#endif
