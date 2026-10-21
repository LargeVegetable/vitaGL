/*
 * This file is part of vitaGL
 * Copyright 2017, 2018, 2019, 2020 Rinnegatamante
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * mem_utils.c:
 * Utilities for memory management
 */

#include <malloc.h>
#include "../shared.h"

// HL2 Vita debugging: with VGL_HEAP_CANARIES, the public allocators below are
// wrappers (see the end of this file) and these are the real implementations
#ifdef VGL_HEAP_CANARIES
#define VGL_RAW(name) name##_raw
#else
#define VGL_RAW(name) name
#endif

GLboolean has_cached_mem = GL_FALSE; // Flag for whether to use cached memory for mempools or not

#ifndef HAVE_CUSTOM_HEAP
static void *mempool_mspace[VGL_MEM_ALL - 1] = {}; // sceClibMspace heaps
#endif
static void *mempool_addr[VGL_MEM_ALL] = {}; // Base addresses of heap memblocks
static void *mempool_end[VGL_MEM_ALL] = {}; // Addresses of heap memblocks ends
static size_t mempool_size[VGL_MEM_ALL] = {}; // Sizes (in bytes) of heap memblocks

GLboolean vgl_has_cdlg_support = GL_TRUE;

#ifdef HAVE_WRAPPED_ALLOCATORS
void *__real_calloc(uint32_t nmember, uint32_t size);
void __real_free(void *addr);
void *__real_malloc(uint32_t size);
void *__real_memalign(uint32_t alignment, uint32_t size);
void *__real_realloc(void *ptr, uint32_t size);
#endif

#ifdef HAVE_CUSTOM_HEAP

// Lightweighted mutexes for thread-safeness
#ifndef HAVE_SINGLE_THREADED_GC
static SceKernelLwMutexWork tm_mutexes[VGL_MEM_ALL - 1] = {};
static SceKernelLwMutexWork header_mutex;
#define tm_lock_mutex(mtx) sceKernelLockLwMutex(mtx, 1, NULL);
#define tm_unlock_mutex(mtx) sceKernelUnlockLwMutex(mtx, 1);
#else
#define tm_lock_mutex(mtx)
#define tm_unlock_mutex(mtx)
#endif

// Minimum size in bytes for a block (header + footer)
#define MIN_FREE_BLOCK_SIZE (8)

// Memblock header
typedef struct tm_block_s {
	struct tm_block_s *next;
	struct tm_block_s *prev;
	uintptr_t base;
	uint32_t size;
	vglMemType type;
	GLboolean used;
} tm_block_t;

static tm_block_t *tm_alloclist[VGL_MEM_ALL - 1] = {}; // Lists of allocated blocks
static tm_block_t *tm_freelist[VGL_MEM_ALL - 1] = {}; // Lists of available free blocks
static uint32_t tm_free[VGL_MEM_ALL - 1] = {}; // Amount of raw free memory per heap type
static uint32_t tm_max_failed_alloc[VGL_MEM_ALL - 1] = {}; // Last requested allocation size in bytes that failed

#define HEADERS_PER_CHUNK (8192) // Number of headers to alloc per chunk when headers list is exhausted
static tm_block_t *tm_free_headers = NULL; // Available memblock headers list

/*
 * Get a new memblock header for an allocation.
 * Here we use malloc-ed pointers rather than appending the actual header on the block itself
 * so that these headers are fast to access by the CPU (since vitaGL uses uncached memory by default.
 */
static tm_block_t *heap_blk_new(void) {
	tm_lock_mutex(&header_mutex)
	if (!tm_free_headers) {
		vgl_log("Extending heap headers list with an extra chunk.\n");
#ifdef HAVE_WRAPPED_ALLOCATORS
		tm_block_t *chunk = __real_malloc(sizeof(tm_block_t) * HEADERS_PER_CHUNK);
#else
		tm_block_t *chunk = malloc(sizeof(tm_block_t) * HEADERS_PER_CHUNK);
#endif
		
#ifndef SKIP_ERROR_HANDLING
		if (!chunk) {
			vgl_log("%s:%d Out of memory: cannot allocate new headers for memory blocks!\n", __FILE__, __LINE__);	
			tm_unlock_mutex(&header_mutex)
			return NULL;
		}
#endif
		for (int i = 0; i < HEADERS_PER_CHUNK - 1; i++) {
			chunk[i].next = &chunk[i + 1];
		}
		chunk[HEADERS_PER_CHUNK - 1].next = NULL;
		tm_free_headers = &chunk[0];
	}
	tm_block_t *h = tm_free_headers;
	tm_free_headers = h->next;
	tm_unlock_mutex(&header_mutex)
	
	vgl_memset(h, 0, sizeof(tm_block_t));
	return h;
}

// Release a memblock header
static inline __attribute__((always_inline)) void heap_blk_release(tm_block_t *block) {
	tm_lock_mutex(&header_mutex)
	block->next = tm_free_headers;
	tm_free_headers = block;
	tm_unlock_mutex(&header_mutex)
}

// Insert a new block in the free blocks list
static void heap_blk_insert_free(tm_block_t *block) {
	block->used = GL_FALSE;
	uint32_t incoming_size = block->size;

	*(tm_block_t **)block->base = block;
	*(tm_block_t **)(block->base + block->size - 4) = block;
	
	uintptr_t right_addr = block->base + block->size;
	if (right_addr < (uintptr_t)mempool_end[block->type]) {
		tm_block_t *right = *(tm_block_t **)right_addr;
		if (right->used == GL_FALSE) {
			if (right->prev)
				right->prev->next = right->next;
			else
				tm_freelist[block->type] = right->next;
			if (right->next)
				right->next->prev = right->prev;

			block->size += right->size;
			*(tm_block_t **)(block->base + block->size - 4) = block;
			heap_blk_release(right);
		}
	}

	uintptr_t left_footer_addr = block->base - 4;
	if (left_footer_addr >= (uintptr_t)mempool_addr[block->type]) {
		tm_block_t *left = *(tm_block_t **)left_footer_addr;
		if (left->used == GL_FALSE) {
			if (left->prev)
				left->prev->next = left->next;
			else
				tm_freelist[block->type] = left->next;
			if (left->next)
				left->next->prev = left->prev;

			left->size += block->size;
			*(tm_block_t **)(left->base + left->size - 4) = left;
			heap_blk_release(block);
			block = left;
		}
	}

	tm_free[block->type] += incoming_size;

	block->prev = NULL;
	block->next = tm_freelist[block->type];
	if (tm_freelist[block->type])
		tm_freelist[block->type]->prev = block;
	tm_freelist[block->type] = block;
}

// Allocs a new block and returns it
static tm_block_t *heap_blk_alloc(int32_t type, uint32_t size, uint32_t alignment) {
	tm_block_t *curblk = tm_freelist[type];
	tm_block_t *prevblk = NULL;

	while (curblk) {
		uint32_t skip = VGL_ALIGN(curblk->base + 4, alignment) - 4 - curblk->base;
		if (skip != 0) {
			while (skip < MIN_FREE_BLOCK_SIZE) {
				skip += alignment;
			}
		}

		if (skip + size <= curblk->size) {
			tm_block_t *skipblk = NULL;
			tm_block_t *unusedblk = NULL;

			uint32_t remaining = curblk->size - skip - size;
			GLboolean has_extra_block = remaining >= MIN_FREE_BLOCK_SIZE;

			if (skip != 0) {
				skipblk = heap_blk_new();
				if (!skipblk)
					return NULL;
			}

			if (has_extra_block) {
				unusedblk = heap_blk_new();
				if (!unusedblk) {
					if (skipblk)
						heap_blk_release(skipblk);
					return NULL;
				}
			}

			if (prevblk)
				prevblk->next = curblk->next;
			else
				tm_freelist[type] = curblk->next;
			if (curblk->next)
				curblk->next->prev = prevblk;
			tm_free[type] -= curblk->size;

			if (skip != 0) {
				skipblk->type = curblk->type;
				skipblk->base = curblk->base;
				skipblk->size = skip;

				curblk->base += skip;
				curblk->size -= skip;
			}

			if (has_extra_block) {
				unusedblk->type = curblk->type;
				unusedblk->base = curblk->base + size;
				unusedblk->size = remaining;
				curblk->size = size;
			}

			curblk->used = GL_TRUE;
			*(tm_block_t **)curblk->base = curblk;
			*(tm_block_t **)(curblk->base + curblk->size - 4) = curblk;
			curblk->prev = NULL;
			curblk->next = tm_alloclist[type];
			if (tm_alloclist[type])
				tm_alloclist[type]->prev = curblk;
			tm_alloclist[type] = curblk;

			if (skip != 0)
				heap_blk_insert_free(skipblk);

			if (has_extra_block)
				heap_blk_insert_free(unusedblk);

			return curblk;
		}

		prevblk = curblk;
		curblk = curblk->next;
	}

	return NULL;
}

// Frees a previously allocated block
static void heap_blk_free(uintptr_t base, vglMemType type) {
	tm_block_t **slot = (tm_block_t **)(base - 4);
	tm_block_t *block = *slot;
	
	tm_lock_mutex(&tm_mutexes[type])

#ifndef SKIP_ERROR_HANDLING
	if (!block || block->base + 4 != base) {
		tm_unlock_mutex(&tm_mutexes[type])
		vgl_log("%s:%d A heap overflow or double free was detected on pointer: 0x%08X!\n", __FILE__, __LINE__, base);
		return;
	}
#endif
	*slot = NULL;
	
	if (block->prev)
		block->prev->next = block->next;
	else
		tm_alloclist[type] = block->next;
	if (block->next)
		block->next->prev = block->prev;

	block->next = NULL;
	block->prev = NULL;

	heap_blk_insert_free(block);
	tm_unlock_mutex(&tm_mutexes[type])
}

// Initializes heap setup
static void heap_init(void) {
	for (int i = 0; i < VGL_MEM_ALL - 1; i++) {
		tm_alloclist[i] = NULL;
		tm_freelist[i] = NULL;
		tm_free[i] = 0;
	}
}

// Extends the heap setup with a new heap type
static void heap_extend(int32_t type, void *base, uint32_t size) {
	tm_max_failed_alloc[type] = size + 1;
	tm_block_t *block = heap_blk_new();
	block->next = NULL;
	block->type = type;
	block->base = (uintptr_t)base;
	block->size = size;
	heap_blk_insert_free(block);
}

// Allocates memory from the requested heap type
static void *heap_alloc(int32_t type, uint32_t size, uint32_t alignment) {
	tm_lock_mutex(&tm_mutexes[type])
	tm_block_t *block = heap_blk_alloc(type, size + MIN_FREE_BLOCK_SIZE, alignment);
	tm_unlock_mutex(&tm_mutexes[type])
	if (!block) {
		tm_max_failed_alloc[type] = size;
		return NULL;
	}

	return (void *)(block->base + 4);
}

// Attempt to extend an allocated block with nearby unused blocks
static GLboolean heap_blk_try_extend(vglMemType type, tm_block_t *block, uint32_t new_size) {
	uintptr_t target_base = block->base + block->size;
	if (target_base >= (uintptr_t)mempool_end[type])
		return GL_FALSE;

	tm_block_t *curblk = *(tm_block_t **)target_base;
	if (curblk->used == GL_TRUE)
		return GL_FALSE;

	uint32_t available = block->size + curblk->size;
	if (available < new_size)
		return GL_FALSE;

	uint32_t needed_from_free = new_size - block->size;
	if (curblk->prev)
		curblk->prev->next = curblk->next;
	else
		tm_freelist[type] = curblk->next;
	if (curblk->next)
		curblk->next->prev = curblk->prev;

	if (needed_from_free == curblk->size) {
		tm_free[type] -= curblk->size;
		heap_blk_release(curblk);
	} else {
		curblk->base += needed_from_free;
		curblk->size -= needed_from_free;
		tm_free[type] -= needed_from_free;
		*(tm_block_t **)curblk->base = curblk;
		curblk->prev = NULL;
		curblk->next = tm_freelist[type];
		if (tm_freelist[type])
			tm_freelist[type]->prev = curblk;
		tm_freelist[type] = curblk;
	}
	
	block->size = new_size;
	*(tm_block_t **)(block->base + block->size - 4) = block;

	return GL_TRUE;
}

// realloc equivalent for heap
static void *heap_realloc(vglMemType type, void *ptr, uint32_t size) {
	// Check for memory corruption
	uint32_t real_size = size + MIN_FREE_BLOCK_SIZE;
	tm_block_t **slot = (tm_block_t **)((uintptr_t)ptr - 4);
	tm_block_t *block = *slot;
#ifndef SKIP_ERROR_HANDLING
	if (!block || block->base + 4 != (uintptr_t)ptr) {
		vgl_log("%s:%d Corrupted block during realloc: 0x%08X!\n", __FILE__, __LINE__, (uintptr_t)ptr);
		return NULL;
	}
#endif

	// If the block size is big enough to contain the new size, just return the block itself
	if (real_size <= block->size) {
		return ptr;
	}

	// Attempt to merge nearby blocks to fullfill the request
	tm_lock_mutex(&tm_mutexes[type]);
	int extended = heap_blk_try_extend(type, block, real_size);
	tm_unlock_mutex(&tm_mutexes[type]);
	if (extended) {
		return ptr;
	}

	// Everything failed, so alloc a new block of the requested size and move data on it
	void *newptr = heap_alloc(type, size, MEM_ALIGNMENT);
	if (newptr) {
		vgl_fast_memcpy(newptr, ptr, block->size - 4);
		heap_blk_free((uintptr_t)ptr, type);
	}
	return newptr;
}
#endif

#ifdef PHYCONT_ON_DEMAND
void *vgl_alloc_phycont_block(uint32_t size) {
	size = VGL_ALIGN(size, 1024 * 1024);
	SceUID blk = sceKernelAllocMemBlock("phycont_blk", has_cached_mem ? SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW : SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, size, NULL);

	if (blk < 0)
		return NULL;

	void *res;
	sceKernelGetMemBlockBase(blk, &res);
	sceGxmMapMemory(res, size, SCE_GXM_MEMORY_ATTRIB_RW);

	return res;
}
#endif

void vgl_mem_init(size_t size_ram, size_t size_cdram, size_t size_phycont, size_t size_cdlg) {
	vgl_has_cdlg_support = GL_TRUE;

	if (!has_cached_mem && size_ram > 0xC800000) // Vita has a smaller address mapping for uncached mem
		size_ram = 0xC800000;

	mempool_size[VGL_MEM_VRAM] = VGL_ALIGN(size_cdram, 256 * 1024);
	mempool_size[VGL_MEM_RAM] = VGL_ALIGN(size_ram, 4 * 1024);
#ifdef PHYCONT_ON_DEMAND
	mempool_size[VGL_MEM_PHYCONT] = 0;
#else
	mempool_size[VGL_MEM_PHYCONT] = VGL_ALIGN(size_phycont, 1024 * 1024);
#endif
	mempool_size[VGL_MEM_BUDGET] = VGL_ALIGN(size_cdlg, 4 * 1024);

#ifdef HAVE_CUSTOM_HEAP
	// Initialize heap
#ifndef HAVE_SINGLE_THREADED_GC
	sceKernelCreateLwMutex(&header_mutex, "header mutex alloc", 0, 0, NULL);
#endif
	heap_init();
#endif

	SceUID mempool_id[VGL_MEM_ALL - 1] = {}; // UIDs of heap memblocks
	if (mempool_size[VGL_MEM_VRAM]) {
		mempool_id[VGL_MEM_VRAM] = sceKernelAllocMemBlock("cdram_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, mempool_size[VGL_MEM_VRAM], NULL);
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
		sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_VRAM], "cdram heap mutex", 0, 0, NULL);
#endif
	}
	if (has_cached_mem) {
		if (mempool_size[VGL_MEM_RAM]) {
			mempool_id[VGL_MEM_RAM] = sceKernelAllocMemBlock("ram_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, mempool_size[VGL_MEM_RAM], NULL);
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
			sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_RAM], "ram heap mutex", 0, 0, NULL);
#endif
		}
		if (mempool_size[VGL_MEM_PHYCONT]) {
			mempool_id[VGL_MEM_PHYCONT] = sceKernelAllocMemBlock("phycont_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, mempool_size[VGL_MEM_PHYCONT], NULL);
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
			sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_PHYCONT], "phycont heap mutex", 0, 0, NULL);
#endif
		}
		if (mempool_size[VGL_MEM_BUDGET]) {
			mempool_id[VGL_MEM_BUDGET] = sceKernelAllocMemBlock("cdlg_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_RW, mempool_size[VGL_MEM_BUDGET], NULL);
			vgl_has_cdlg_support = GL_FALSE;
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
			sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_BUDGET], "cdlg heap mutex", 0, 0, NULL);
#endif
		}
	} else {
		if (mempool_size[VGL_MEM_RAM]) {
			mempool_id[VGL_MEM_RAM] = sceKernelAllocMemBlock("ram_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, mempool_size[VGL_MEM_RAM], NULL);
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
			sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_RAM], "ram heap mutex", 0, 0, NULL);
#endif
		}
		if (mempool_size[VGL_MEM_PHYCONT]) {
			mempool_id[VGL_MEM_PHYCONT] = sceKernelAllocMemBlock("phycont_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, mempool_size[VGL_MEM_PHYCONT], NULL);
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
			sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_PHYCONT], "phycont heap mutex", 0, 0, NULL);
#endif
		}
		if (mempool_size[VGL_MEM_BUDGET]) {
			mempool_id[VGL_MEM_BUDGET] = sceKernelAllocMemBlock("cdlg_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_NC_RW, mempool_size[VGL_MEM_BUDGET], NULL);
			vgl_has_cdlg_support = GL_FALSE;
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
			sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_BUDGET], "cdlg heap mutex", 0, 0, NULL);
#endif
		}
	}
	for (int i = 0; i < VGL_MEM_EXTERNAL; i++) {
		if (mempool_size[i]) {
			mempool_addr[i] = NULL;
			sceKernelGetMemBlockBase(mempool_id[i], &mempool_addr[i]);

			if (mempool_addr[i]) {
				mempool_end[i] = (void *)((uintptr_t)mempool_addr[i] + mempool_size[i]);
				sceGxmMapMemory(mempool_addr[i], mempool_size[i], SCE_GXM_MEMORY_ATTRIB_RW);
#ifndef HAVE_CUSTOM_HEAP
				mempool_mspace[i] = sceClibMspaceCreate(mempool_addr[i], mempool_size[i]);
#else
				heap_extend(i, mempool_addr[i], mempool_size[i]);
#endif
			}
		}
	}

#ifdef PHYCONT_ON_DEMAND
	// Getting total available phycont mem
	uint32_t phycont_size;
	if (system_app_mode) {
		SceAppMgrBudgetInfo info;
		info.size = sizeof(SceAppMgrBudgetInfo);
		sceAppMgrGetBudgetInfo(&info);
		phycont_size = info.total_phycont_mem;
	} else {
		SceKernelFreeMemorySizeInfo info;
		info.size = sizeof(SceKernelFreeMemorySizeInfo);
		sceKernelGetFreeMemorySize(&info);
		phycont_size = info.size_phycont;
	}
	mempool_size[VGL_MEM_PHYCONT] = phycont_size;
#endif

	// Mapping newlib heap into sceGxm
#ifdef HAVE_WRAPPED_ALLOCATORS
	void *dummy = __real_malloc(1);
	__real_free(dummy);
#else
	void *dummy = malloc(1);
	free(dummy);
#endif
	

	SceKernelMemBlockInfo info;
	info.size = sizeof(SceKernelMemBlockInfo);
	sceKernelGetMemBlockInfoByAddr(dummy, &info);
	sceGxmMapMemory(info.mappedBase, info.mappedSize, SCE_GXM_MEMORY_ATTRIB_RW);

	mempool_size[VGL_MEM_EXTERNAL] = info.mappedSize;
	mempool_addr[VGL_MEM_EXTERNAL] = info.mappedBase;
	mempool_end[VGL_MEM_EXTERNAL] = (void *)((uintptr_t)mempool_addr[VGL_MEM_EXTERNAL] + mempool_size[VGL_MEM_EXTERNAL]);
}

#ifndef PHYCONT_ON_DEMAND
void vgl_mem_provide_phycont(size_t size_phycont) {
	if (mempool_size[VGL_MEM_PHYCONT])
		return;

	SceUID mempool_id;
	mempool_size[VGL_MEM_PHYCONT] = VGL_ALIGN(size_phycont, 1024 * 1024);
	if (has_cached_mem) {
		if (mempool_size[VGL_MEM_PHYCONT]) {
			mempool_id = sceKernelAllocMemBlock("phycont_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, mempool_size[VGL_MEM_PHYCONT], NULL);
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
			sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_PHYCONT], "phycont heap mutex", 0, 0, NULL);
#endif
		}
	} else {
		if (mempool_size[VGL_MEM_PHYCONT]) {
			mempool_id = sceKernelAllocMemBlock("phycont_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, mempool_size[VGL_MEM_PHYCONT], NULL);
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
			sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_PHYCONT], "phycont heap mutex", 0, 0, NULL);
#endif
		}
	}
	
	mempool_addr[VGL_MEM_PHYCONT] = NULL;
	sceKernelGetMemBlockBase(mempool_id, &mempool_addr[VGL_MEM_PHYCONT]);
	if (mempool_addr[VGL_MEM_PHYCONT]) {
		mempool_end[VGL_MEM_PHYCONT] = (void *)((uintptr_t)mempool_addr[VGL_MEM_PHYCONT] + mempool_size[VGL_MEM_PHYCONT]);
		sceGxmMapMemory(mempool_addr[VGL_MEM_PHYCONT], mempool_size[VGL_MEM_PHYCONT], SCE_GXM_MEMORY_ATTRIB_RW);
#ifndef HAVE_CUSTOM_HEAP
		mempool_mspace[VGL_MEM_PHYCONT] = sceClibMspaceCreate(mempool_addr[VGL_MEM_PHYCONT], mempool_size[VGL_MEM_PHYCONT]);
#else
		heap_extend(VGL_MEM_PHYCONT, mempool_addr[VGL_MEM_PHYCONT], mempool_size[VGL_MEM_PHYCONT]);
#endif
	}
}
#endif

vglMemType vgl_mem_get_type_by_addr(void *addr) {
	if (addr >= mempool_addr[VGL_MEM_VRAM] && addr < mempool_end[VGL_MEM_VRAM])
		return VGL_MEM_VRAM;
	else if (addr >= mempool_addr[VGL_MEM_RAM] && addr < mempool_end[VGL_MEM_RAM])
		return VGL_MEM_RAM;
#ifndef PHYCONT_ON_DEMAND
	else if (addr >= mempool_addr[VGL_MEM_PHYCONT] && addr < mempool_end[VGL_MEM_PHYCONT])
		return VGL_MEM_PHYCONT;
#endif
	else if (addr >= mempool_addr[VGL_MEM_BUDGET] && addr < mempool_end[VGL_MEM_BUDGET])
		return VGL_MEM_BUDGET;
	else if (addr >= mempool_addr[VGL_MEM_EXTERNAL] && addr < mempool_end[VGL_MEM_EXTERNAL])
		return VGL_MEM_EXTERNAL;
#ifdef PHYCONT_ON_DEMAND
	return VGL_MEM_PHYCONT;
#else
	return -1;
#endif
}

size_t vgl_mem_get_free_space(vglMemType type) {
	if (type == VGL_MEM_EXTERNAL) {
		return 0;
#ifdef PHYCONT_ON_DEMAND
	} else if (type == VGL_MEM_PHYCONT) {
		if (system_app_mode) {
			SceAppMgrBudgetInfo info;
			info.size = sizeof(SceAppMgrBudgetInfo);
			sceAppMgrGetBudgetInfo(&info);
			return info.free_phycont_mem;
		} else {
			SceKernelFreeMemorySizeInfo info;
			info.size = sizeof(SceKernelFreeMemorySizeInfo);
			sceKernelGetFreeMemorySize(&info);
			return info.size_phycont;
		}
#endif
	} else if (type == VGL_MEM_ALL) {
		size_t size = 0;
		for (int i = 0; i < VGL_MEM_EXTERNAL; i++) {
			size += vgl_mem_get_free_space(i);
		}
		return size;
#ifdef HAVE_CUSTOM_HEAP
	} else {
		return tm_free[type];
	}
#else
	} else if (mempool_size[type]) {
		SceClibMspaceStats stats;
		sceClibMspaceMallocStats(mempool_mspace[type], &stats);
		return stats.capacity - stats.current_in_use;
	} else
		return 0;
#endif
}

size_t vgl_mem_get_total_space(vglMemType type) {
	if (type == VGL_MEM_ALL) {
		size_t size = 0;
		for (int i = 0; i < VGL_MEM_EXTERNAL; i++) {
			size += vgl_mem_get_total_space(i);
		}
		return size;
	} else {
		return mempool_size[type];
	}
}

size_t vgl_malloc_usable_size(void *ptr) {
	vglMemType type = vgl_mem_get_type_by_addr(ptr);
	if (type == VGL_MEM_EXTERNAL)
		return malloc_usable_size(ptr);
#ifdef PHYCONT_ON_DEMAND
	else if (type == VGL_MEM_PHYCONT) {
		SceKernelMemBlockInfo info;
		info.size = sizeof(SceKernelMemBlockInfo);
		sceKernelGetMemBlockInfoByAddr(ptr, &info);
		return info.mappedSize;
	}
#endif
#ifdef HAVE_CUSTOM_HEAP
	tm_block_t **hdr = (tm_block_t **)((uintptr_t)ptr - 4);
	tm_block_t *block = *hdr;
	return block->size - MIN_FREE_BLOCK_SIZE;
#else
	return sceClibMspaceMallocUsableSize(ptr);
#endif
}

// Bytes vitaGL holds in the newlib heap (its own objects, and GPU data that
// spilled out of its mempools), reported through vglMemExternalInUse
static size_t external_in_use = 0;

static inline void *external_track(void *ptr) {
	if (ptr)
		__atomic_add_fetch(&external_in_use, malloc_usable_size(ptr), __ATOMIC_RELAXED);
	return ptr;
}

static inline void external_untrack(void *ptr) {
	__atomic_sub_fetch(&external_in_use, malloc_usable_size(ptr), __ATOMIC_RELAXED);
}

size_t vglMemExternalInUse(void) {
	return __atomic_load_n(&external_in_use, __ATOMIC_RELAXED);
}

void VGL_RAW(vgl_free)(void *ptr) {
	if (!ptr)
		return;

	vglMemType type = vgl_mem_get_type_by_addr(ptr);
	if (type == VGL_MEM_EXTERNAL) {
		external_untrack(ptr);
#ifdef HAVE_WRAPPED_ALLOCATORS
		__real_free(ptr);
#else
		free(ptr);
#endif
	}
#ifdef PHYCONT_ON_DEMAND
	else if (type == VGL_MEM_PHYCONT) {
		sceGxmUnmapMemory(ptr);
		sceKernelFreeMemBlock(sceKernelFindMemBlockByAddr(ptr, 0));
	}
#endif
#ifdef HAVE_CUSTOM_HEAP
	else {
		heap_blk_free((uintptr_t)ptr, type);
		tm_max_failed_alloc[type] = tm_free[type] + 1;
	}
#else
	else if (mempool_mspace[type])
		sceClibMspaceFree(mempool_mspace[type], ptr);
#endif
}

void *VGL_RAW(vgl_malloc)(size_t size, vglMemType type) {
	if (type == VGL_MEM_EXTERNAL)
#ifdef HAVE_WRAPPED_ALLOCATORS
		return external_track(__real_malloc(size));
#else
		return external_track(malloc(size));
#endif
#ifdef PHYCONT_ON_DEMAND
	else if (type == VGL_MEM_PHYCONT)
		return vgl_alloc_phycont_block(size);
#endif
#ifdef HAVE_CUSTOM_HEAP
	else if (size <= tm_max_failed_alloc[type])
		return heap_alloc(type, size, MEM_ALIGNMENT);
#else
	else if (mempool_mspace[type])
		return sceClibMspaceMalloc(mempool_mspace[type], size);
#endif
	return NULL;
}

void *VGL_RAW(vgl_calloc)(size_t num, size_t size, vglMemType type) {
	if (type == VGL_MEM_EXTERNAL)
#ifdef HAVE_WRAPPED_ALLOCATORS
		return external_track(__real_calloc(num, size));
#else
		return external_track(calloc(num, size));
#endif
#ifdef PHYCONT_ON_DEMAND
	else if (type == VGL_MEM_PHYCONT) {
		void *ret = vgl_alloc_phycont_block(num * size);
		if (ret) {
			sceClibMemset(ret, 0, num * size);
		}
		return ret;
	}
#endif
#ifdef HAVE_CUSTOM_HEAP
	else if (num * size <= tm_max_failed_alloc[type]) {
		void *ret = heap_alloc(type, num * size, MEM_ALIGNMENT);
		if (ret) {
			sceClibMemset(ret, 0, num * size);
		}
		return ret;
	}
#else
	else if (mempool_mspace[type])
		return sceClibMspaceCalloc(mempool_mspace[type], num, size);
#endif
	return NULL;
}

void *VGL_RAW(vgl_memalign)(size_t alignment, size_t size, vglMemType type) {
	if (type == VGL_MEM_EXTERNAL)
#ifdef HAVE_WRAPPED_ALLOCATORS
		return external_track(__real_memalign(alignment, size));
#else
		return external_track(memalign(alignment, size));
#endif
#ifdef PHYCONT_ON_DEMAND
	else if (type == VGL_MEM_PHYCONT)
		return vgl_alloc_phycont_block(size);
#endif
#ifdef HAVE_CUSTOM_HEAP
	else if (size <= tm_max_failed_alloc[type])
		return heap_alloc(type, size, alignment);
#else
	else if (mempool_mspace[type])
		return sceClibMspaceMemalign(mempool_mspace[type], alignment, size);
#endif
	return NULL;
}

void *VGL_RAW(vgl_realloc)(void *ptr, size_t size) {
	if (!ptr) {
#ifdef HAVE_WRAPPED_ALLOCATORS
		return external_track(__real_malloc(size));
#else
		return external_track(malloc(size));
#endif
	}
	vglMemType type = vgl_mem_get_type_by_addr(ptr);
	if (type == VGL_MEM_EXTERNAL) {
		size_t old_size = malloc_usable_size(ptr);
#ifdef HAVE_WRAPPED_ALLOCATORS
		void *res = __real_realloc(ptr, size);
#else
		void *res = realloc(ptr, size);
#endif
		if (res) {
			__atomic_sub_fetch(&external_in_use, old_size, __ATOMIC_RELAXED);
			external_track(res);
		}
		return res;
	}
#ifdef PHYCONT_ON_DEMAND
	if (type == VGL_MEM_PHYCONT) {
		size_t old_size = vgl_malloc_usable_size(ptr);
		if (old_size >= size) {
			return ptr;
		}
		void *res = vgl_alloc_phycont_block(size);
		if (res) {
			vgl_fast_memcpy(res, ptr, old_size);
			vgl_free(ptr);
			return res;
		}
	}
#endif
#ifndef HAVE_CUSTOM_HEAP
	return sceClibMspaceRealloc(mempool_mspace[type], ptr, size);
#else
	return heap_realloc(type, ptr, size);
#endif
}


#ifdef VGL_HEAP_CANARIES
/*
 * Heap overflow detector (HL2 Vita debugging). Every allocation from vitaGL's
 * own heaps gets a guard pattern past its end and is recorded; after each
 * upload-type GL call, vgl_heap_canary_check() verifies all guards and logs
 * the first one found broken, with the call it was detected after.
 */
#define CANARY_SIZE 16
#define CANARY_BYTE 0xC5
#define CANARY_SLOTS 32768 // power of two

typedef struct {
	uint8_t *ptr;
	uint32_t size;
} canary_entry;

static canary_entry canary_table[CANARY_SLOTS];
static uint32_t canary_count = 0;
static GLboolean canary_reported = GL_FALSE;
static SceKernelLwMutexWork canary_mutex;
static GLboolean canary_mutex_ready = GL_FALSE;
static GLboolean canary_table_full = GL_FALSE; // untracked blocks exist, so bad-free reports would be unreliable
static uint32_t bad_free_reports = 0;

// launcher_vita/so_loader.cpp, when linked into the HL2 Vita launcher
extern void so_log_backtrace(const char *tag) __attribute__((weak));

static void canary_lock(void) {
	if (!canary_mutex_ready) {
		sceKernelCreateLwMutex(&canary_mutex, "vgl_canary", 2, 0, NULL);
		canary_mutex_ready = GL_TRUE;
	}
	sceKernelLockLwMutex(&canary_mutex, 1, NULL);
}

static void canary_unlock(void) {
	sceKernelUnlockLwMutex(&canary_mutex, 1);
}

static inline uint32_t canary_hash(void *ptr) {
	return (((uintptr_t)ptr >> 3) * 2654435761u) & (CANARY_SLOTS - 1);
}

static GLboolean canary_intact(canary_entry *e) {
	for (int i = 0; i < CANARY_SIZE; i++) {
		if (e->ptr[e->size + i] != CANARY_BYTE)
			return GL_FALSE;
	}
	return GL_TRUE;
}

static void canary_report(canary_entry *e, const char *where) {
	if (canary_reported)
		return;
	canary_reported = GL_TRUE;
	int first = 0;
	while (first < CANARY_SIZE && e->ptr[e->size + first] == CANARY_BYTE)
		first++;
	vgl_log("HEAP OVERFLOW detected after %s: block %p of %u bytes (type %d) overwritten at +%u: %02X %02X %02X %02X\n",
		where, e->ptr, e->size, vgl_mem_get_type_by_addr(e->ptr), e->size + first,
		e->ptr[e->size], e->ptr[e->size + 1], e->ptr[e->size + 2], e->ptr[e->size + 3]);
}

static void canary_add(void *ptr, size_t size) {
	if (!ptr)
		return;
	sceClibMemset((uint8_t *)ptr + size, CANARY_BYTE, CANARY_SIZE);
	canary_lock();
	if (canary_count >= CANARY_SLOTS - 1) {
		canary_table_full = GL_TRUE;
	} else {
		uint32_t i = canary_hash(ptr);
		while (canary_table[i].ptr)
			i = (i + 1) & (CANARY_SLOTS - 1);
		canary_table[i].ptr = (uint8_t *)ptr;
		canary_table[i].size = size;
		canary_count++;
	}
	canary_unlock();
}

// Removes the entry for ptr, checking its guard first; returns GL_FALSE if
// ptr isn't a live block (double free, or not the start of a block)
static GLboolean canary_remove(void *ptr, const char *where) {
	GLboolean found = GL_FALSE;
	canary_lock();
	uint32_t i = canary_hash(ptr);
	while (canary_table[i].ptr) {
		if (canary_table[i].ptr == ptr) {
			if (!canary_intact(&canary_table[i]))
				canary_report(&canary_table[i], where);
			// backward-shift deletion keeps linear probing chains intact
			uint32_t j = i;
			for (;;) {
				j = (j + 1) & (CANARY_SLOTS - 1);
				if (!canary_table[j].ptr)
					break;
				uint32_t k = canary_hash(canary_table[j].ptr);
				if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) {
					canary_table[i] = canary_table[j];
					i = j;
				}
			}
			canary_table[i].ptr = NULL;
			canary_count--;
			found = GL_TRUE;
			break;
		}
		i = (i + 1) & (CANARY_SLOTS - 1);
	}
	canary_unlock();
	return found || canary_table_full;
}

static void bad_free_report(void *ptr, const char *where) {
	if (bad_free_reports >= 16)
		return;
	bad_free_reports++;
	vgl_log("BAD FREE in %s: %p (type %d) is not a live block (double free or interior pointer); not freed\n",
		where, ptr, vgl_mem_get_type_by_addr(ptr));
	if (so_log_backtrace)
		so_log_backtrace("vitaGL bad free");
}

// Heap statistics that no longer add up mean the heap's metadata is corrupt;
// returns the corrupt pool, or -1
static int mspace_corrupt_pool(void) {
#ifndef HAVE_CUSTOM_HEAP
	for (int i = 0; i < VGL_MEM_EXTERNAL; i++) {
		if (!mempool_mspace[i])
			continue;
		SceClibMspaceStats stats;
		sceClibMspaceMallocStats(mempool_mspace[i], &stats);
		if (stats.current_in_use > stats.capacity || stats.capacity > mempool_size[i])
			return i;
	}
#endif
	return -1;
}

static void mspace_sanity_check(const char *where) {
	static GLboolean reported = GL_FALSE;
	if (reported)
		return;
	int pool = mspace_corrupt_pool();
	if (pool >= 0) {
		reported = GL_TRUE;
		vgl_log("HEAP CORRUPT detected after %s: pool %d\n", where, pool);
	}
}

// Brackets the GL call that corrupts a heap: every traced call checks the
// heap, the last clean check is remembered, and the first corrupt one is
// logged together with it
typedef struct {
	const char *where;
	int after;
	int args[6];
} heap_trace_point;

static heap_trace_point heap_last_clean;

static void heap_trace_format(char *buf, size_t len, const heap_trace_point *p) {
	if (!p->where) {
		snprintf(buf, len, "(none)");
		return;
	}
	snprintf(buf, len, "%s %s(%d, %d, %d, %d, %d, %d)", p->after ? "after" : "before", p->where,
		p->args[0], p->args[1], p->args[2], p->args[3], p->args[4], p->args[5]);
}

void vgl_heap_trace(const char *where, int after, int a, int b, int c, int d, int e, int f) {
	static GLboolean reported = GL_FALSE;
	if (reported)
		return;
	heap_trace_point p = {where, after, {a, b, c, d, e, f}};
	int pool = mspace_corrupt_pool();
	if (pool < 0) {
		heap_last_clean = p;
		return;
	}
	reported = GL_TRUE;
	char now[192], last[192];
	heap_trace_format(now, sizeof(now), &p);
	heap_trace_format(last, sizeof(last), &heap_last_clean);
	vgl_log("HEAP CORRUPT (pool %d) first seen %s; last clean check was %s\n", pool, now, last);
	if (so_log_backtrace)
		so_log_backtrace("vitaGL heap corrupt");
}

void vgl_heap_canary_check(const char *where) {
	// A full scan after every upload made level loads very slow; scan on
	// buffer swaps and every 64th upload (reports may then lag the culprit)
	static uint32_t calls = 0;
	if (strcmp(where, "vglSwapBuffers") != 0 && (++calls & 63) != 0)
		return;
	mspace_sanity_check(where);
	if (canary_reported)
		return;
	canary_lock();
	for (uint32_t i = 0; i < CANARY_SLOTS; i++) {
		if (canary_table[i].ptr && !canary_intact(&canary_table[i])) {
			canary_report(&canary_table[i], where);
			break;
		}
	}
	canary_unlock();
}

static inline GLboolean canary_type(vglMemType type) {
	return type != VGL_MEM_EXTERNAL
#ifdef PHYCONT_ON_DEMAND
		&& type != VGL_MEM_PHYCONT
#endif
		;
}

void vgl_free(void *ptr) {
	if (ptr && canary_type(vgl_mem_get_type_by_addr(ptr)) && !canary_remove(ptr, "free")) {
		// freeing it again would corrupt the heap
		bad_free_report(ptr, "vgl_free");
		return;
	}
	vgl_free_raw(ptr);
}

void *vgl_malloc(size_t size, vglMemType type) {
	if (!canary_type(type))
		return vgl_malloc_raw(size, type);
	void *res = vgl_malloc_raw(size + CANARY_SIZE, type);
	canary_add(res, size);
	return res;
}

void *vgl_calloc(size_t num, size_t size, vglMemType type) {
	if (!canary_type(type))
		return vgl_calloc_raw(num, size, type);
	void *res = vgl_calloc_raw(1, num * size + CANARY_SIZE, type);
	canary_add(res, num * size);
	return res;
}

void *vgl_memalign(size_t alignment, size_t size, vglMemType type) {
	if (!canary_type(type))
		return vgl_memalign_raw(alignment, size, type);
	void *res = vgl_memalign_raw(alignment, size + CANARY_SIZE, type);
	canary_add(res, size);
	return res;
}

void *vgl_realloc(void *ptr, size_t size) {
	if (!ptr || !canary_type(vgl_mem_get_type_by_addr(ptr)))
		return vgl_realloc_raw(ptr, size);
	if (!canary_remove(ptr, "realloc"))
		bad_free_report(ptr, "vgl_realloc");
	void *res = vgl_realloc_raw(ptr, size + CANARY_SIZE);
	canary_add(res, size);
	return res;
}
#endif
