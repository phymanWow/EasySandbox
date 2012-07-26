// EasySandbox - sandboxing for untrusted code using Linux/seccomp
// Copyright (c) 2012, David H. Hovemeyer <david.hovemeyer@gmail.com>
// 
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.

#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/mman.h>

#define DEFAULT_HEAP_SIZE (1024*1024)

// Error exit codes
#define SECCOMP_ERROR     17
#define MMAP_FAILED_ERROR 18

//----------------------------------------------------------------
// Statically-allocated memory manager
//
// by Eli Bendersky (eliben@gmail.com)
//  
// This code is in the public domain.
//
// Adapted for EasySandbox by David Hovemeyer. See:
//    http://eli.thegreenplace.net/2008/10/17/memmgr-a-fixed-pool-memory-allocator/
//----------------------------------------------------------------
#define MIN_POOL_ALLOC_QUANTAS 16
typedef unsigned char byte;
typedef unsigned long ulong;

static void memmgr_init(byte *pool_, unsigned long heap_size);
static void* memmgr_alloc(ulong nbytes);
static void memmgr_free(void* ap);
static ulong memmgr_get_block_size(void *ap);

// "realmain" is the main function of the untrusted program.
// Even though it is probably defined as "main" in the source code,
// we'll use objcopy to rename it to ensure that our
// main() function is called first.
extern int realmain(int argc, char **argv, char **envp);

////////////////////////////////////////////////////////////////////////
// Wrapper for main
//
// Creates the malloc heap, enables seccomp, and then starts
// execution of the untrusted main function.
////////////////////////////////////////////////////////////////////////

int main(int argc, char **argv, char **envp)
{
	// FIXME: make this configurable
	size_t heapsize = DEFAULT_HEAP_SIZE;

	// Initialize the malloc heap.
	void *heap = mmap(0, (size_t)heapsize, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, -1, 0);
	if (heap == MAP_FAILED) {
		// Couldn't allocate heap memory.
		_exit(MMAP_FAILED_ERROR);
	}
	memmgr_init(heap, heapsize);

	// Now we can enter SECCOMP mode.
	if (prctl(PR_SET_SECCOMP, 1, 0, 0) < 0) {
		_exit(SECCOMP_ERROR);
	}

	return realmain(argc, argv, envp);
}

////////////////////////////////////////////////////////////////////////
// Memory allocation functions
//
// We use these in preference to the ones defined in klibc so that
// all allocations are satisfied out of the preallocated heap.
////////////////////////////////////////////////////////////////////////

void *malloc(size_t size)
{
	return memmgr_alloc((ulong) size);
}

void free(void *ptr)
{
	memmgr_free(ptr);
}

void *calloc(size_t nmemb, size_t size)
{
	unsigned char *buf = malloc(nmemb * size);
	if (buf != 0) {
		unsigned char *p;
		for (p = buf; p < buf + (nmemb * size); p++) {
			*p = (unsigned char) '\0';
		}
	}
	return buf;
}

void *realloc(void *ptr, size_t size)
{
	void *buf;
	unsigned char *dst;
	unsigned char *src;
	size_t alloc_size, to_copy, i;

	// Allocate new buffer
	buf = malloc(size);

	if (buf != 0) {
		// Find original allocation size
		alloc_size = (size_t) memmgr_get_block_size(ptr);
		to_copy = alloc_size;
		if (to_copy > size) {
			to_copy = size;
		}

		// Copy data to new buffer
		dst = buf;
		src = ptr;
		for (i = 0; i < to_copy; i++) {
			*dst++ = *src++;
		}

		// Free the old buffer
		free(ptr);
	}

	return buf;
}

////////////////////////////////////////////////////////////////////////
// memmgr implementation
////////////////////////////////////////////////////////////////////////

typedef ulong Align;

union mem_header_union
{
    struct 
    {
        // Pointer to the next block in the free list
        //
        union mem_header_union* next;

        // Size of the block (in quantas of sizeof(mem_header_t))
        //
        ulong size; 
    } s;

    // Used to align headers in memory to a boundary
    //
    Align align_dummy;
};

typedef union mem_header_union mem_header_t;

// Initial empty list
//
static mem_header_t base;

// Start of free list
//
static mem_header_t* freep = 0;

// Static pool for new allocations
//
//static byte pool[POOL_SIZE] = {0};

// DHH: changed so that memmgr_init takes the pool buffer and size
// as parameters.  They will be assigned to these static variables.
static byte *pool;
static unsigned long POOL_SIZE;

static ulong pool_free_pos = 0;

static void memmgr_init(byte *pool_, unsigned long pool_size)
{
    // DHH: heap memory buffer and size are passed as parameters
    pool = pool_;
    POOL_SIZE = pool_size;

    base.s.next = 0;
    base.s.size = 0;
    freep = 0;
    pool_free_pos = 0;
}

static mem_header_t* get_mem_from_pool(ulong nquantas)
{
    ulong total_req_size;

    mem_header_t* h;

    if (nquantas < MIN_POOL_ALLOC_QUANTAS)
        nquantas = MIN_POOL_ALLOC_QUANTAS;

    total_req_size = nquantas * sizeof(mem_header_t);

    if (pool_free_pos + total_req_size <= POOL_SIZE)
    {
        h = (mem_header_t*) (pool + pool_free_pos);
        h->s.size = nquantas;
        memmgr_free((void*) (h + 1));
        pool_free_pos += total_req_size;
    }
    else
    {
        return 0;
    }

    return freep;
}

// Allocations are done in 'quantas' of header size.
// The search for a free block of adequate size begins at the point 'freep' 
// where the last block was found.
// If a too-big block is found, it is split and the tail is returned (this 
// way the header of the original needs only to have its size adjusted).
// The pointer returned to the user points to the free space within the block,
// which begins one quanta after the header.
//
static void* memmgr_alloc(ulong nbytes)
{
    mem_header_t* p;
    mem_header_t* prevp;

    // Calculate how many quantas are required: we need enough to house all
    // the requested bytes, plus the header. The -1 and +1 are there to make sure
    // that if nbytes is a multiple of nquantas, we don't allocate too much
    //
    ulong nquantas = (nbytes + sizeof(mem_header_t) - 1) / sizeof(mem_header_t) + 1;

    // First alloc call, and no free list yet ? Use 'base' for an initial
    // denegerate block of size 0, which points to itself
    // 
    if ((prevp = freep) == 0)
    {
        base.s.next = freep = prevp = &base;
        base.s.size = 0;
    }

    for (p = prevp->s.next; ; prevp = p, p = p->s.next)
    {
        // big enough ?
        if (p->s.size >= nquantas) 
        {
            // exactly ?
            if (p->s.size == nquantas)
            {
                // just eliminate this block from the free list by pointing
                // its prev's next to its next
                //
                prevp->s.next = p->s.next;
            }
            else // too big
            {
                p->s.size -= nquantas;
                p += p->s.size;
                p->s.size = nquantas;
            }

            freep = prevp;
            return (void*) (p + 1);
        }
        // Reached end of free list ?
        // Try to allocate the block from the pool. If that succeeds,
        // get_mem_from_pool adds the new block to the free list and
        // it will be found in the following iterations. If the call
        // to get_mem_from_pool doesn't succeed, we've run out of
        // memory
        //
        else if (p == freep)
        {
            if ((p = get_mem_from_pool(nquantas)) == 0)
            {
                #ifdef DEBUG_MEMMGR_FATAL
                printf("!! Memory allocation failed !!\n");
                #endif
                return 0;
            }
        }
    }
}

// Scans the free list, starting at freep, looking the the place to insert the 
// free block. This is either between two existing blocks or at the end of the
// list. In any case, if the block being freed is adjacent to either neighbor,
// the adjacent blocks are combined.
//
static void memmgr_free(void* ap)
{
    mem_header_t* block;
    mem_header_t* p;

    // acquire pointer to block header
    block = ((mem_header_t*) ap) - 1;

    // Find the correct place to place the block in (the free list is sorted by
    // address, increasing order)
    //
    for (p = freep; !(block > p && block < p->s.next); p = p->s.next)
    {
        // Since the free list is circular, there is one link where a 
        // higher-addressed block points to a lower-addressed block. 
        // This condition checks if the block should be actually 
        // inserted between them
        //
        if (p >= p->s.next && (block > p || block < p->s.next))
            break;
    }

    // Try to combine with the higher neighbor
    //
    if (block + block->s.size == p->s.next)
    {
        block->s.size += p->s.next->s.size;
        block->s.next = p->s.next->s.next;
    }
    else
    {
        block->s.next = p->s.next;
    }

    // Try to combine with the lower neighbor
    //
    if (p + p->s.size == block)
    {
        p->s.size += block->s.size;
        p->s.next = block->s.next;
    }
    else
    {
        p->s.next = block;
    }

    freep = p;
}

// Find out the allocation size of given block.
// Needed to implement realloc() and similar functions.
static ulong memmgr_get_block_size(void *ap)
{
    mem_header_t* block = ((mem_header_t *)ap) - 1;
    return block->s.size;
}
