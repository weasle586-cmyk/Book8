#ifndef BOOK8_KERNEL_HWDIAG_H
#define BOOK8_KERNEL_HWDIAG_H

#include "../include/hwdiag.h"
#include "../drivers/nv_maxwell.h"

void hwdiag_init(const struct nv_gpu *gpu);
long hwdiag_info(struct book8_hwdiag_info *info);
long hwdiag_snapshot(struct book8_nvdiag_snapshot *snapshot);
long hwdiag_read32(struct book8_hwdiag_read32 *request);
long hwdiag_report(const struct book8_hwdiag_report *report);

#endif
