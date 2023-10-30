// SPDX-License-Identifier: GPL-2.0-only
/*
 * Microblaze support for cache consistent memory.
 * Copyright (C) 2010 Michal Simek <monstr@monstr.eu>
 * Copyright (C) 2010 PetaLogix
 * Copyright (C) 2005 John Williams <jwilliams@itee.uq.edu.au>
 *
 * Based on PowerPC version derived from arch/arm/mm/consistent.c
 * Copyright (C) 2001 Dan Malek (dmalek@jlc.net)
 * Copyright (C) 2000 Russell King
 */

#include <linux/export.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ptrace.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/stddef.h>
#include <linux/vmalloc.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/memblock.h>
#include <linux/highmem.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/gfp.h>
#include <linux/dma-noncoherent.h>

#include <asm/pgalloc.h>
#include <linux/io.h>
#include <linux/hardirq.h>
#include <linux/mmu_context.h>
#include <asm/mmu.h>
#include <linux/uaccess.h>
#include <asm/pgtable.h>
#include <asm/cpuinfo.h>
#include <asm/tlbflush.h>

#ifndef CONFIG_MMU
/* I have to use dcache values because I can't relate on ram size */
# define UNCACHED_SHADOW_MASK (cpuinfo.dcache_high - cpuinfo.dcache_base + 1)
#endif

/*
 * Consistent memory allocators. Used for DMA devices that want to
 * share uncached memory with the processor core.
 * My crufty no-MMU approach is simple. In the HW platform we can optionally
 * mirror the DDR up above the processor cacheable region.  So, memory accessed
 * in this mirror region will not be cached.  It's alloced from the same
 * pool as normal memory, but the handle we return is shifted up into the
 * uncached region.  This will no doubt cause big problems if memory allocated
 * here is not also freed properly. -- JW
 */
void *arch_dma_alloc(struct device *dev, size_t size, dma_addr_t *dma_handle,
		gfp_t gfp, unsigned long attrs)
{
	unsigned long order, vaddr;
	void *ret;
	unsigned int i, err = 0;
	struct page *page, *end;

#ifdef CONFIG_MMU
	phys_addr_t pa;
	struct vm_struct *area;
	unsigned long va;
#endif

	if (in_interrupt())
		BUG();

	/* Only allocate page size areas. */
	size = PAGE_ALIGN(size);
	order = get_order(size);

	vaddr = __get_free_pages(gfp | __GFP_ZERO, order);
	if (!vaddr)
		return NULL;

	/*
	 * we need to ensure that there are no cachelines in use,
	 * or worse dirty in this area.
	 */
	flush_dcache_range(virt_to_phys((void *)vaddr),
					virt_to_phys((void *)vaddr) + size);

#ifndef CONFIG_MMU
	ret = (void *)vaddr;
	/*
	 * Here's the magic!  Note if the uncached shadow is not implemented,
	 * it's up to the calling code to also test that condition and make
	 * other arranegments, such as manually flushing the cache and so on.
	 */
# ifdef CONFIG_XILINX_UNCACHED_SHADOW
	ret = (void *)((unsigned) ret | UNCACHED_SHADOW_MASK);
# endif
	if ((unsigned int)ret > cpuinfo.dcache_base &&
				(unsigned int)ret < cpuinfo.dcache_high)
		pr_warn("ERROR: Your cache coherent area is CACHED!!!\n");

	/* dma_handle is same as physical (shadowed) address */
	*dma_handle = (dma_addr_t)ret;
#else
	/* Allocate some common virtual space to map the new pages. */
	area = get_vm_area(size, VM_ALLOC);
	if (!area) {
		free_pages(vaddr, order);
		return NULL;
	}
	va = (unsigned long) area->addr;
	ret = (void *)va;

	/* This gives us the real physical address of the first page. */
	*dma_handle = pa = __virt_to_phys(vaddr);
#endif

	/*
	 * free wasted pages.  We skip the first page since we know
	 * that it will have count = 1 and won't require freeing.
	 * We also mark the pages in use as reserved so that
	 * remap_page_range works.
	 */
	page = virt_to_page(vaddr);
	end = page + (1 << order);

	split_page(page, order);

	for (i = 0; i < size && err == 0; i += PAGE_SIZE) {
#ifdef CONFIG_MMU
		/* MS: This is the whole magic - use cache inhibit pages */
		err = map_page(va + i, pa + i, _PAGE_KERNEL | _PAGE_NO_CACHE);
#endif

		SetPageReserved(page);
		page++;
	}

	/* Free the otherwise unused pages. */
	while (page < end) {
		__free_page(page);
		page++;
	}

	if (err) {
		free_pages(vaddr, order);
		return NULL;
	}

	return ret;
}

#ifdef CONFIG_MMU
static pte_t *consistent_virt_to_pte(void *vaddr)
{
	unsigned long addr = (unsigned long)vaddr;

	return pte_offset_kernel(pmd_offset(pgd_offset_k(addr), addr), addr);
}

long arch_dma_coherent_to_pfn(struct device *dev, void *vaddr,
		dma_addr_t dma_addr)
{
	pte_t *ptep = consistent_virt_to_pte(vaddr);

	if (pte_none(*ptep) || !pte_present(*ptep))
		return 0;

	return pte_pfn(*ptep);
}
#endif

/*
 * free page(s) as defined by the above mapping.
 */
void arch_dma_free(struct device *dev, size_t size, void *vaddr,
		dma_addr_t dma_addr, unsigned long attrs)
{
	struct page *page;

	if (in_interrupt())
		BUG();

	size = PAGE_ALIGN(size);

#ifndef CONFIG_MMU
	/* Clear SHADOW_MASK bit in address, and free as per usual */
# ifdef CONFIG_XILINX_UNCACHED_SHADOW
	vaddr = (void *)((unsigned)vaddr & ~UNCACHED_SHADOW_MASK);
# endif
	page = virt_to_page(vaddr);

	do {
		__free_reserved_page(page);
		page++;
	} while (size -= PAGE_SIZE);
#else
	do {
		pte_t *ptep = consistent_virt_to_pte(vaddr);
		unsigned long pfn;

		if (!pte_none(*ptep) && pte_present(*ptep)) {
			pfn = pte_pfn(*ptep);
			pte_clear(&init_mm, (unsigned int)vaddr, ptep);
			if (pfn_valid(pfn)) {
				page = pfn_to_page(pfn);
				__free_reserved_page(page);
			}
		}
		vaddr += PAGE_SIZE;
	} while (size -= PAGE_SIZE);

	/* flush tlb */
	flush_tlb_all();
#endif
}