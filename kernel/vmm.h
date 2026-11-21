#ifndef BOOK8_VMM_H
#define BOOK8_VMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool vmm_init(const void *multiboot_info);
void *vmm_alloc_pages(uint32_t count);
void *vmm_alloc_contiguous_pages(uint32_t count);
void vmm_free_pages(void *base, uint32_t count);
bool vmm_dma_address(const void *pointer, size_t size, uint64_t *physical);
bool vmm_handle_page_fault(uint64_t address, uint64_t error_code);
bool vmm_swap_selftest(void);
uint32_t vmm_page_count(void);
uint32_t vmm_swapped_page_count(void);

#endif
