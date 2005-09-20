#include <rtems.h>
#include <libcpu/pte121.h>
#include <libcpu/page.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <rtems/bspIo.h>

/* Configuration Parameters */

#define DFLT_FENCE	1
						/* whether to locate the end (1) or the beginning (-1) of 
						 * the allocated area on a page boundary, i.e., catch overwriting
						 * the end or the beginning, respectively.
						 * May also be 0 to indicate that protection is off, i.e., the
						 * facility installs pointers to the original allocators.
						 */

#undef	LOADABLE_TEST	/* create weak aliases that map __real_malloc to malloc etc.
						 * this *must not* be used for a static link and is intended
						 * for testing only (loadable module).
						 */

#define	END_FENCED_MALLOC_ALIGNMENT	1
						/* how to align memory returned by 'malloc'. Normal allocation
						 * guarantees that the returned address is aligned at least
						 * to CPU_HEAP_ALIGNMENT (8-bytes for 604+ PPC). However,
						 * as alignment is increased, violations may be lost...
						 */

#undef  CREATE_NEW_MAPPING
						/* This option, if enabled, creates new mappings for legally
						 * allocated blocks in a new address space beyond the physical
						 * 1:1 mapping. The advantage being that any access to non-allocated
						 * regions of the heap will be caught.
						 * OTOH, this doesn't work without recompiling and fixing all
						 * drivers that do backplane access to memory since the usual
						 * CPU<->physical mapping as defined by the BSP is not valid
						 * anymore.
						 * FIXME: The 'new mapping' algorithm uses triv121PgTblMap()
						 *        which should be mutex-protected (otherwise, concurrent
						 * 		  mallocs are likely to mess up the page table!!
						 *
						 *        OTOH, the 'non-new mapping' method shouldn't require
						 *		  mutex protection as only the affected PTE is modified
						 *		  (critical section when PTE is modified is ISR protected
						 *		  within the pte121).
						 */

#define PAGE_ALIGN_DOWN(x) ((unsigned32)(x) & PAGE_MASK)
#define PAGE_ALIGN_UP(x)   PAGE_ALIGN_DOWN((unsigned32)(x) + PAGE_SIZE - 1)

#define SEG_SIZE	0x10000000
#define PROTECTED_VSID	0x10	/* must be outside of the physical range (4 lsb of VSID)
								 * so the rpn/ea mapping still is 1:1
								 */

static unsigned long ea_offset = 0;

#define EA(x)	((unsigned32)(x)+ea_offset)
#define PA(x)	((unsigned32)(x)-ea_offset)

#ifdef LOADABLE_TEST
#define WKALIAS(sym)	__attribute__((weak,alias(#sym)))
#define STATIC
#else
#define STATIC			static
#define WKALIAS(sym)	
#endif


/* The real functions we need */
extern	void *__real_malloc(size_t)
	WKALIAS(malloc)
;
extern	void *__real_calloc(size_t, size_t)
	WKALIAS(calloc)
;
extern	void *__real_realloc(void *, size_t)
	WKALIAS(realloc)
;
extern	void  __real_free(void *)
	WKALIAS(free)
;

/* Information about the malloced area we need to keep;
 * we put this into the protected page and just have to
 * make sure we only access it when protection is off.
 */
typedef struct {
	char		*malloc_addr;
	size_t		malloc_size;
} FenceDataRec, *FenceData;


/* Cached page table pointer */
static Triv121PgTbl	pgtbl;

STATIC void *
end_fenced_malloc(size_t s);
STATIC void *
beg_fenced_malloc(size_t s);
STATIC FenceData
end_fenced_unmap(void *ea);
STATIC FenceData
beg_fenced_unmap(void *ea);
STATIC void
fenced_free(void *ea);
STATIC void *
fenced_realloc(void *ea, size_t s);

STATIC void *
fenced_init(size_t s);

unsigned efence_type = DFLT_FENCE;

static void * (* volatile malloc_proc)(size_t)			= fenced_init;
static void * (* volatile realloc_proc)(void *, size_t)	= fenced_realloc;
static void   (* volatile free_proc)(void *)			= fenced_free;
static FenceData (* volatile unmap_proc)(void*)			= end_fenced_unmap;

/* called only one time */
STATIC void *
fenced_init(size_t s)
{
	if ( efence_type > 0 ) {
		malloc_proc = end_fenced_malloc;
		unmap_proc  = end_fenced_unmap;
	} else if ( efence_type < 0 ) {
		malloc_proc = beg_fenced_malloc;
		unmap_proc  = beg_fenced_unmap;
	} else {
		/* use unfenced version */
		malloc_proc		= __real_malloc;
		realloc_proc	= __real_realloc;
		free_proc		= __real_free;
	}
	if ( efence_type ) {
#ifdef CREATE_NEW_MAPPING
		unsigned long			seg;
		extern unsigned long	BSP_mem_size;
#endif

		printk("$Id$\n");
		printk("** WARNING: Heap Protection engaged.\n");
		if ( efence_type > 0 )
		printk("**          Access beyond the end of malloced areas will be trapped\n");
		else
		printk("**          Access preceding the start of malloced areas will be trapped\n");
		printk("** This is a diagnostic tool that seriously degrades heap performance\n");
		printk("** USE FOR DEBUGGING ONLY\n");

		printk("Making page table writable...\n");
		pgtbl = triv121PgTblGet();
		triv121MakePgTblRW();

#ifdef CREATE_NEW_MAPPING
		printk("Setting up segment registers...\n");
		/* calculate the segment offset */
		for ( ea_offset = 0; ea_offset < BSP_mem_size; ea_offset += SEG_SIZE )
			/* nothing else to do */;
		for ( seg = 0; seg < ea_offset; seg+=SEG_SIZE ) {
			unsigned long vsid;
			asm volatile("mfsrin %0, %1":"=r"(vsid):"r"(seg+ea_offset));
			vsid = (vsid & ~((1<<24)-1)) | ((PROTECTED_VSID + (seg>>28)) & ((1<<24)-1));
			asm volatile(
				"	isync 			\n\t"
				"	mtsrin %0, %1	\n\t"
				"	isync			\n\t"
				:
				:"r"(vsid),"r"(seg+ea_offset)
			);
		}
#else
		ea_offset = 0;
#endif
		printk("All hooks installed.\n");
	}

	return malloc_proc(s);
}

STATIC void *
end_fenced_malloc(size_t s)
{
/* get additional space for alignment + fence page */
size_t tot = s + 2*PAGE_SIZE;

char  *ptr = __real_malloc(tot);

char  *end;
char  *rval;
FenceData f;

	if ( !ptr )
		return 0;

	/* align return value so that the block ends on a page boundary */
	end  = (char*)PAGE_ALIGN_UP(ptr + s);
	/* Must still preserve heap alignment */
	rval = (char*)((unsigned32)(end - s) & ~(END_FENCED_MALLOC_ALIGNMENT-1));

	/* store original malloc return value in reserved page */
	f = (FenceData)end;
	f->malloc_addr = ptr;
	f->malloc_size = s;

	/* create a protected mapping */
#ifdef CREATE_NEW_MAPPING
{
	int npages = PAGE_ALIGN_UP(s) >> PAGE_SHIFT;
	assert ( -1 == triv121PgTblMap(pgtbl, TRIV121_SEG_VSID, EA(PAGE_ALIGN_DOWN(rval)), npages, 0, TRIV121_PP_RW_PAGE));
}
#else
	triv121ChangeEaAttributes( (unsigned32)end, -1, 0);
#endif

	return (void*)EA(rval);
}

STATIC FenceData
end_fenced_unmap(void *ea)
{
unsigned32 p = PAGE_ALIGN_DOWN(ea);
APte pte;
#ifdef CREATE_NEW_MAPPING
	while ( (pte = triv121UnmapEa(p)) ) {
		p += PAGE_SIZE;
	}
#else
	while ( (pte = triv121FindPte(TRIV121_SEG_VSID, p)) && pte->pp ) {
		p += PAGE_SIZE;
	}
	assert( pte );
	triv121ChangeEaAttributes( p, -1, TRIV121_PP_RW_PAGE);
#endif
	return (FenceData)PA(p);
}

STATIC FenceData
beg_fenced_unmap(void *ea)
{
FenceData f = ((FenceData)PA(ea))-1;

#ifdef CREATE_NEW_MAPPING
char *b;
	for ( b = ea; b < ea+f->size; b+= PAGE_SIZE )
		triv121UnmapEa((unsigned32)b);
#else
	triv121ChangeEaAttributes( (unsigned32)f, -1, TRIV121_PP_RW_PAGE);
#endif
	return f;
}

STATIC void *
beg_fenced_malloc(size_t s)
{
/* get additional space for alignment + fence page */
size_t tot = s + 2*PAGE_SIZE;

char  *ptr = __real_malloc(tot);

char  *end;
char  *rval;
FenceData f;

	if ( !ptr )
		return 0;

	/* align return value so that the block ends on a page boundary and skip the fence page */
	rval  = (char*)PAGE_ALIGN_UP(ptr) + PAGE_SIZE;
	end   = rval + s;

	/* store original malloc return value in reserved page */

	f = ((FenceData)rval)-1;
	f->malloc_addr = ptr;
	f->malloc_size = s;

#ifdef CREATE_NEW_MAPPING
	{/* create a protected mapping */
	int npages = PAGE_ALIGN_UP(s) >> PAGE_SHIFT;
	assert ( -1 == triv121PgTblMap(pgtbl, TRIV121_SEG_VSID, EA(rval), npages, 0, TRIV121_PP_RW_PAGE));
	}
#else
	triv121ChangeEaAttributes( (unsigned32)f, -1, 0);
#endif

	return (void*)EA(rval);
}

STATIC void
fenced_free(void *ea)
{
FenceData f;
	if ( 0==ea )
		return;
	f = unmap_proc(ea);
	__real_free(f->malloc_addr);
}

STATIC void *
fenced_realloc(void *ea, size_t s)
{
void *rval;
FenceData f;

	if ( !ea )
		return malloc_proc(s);

	if ( !s ) {
		fenced_free(ea);
		return 0;
	}

	rval = malloc_proc(s);

	if ( !rval )
		return 0;


	f = unmap_proc(ea);

	if ( s > f->malloc_size )
		s = f->malloc_size;

	memcpy(rval, f->malloc_addr, s);

	__real_free(f->malloc_addr);

	return rval;
}

/* Finally, the wrappers -- note that we must wrap the re-entrant library versions as well! */
void *
__wrap_malloc(size_t s)
{
	return malloc_proc(s);
}

void *
__wrap__malloc_r(void *reent_ignored, size_t s)
{
	return malloc_proc(s);
}

void *
__wrap_realloc(void *old, size_t s)
{
	return realloc_proc(old, s);
}

void *
__wrap__realloc_r(void *reent_ignored, void *old, size_t s)
{
	return realloc_proc(old,s);
}

void *
__wrap_calloc(size_t nmb, size_t s)
{
unsigned l = nmb*s;
char *rval = malloc_proc(l);
	if ( rval )
		memset(rval, 0, l);
	return rval;
}

void *
__wrap__calloc_r(void *reent_ignored, size_t nmb, size_t s)
{
	return __wrap_calloc(nmb, s);
}

void
__wrap_free(void *p)
{
	free_proc(p);
}

void
__wrap__free_r(void *reent_ignored, void *p)
{
	free_proc(p);
}

