#include"common.h"
#include"memory.h"
#include"memory_private.h"
#include"page.h"
#include"assembly/assembly.h"
#include"interrupt/handler.h"
#include"interrupt/controller/pic.h"
#include"multiprocessor/spinlock.h"
#include"multiprocessor/processorlocal.h"

#define PAGE_TABLE_LENGTH (1024)

#define AUTO_RELEASE_PAGE_FLAG (1 << 11)
typedef enum{
	NONE_AUTO_RELEASE_PAGE = 0,
	AUTO_RELEASE_PAGE = AUTO_RELEASE_PAGE_FLAG
}ExtraPageAttribute;

typedef struct{
	uint8_t present: 1;
	uint8_t writable: 1;
	uint8_t userAccessible: 1; // CPL = 3
	uint8_t writeThrough: 1;
	uint8_t cacheDisabled: 1;
	uint8_t accessed: 1;
	uint8_t dirty: 1;
	uint8_t zero: 1;
	uint8_t global: 1;
	uint8_t unused: 2; // available for OS; unused
	uint8_t autoReleasePhysical: 1; // OS-defined
	uint8_t address0_4: 4;
	uint16_t address4_20: 16;
}PageTableEntry;
typedef struct PageTable{
	volatile PageTableEntry entry[PAGE_TABLE_LENGTH];
}PageTable;

#define PAGE_TABLE_REGION_SIZE (PAGE_SIZE * PAGE_TABLE_LENGTH)

#define PAGE_DIRECTORY_LENGTH (1024)

typedef struct{
	uint8_t present: 1;
	uint8_t writable: 1;
	uint8_t userAccessible: 1;
	uint8_t writeThrough: 1;
	uint8_t cacheDisabled: 1;
	uint8_t accessed: 1;
	uint8_t zero1: 1;
	uint8_t size4MB: 1;
	uint8_t zero2: 1;
	uint8_t unused: 3; // available for OS; unused
	uint8_t address0_4: 4;
	uint16_t address4_20: 16;
}PageDirectoryEntry;
typedef struct PageDirectory{
	volatile PageDirectoryEntry entry[PAGE_DIRECTORY_LENGTH];
}PageDirectory;

static_assert(sizeof(PageDirectoryEntry) == 4);
static_assert(sizeof(PageTableEntry) == 4);
static_assert(sizeof(PageDirectory) % PAGE_SIZE == 0);
static_assert(sizeof(PageTable) % PAGE_SIZE == 0);
static_assert(PAGE_SIZE % MIN_BLOCK_SIZE == 0);

static_assert(USER_LINEAR_BEGIN % PAGE_SIZE == 0);
// see kernel.ld
// static_assert(USER_LINEAR_END % PAGE_SIZE == 0);
// static_assert(KERNEL_LINEAR_BEGIN % PAGE_SIZE == 0);
// static_assert(KERNEL_LINEAR_END % PAGE_SIZE == 0);

#define SET_ENTRY_ADDRESS(E, A) ((*(uint32_t*)(E)) = (((*(uint32_t*)(E)) & (4095)) | ((uint32_t)(A))))
#define GET_ENTRY_ADDRESS(E) ((*(uint32_t*)(E)) & (~4095))

static PhysicalAddress getPTEAddress(volatile PageTableEntry *e){
	PhysicalAddress a = {GET_ENTRY_ADDRESS(e)};
	return a;
}

static void setPTEAddress(PageTableEntry *e, PhysicalAddress a){
	SET_ENTRY_ADDRESS(e, a.value);
}

static PhysicalAddress getPDEAddress(volatile PageDirectoryEntry *e){
	PhysicalAddress a = {GET_ENTRY_ADDRESS(e)};
	return a;
}

static void setPDEAddress(PageDirectoryEntry *e, PhysicalAddress a){
	SET_ENTRY_ADDRESS(e, a.value);
}

#undef GET_ENTRY_ADDRESS
#undef SET_ENTRY_ADDRESS

uint32_t getCR3(void){
	uint32_t value;
	__asm__(
	"mov %%cr3, %0\n"
	:"=a"(value)
	:
	);
	return value;
}

static void setCR3(uint32_t value){
	__asm__(
	"mov  %0, %%cr3\n"
	:
	:"a"(value)
	);
}

static void setCR0PagingBit(void){
	uint32_t cr0 = getCR0();
	if((cr0 & 0x80000000) == 0){
		setCR0(cr0 | 0x80000000);
	}
}

static void invlpgOrSetCR3(uintptr_t linearAddress, size_t size){
	if(size >= 512 * PAGE_SIZE){
		setCR3(getCR3());
	}
	else{
		size_t s;
		for(s = 0; s < size; s += PAGE_SIZE){
			__asm__(
			"invlpg %0\n"
			:
			:"m"(*(char*)(linearAddress + s))
			);
		}
	}
}

// not necessary to invlpg when changing present flag from 0 to 1
#define PD_INDEX(ADDRESS) ((int)(((ADDRESS) >> 22) & (PAGE_DIRECTORY_LENGTH - 1)))
#define PT_INDEX(ADDRESS) ((int)(((ADDRESS) >> 12) & (PAGE_TABLE_LENGTH - 1)))

static void setPDE(
	volatile PageDirectoryEntry *targetPDE, PageAttribute attribute, PhysicalAddress pt_physical
){
	PageDirectoryEntry pde;
	pde.present = 1;
	pde.writable = (attribute & WRITABLE_PAGE_FLAG? 1: 0);
	pde.userAccessible = (attribute & USER_PAGE_FLAG? 1: 0);
	pde.writeThrough = 0;
	pde.cacheDisabled = 0;
	pde.accessed = 0;
	pde.zero1 = 0;
	pde.size4MB = 0;
	pde.zero2 = 0;
	pde.unused = 0;
	setPDEAddress(&pde, pt_physical);
	assert(targetPDE->present == 0);
	(*targetPDE) = pde;
}

static void invalidatePDE(volatile PageDirectoryEntry *targetPDE){
	PageDirectoryEntry pde = *targetPDE;
	pde.present = 0;
	assert(targetPDE->present == 1);
	(*targetPDE) = pde;
}

static void setPTE(
	volatile PageTableEntry *targetPTE, PageAttribute attribute, ExtraPageAttribute extraAttribute,
	PhysicalAddress physicalAddress
){
	PageTableEntry pte;
	pte.present = 1;
	pte.writable = (attribute & WRITABLE_PAGE_FLAG? 1: 0);
	pte.userAccessible = (attribute & USER_PAGE_FLAG? 1: 0);
	pte.writeThrough = 0;
	pte.cacheDisabled = 0;
	pte.accessed = 0;
	pte.dirty = 0;
	pte.zero = 0;
	pte.global = 0;//(type & GLOBAL_PAGE_FLAG? 1: 0);
	pte.unused = 0;
	pte.autoReleasePhysical = (extraAttribute & AUTO_RELEASE_PAGE_FLAG? 1: 0);
	setPTEAddress(&pte, physicalAddress);
	(*targetPTE) = pte;
}

static void invalidatePTE(volatile PageTableEntry *targetPTE){
	PageTableEntry pte = *targetPTE;
	// keep the address in page table
	// see unmapPage_LP
	pte.present = 0;
	assert(targetPTE->present == 1);
	(*targetPTE) = pte;
}

#undef ASSIGN_AND_INVLPG
#undef GLOBAL_PAGE_FLAG
#undef USER_PAGE_FLAG
#undef WRITABLE_PAGE_FLAG

static int isPDEPresent(volatile PageDirectoryEntry *e){
	return e->present;
}

static int isPTEPresent(volatile PageTableEntry *e){
	return e->present;
}

static int isPageAutoRelease(volatile PageTableEntry *e){
	return e->autoReleasePhysical;
}

// kernel page table

// if external == 1, deleteWhenEmpty has to be 0 and presentCount is ignored
typedef struct{
	uint16_t presentCount;
	uint16_t releaseWhenEmpty: 1;
	uint16_t external: 1;
	uint16_t unused: 14;
}PageTableAttribute;

typedef struct PageTableSet{
	PageDirectory pd;
	PageTableAttribute ptAttribute[PAGE_DIRECTORY_LENGTH];
	PageTable pt[PAGE_DIRECTORY_LENGTH];
}PageTableSet;

static_assert((sizeof(PageTableAttribute) * PAGE_DIRECTORY_LENGTH) % PAGE_SIZE == 0);

// pd->entry[i] map to pt[(i + ptIndexBase) % PAGE_DIRECTORY_LENGTH]
// pdIndexBase = (4G-&pageManager)>>22, so that &pageManager is at pt[0]
struct PageManager{
	PhysicalAddress physicalPD;
	uintptr_t reservedBase;
	uintptr_t reservedEnd;
	int pdIndexBase;
	PageTableSet *page;
	const PageTableSet *pageInUserSpace;
};

PageManager *kernelPageManager = NULL;
static uintptr_t kLinearBegin, kLinearEnd;


uint32_t toCR3(PageManager *p){
	// bit 3: write through
	// bit 4: cache disable
	return p->physicalPD.value;
}

#define PD_INDEX_ADD_BASE(LINEAR) ((PD_INDEX(linear) + p->pdIndexBase) & (PAGE_DIRECTORY_LENGTH - 1))

static PageTable *linearAddressOfPageTable(PageManager *p, uintptr_t linear){
	return p->page->pt + PD_INDEX_ADD_BASE(linear);
}
static PageTableAttribute *linearAddressOfPageTableAttribute(PageManager *p, uintptr_t linear){
	return p->page->ptAttribute + PD_INDEX_ADD_BASE(linear);
}

#undef PD_INDEX_ADD_BASE

PhysicalAddress _allocatePhysicalPages(MemoryBlockManager *physical, size_t size){
	size_t p_size = size;
	PhysicalAddress p_address = {allocateBlock(physical, &p_size)};
	return p_address;
}

void _releasePhysicalPages(MemoryBlockManager *physical, PhysicalAddress address){
	releaseBlock(physical, address.value);
}

static PhysicalAddress translatePage(PageManager *p, uintptr_t linearAddress, int isPresent){
	int i1 = PD_INDEX(linearAddress);
	int i2 = PT_INDEX(linearAddress);
	if(isPDEPresent(p->page->pd.entry + i1) == 0){
		panic("translatePage: page table not present");
	}
	PageTable *pt = linearAddressOfPageTable(p, linearAddress);
	if(isPresent != isPTEPresent(pt->entry + i2)){
		panic("translatePage: page present bit differs to expected");
	}
	return getPTEAddress(pt->entry + i2);
}

// return 1 if success, 0 if error
static int setPage(
	PageManager *p,
	MemoryBlockManager *physical,
	uintptr_t linearAddress, PhysicalAddress physicalAddress,
	PageAttribute attribute, ExtraPageAttribute extraAttribute
){
	assert((physicalAddress.value & 4095) == 0 && (linearAddress & 4095) == 0);
	int i1 = PD_INDEX(linearAddress);
	int i2 = PT_INDEX(linearAddress);
	PageTable *pt_linear = linearAddressOfPageTable(p, linearAddress);
	PageTableAttribute *pt_attribute = linearAddressOfPageTableAttribute(p, linearAddress);
	if(isPDEPresent(p->page->pd.entry + i1) == 0){
		if(physical == NULL){
			panic("physical memory manager == NULL");
			return 0;
		}
		PhysicalAddress pt_physical = _allocatePhysicalPages(physical, sizeof(PageTable));
		if(pt_physical.value == UINTPTR_NULL){
			return 0;
		}
		// the userAccesible/writable bits in all page levels must be 1 to allow the operations
		PageTableAttribute *pta = linearAddressOfPageTableAttribute(p, linearAddress);
		pta->presentCount = 0;
		pta->releaseWhenEmpty = 1;
		pta->external = 0;
		setPDE(p->page->pd.entry + i1, USER_WRITABLE_PAGE, pt_physical);
#ifndef NDEBUG
		uintptr_t pt_i1 = PD_INDEX((uintptr_t)pt_linear);
		assert(isPDEPresent(p->page->pd.entry + pt_i1));
#endif
		size_t s;
		for(s = 0; s < sizeof(PageTable); s += PAGE_SIZE){
			PhysicalAddress pt_physical_s = {pt_physical.value + s};
			if(setPage(p, NULL, ((uintptr_t)pt_linear) + s, pt_physical_s,
					KERNEL_PAGE, NONE_AUTO_RELEASE_PAGE) == 0){
				panic("pt_linear must present in kernel page directory");
			}
		}
		MEMSET0(pt_linear);
	}
	assert((((uintptr_t)pt_linear) & 4095) == 0);
	if(pt_attribute->external == 0){
		//TODO: multiprocessor spinlock
		pt_attribute->presentCount++;
	}
	setPTE(pt_linear->entry + i2, attribute, extraAttribute, physicalAddress);
	assert(pt_linear->entry[i2].present == 1);

	return 1;
}

static void invalidatePage(
	PageManager *p,
	uintptr_t linear
){
	int i2 = PT_INDEX(linear);
	PageTable *pt_linear = linearAddressOfPageTable(p, linear);
	assert(isPTEPresent(pt_linear->entry + i2));
	invalidatePTE(pt_linear->entry + i2);
}

static void releaseInvalidatedPage(
	PageManager *p,
	MemoryBlockManager *physical,
	uintptr_t linear
){
	int i1 = PD_INDEX(linear);
	int i2 = PT_INDEX(linear);

	PageTable *pt_linear = linearAddressOfPageTable(p, linear);
	PageTableAttribute *pt_attribute = linearAddressOfPageTableAttribute(p ,linear);
	assert(isPDEPresent(p->page->pd.entry + i1));
	assert(isPTEPresent(pt_linear->entry + i2) == 0);

	if(isPageAutoRelease(pt_linear->entry + i2)){
		PhysicalAddress page_physical = getPTEAddress(pt_linear->entry + i2);
		_releasePhysicalPages(physical, page_physical);
	}
	if(pt_attribute->external == 0){
		pt_attribute->presentCount--;
		if(pt_attribute->presentCount == 0 && pt_attribute->releaseWhenEmpty != 0){
			PhysicalAddress pt_physical = getPDEAddress(p->page->pd.entry + i1);
			invalidatePDE(p->page->pd.entry + i1);
			_releasePhysicalPages(physical , pt_physical);
		}
	}
}

static size_t evaluateSizeOfPageTableSet(uintptr_t reservedBase, uintptr_t reservedEnd){
	size_t s = 0;
	// PageDirectory
	s += (uintptr_t)(((PageTableSet*)0)->pt);
	// PageTable
	uintptr_t manageBaseIndex =  PD_INDEX(reservedBase);
	uintptr_t manageEndIndex = PD_INDEX(reservedEnd - 1 >= reservedBase? reservedEnd - 1: reservedEnd);
	s += (manageEndIndex - manageBaseIndex + 1) * sizeof(PageTable);
	return s;
}

enum PhyscialMapping{
	// MAP_TO_NEW_PHYSICAL,
	MAP_TO_PRESENT_PHYSICAL = 1,
	MAP_TO_KERNEL_RESERVED = 2
};

static PhysicalAddress linearToPhysical(enum PhyscialMapping mapping, void *linear){
	PhysicalAddress physical;
	switch(mapping){
	case MAP_TO_PRESENT_PHYSICAL:
		physical = translatePage(kernelPageManager, (uintptr_t)linear, 1);
		break;
	case MAP_TO_KERNEL_RESERVED:
		physical.value = (((uintptr_t)(linear)) - kLinearBegin);
		break;
	default:
		panic("invalid argument");
	}
	return physical;
}

static void initPageManager(
	PageManager *p, const PageTableSet *tablesLoadAddress, PageTableSet *tables,
	uintptr_t reservedBase, uintptr_t reservedEnd, enum PhyscialMapping mapping
){
	assert(((uintptr_t)tables) % PAGE_SIZE == 0);
	assert(((uintptr_t)tablesLoadAddress) % PAGE_SIZE == 0);

	p->reservedBase = reservedBase;
	p->reservedEnd = reservedEnd;
	p->page = tables;
	p->pageInUserSpace = tablesLoadAddress;
	p->physicalPD = linearToPhysical(mapping, &(tables->pd));
	p->pdIndexBase = (PAGE_DIRECTORY_LENGTH - PD_INDEX(reservedBase)) & (PAGE_DIRECTORY_LENGTH - 1);
	assert(linearAddressOfPageTable(p, reservedBase) == tables->pt + 0);
	PageDirectory *kpd = &tables->pd;
	MEMSET0(kpd);
}

static void initPageManagerPD(
	PageManager *p,
	uintptr_t linearBase, uintptr_t linearEnd, enum PhyscialMapping mapping
){
	PageTableSet *pts = p->page;
	uintptr_t a;
	// avoid overflow
	const uintptr_t b = PD_INDEX(linearBase), e = PD_INDEX(linearEnd - 1);
	for(a = b; a <= e; a++){
		int pdIndex = PD_INDEX(a * PAGE_TABLE_REGION_SIZE);
		PageTable *kpt = linearAddressOfPageTable(p, a * PAGE_TABLE_REGION_SIZE);
		PageTableAttribute *kpta = linearAddressOfPageTableAttribute(p, a * PAGE_TABLE_REGION_SIZE);
		MEMSET0(kpt);
		kpta->presentCount = 0;
		kpta->releaseWhenEmpty = 0;
		kpta->external = 0;
		PhysicalAddress kpt_physical = linearToPhysical(mapping, kpt);
		setPDE(pts->pd.entry + pdIndex, KERNEL_PAGE, kpt_physical);
	}
}

static void copyPageManagerPD(
	PageManager *dst, const PageManager *src,
	uintptr_t linearBegin, uintptr_t linearEnd
){
	uintptr_t a, b = PD_INDEX(linearBegin), e = PD_INDEX(linearEnd - 1);
	for(a = b; a <= e; a++){
		assert(isPDEPresent(dst->page->pd.entry + a) == 0);
		assert(isPDEPresent(src->page->pd.entry + a) != 0);
		PageTableAttribute *pta = linearAddressOfPageTableAttribute(dst, a * PAGE_TABLE_REGION_SIZE);
		pta->releaseWhenEmpty = 0;
		pta->presentCount = 0;
		pta->external = 1;
		dst->page->pd.entry[a] = src->page->pd.entry[a];
	}
}

static void initPageManagerPT(
	PageManager *p,
	uintptr_t linearBegin, uintptr_t linearEnd, uintptr_t mappedlinearBegin, enum PhyscialMapping mapping
){
	uintptr_t a;
	for(a = linearBegin; a < linearEnd; a += PAGE_SIZE){
		assert(isPDEPresent(p->page->pd.entry + PD_INDEX(a)));
		PhysicalAddress kp_physical = linearToPhysical(mapping, (void*)(a - linearBegin + mappedlinearBegin));
		setPage(p, NULL, a, kp_physical, KERNEL_PAGE, NONE_AUTO_RELEASE_PAGE);
	}
}

int _mapExistingPages_L(
	MemoryBlockManager *physical, PageManager *dst, PageManager *src,
	void *dstLinear, uintptr_t srcLinear, size_t size,
	PageAttribute attribute
){
	assert(srcLinear % PAGE_SIZE == 0 && ((uintptr_t)dstLinear) % PAGE_SIZE == 0 && size % PAGE_SIZE == 0);
	uintptr_t s;
	for(s = 0; s < size; s += PAGE_SIZE){
		PhysicalAddress srcPhysical = translatePage(src, srcLinear + s, 1);
		if(setPage(dst, physical, ((uintptr_t)dstLinear) + s, srcPhysical, attribute, NONE_AUTO_RELEASE_PAGE) == 0){
			break;
		}
	}
	EXPECT(s >= size);

	return 1;

	ON_ERROR;
	_unmapPage_LP(dst, physical, dstLinear, s);
	return 0;
}

void unmapUserPageTableSet(PageManager *p){
	unmapKernelPage(p->page);
	p->page = (PageTableSet*)p->pageInUserSpace;
}

PageManager *initKernelPageTable(
	uintptr_t manageBase, uintptr_t manageBegin, uintptr_t manageEnd,
	uintptr_t kernelLinearBegin, uintptr_t kernelLinearEnd
){
	assert(kernelLinearBegin % PAGE_TABLE_REGION_SIZE == 0 && kernelLinearEnd % PAGE_SIZE == 0);
	assert(manageBase >= kernelLinearBegin && manageEnd <= kernelLinearEnd);
	assert(kernelLinearBegin == KERNEL_LINEAR_BEGIN);
	assert(kernelPageManager == NULL);

	kernelPageManager = (PageManager*)manageBegin;
	kLinearBegin = kernelLinearBegin;
	kLinearEnd = kernelLinearEnd;

	manageBegin += sizeof(*kernelPageManager);
	if(manageBegin % PAGE_SIZE != 0){
		manageBegin += (PAGE_SIZE - (manageBegin % PAGE_SIZE));
	}
	if(manageBegin + evaluateSizeOfPageTableSet(manageBase, manageEnd) > manageEnd){
		panic("insufficient reserved memory for kernel page table");
	}
	initPageManager(
		kernelPageManager, (PageTableSet*)manageBegin, (PageTableSet*)manageBegin,
		manageBase, manageEnd, MAP_TO_KERNEL_RESERVED
	);
	initPageManagerPD(kernelPageManager, kLinearBegin, kernelLinearEnd, MAP_TO_KERNEL_RESERVED);
	initPageManagerPT(kernelPageManager, manageBase, manageEnd, manageBase, MAP_TO_KERNEL_RESERVED);
	setCR3(toCR3(kernelPageManager));
	setCR0PagingBit();
	return kernelPageManager;
}

// multiprocessor TLB

static void sendINVLPG_disabled(
	__attribute__((__unused__)) uint32_t cr3,
	uintptr_t linearAddress, size_t size
){
	invlpgOrSetCR3(linearAddress, size);
}

struct INVLPGArguments{
	// int finishCount;
	uint32_t cr3;
	uintptr_t linearAddress;
	size_t size;
	int isGlobal;
};
volatile struct INVLPGArguments args;
static InterruptVector *invlpgVector = NULL;
static void (*sendINVLPG)(uint32_t cr3, uintptr_t linearAddress, size_t size) = sendINVLPG_disabled;

static void invlpgHandler(InterruptParam *p){
	if(args.isGlobal || args.cr3 == getCR3()){
		invlpgOrSetCR3(args.linearAddress, args.size);
	}
	getProcessorLocal()->pic->endOfInterrupt(p);
	sti();
}

static void sendINVLPG_enabled(uint32_t cr3, uintptr_t linearAddress, size_t size){
	static Spinlock lock = INITIAL_SPINLOCK;
	acquireLock(&lock);
	{
		PIC *pic = getProcessorLocal()->pic;
		args.cr3 = cr3;
		args.linearAddress = linearAddress;
		args.size = size;
		args.isGlobal = (linearAddress >= KERNEL_LINEAR_BEGIN? 1: 0);
		pic->interruptAllOther(pic, invlpgVector);
		invlpgOrSetCR3(linearAddress, size);
	}
	releaseLock(&lock);
}

void initMultiprocessorPaging(InterruptTable *t){
	invlpgVector = registerGeneralInterrupt(t, invlpgHandler, 0);
	sendINVLPG = sendINVLPG_enabled;
}

// user page table

const size_t sizeOfPageTableSet = sizeof(PageTableSet);
#define MAX_USER_RESERVED_PAGES (4)

// create an page table in kernel linear memory
// with manageBase ~ manageEnd (linear address) mapped to physical address
// targetAddress ~ targetAddress + sizeOfPageTableSet is free
PageManager *createAndMapUserPageTable(uintptr_t targetAddress){
	uintptr_t targetBegin = targetAddress;
	uintptr_t targetEnd = targetAddress + sizeOfPageTableSet;
	EXPECT(targetAddress % PAGE_SIZE == 0);
	PageManager *NEW(p);
	EXPECT(p != NULL);
	size_t evalSize = evaluateSizeOfPageTableSet(targetBegin, targetEnd);
	EXPECT(targetAddress >= targetBegin && targetAddress + evalSize <= targetEnd);
	assert(evalSize <= MAX_USER_RESERVED_PAGES * PAGE_SIZE);
	PageTableSet *pts = allocateKernelPages(evalSize);
	EXPECT(pts != NULL);
	initPageManager(
		p, (PageTableSet*)targetAddress, pts,
		targetBegin, targetEnd, MAP_TO_PRESENT_PHYSICAL
	);
	copyPageManagerPD(p, kernelPageManager, kLinearBegin, kLinearEnd);
	initPageManagerPD(p, targetBegin, targetEnd, MAP_TO_PRESENT_PHYSICAL);
	initPageManagerPT(p, targetBegin, targetBegin + evalSize, (uintptr_t)pts, MAP_TO_PRESENT_PHYSICAL);
	return p;

	ON_ERROR;
	ON_ERROR;
	DELETE(p);
	ON_ERROR;
	ON_ERROR;
	return NULL;
}

// assume the page manager remains only reservedBase ~ reservedEnd; the other pages does not need to be released
void deleteUserPageTable(PageManager *p){
	assert(getCR3() == p->physicalPD.value);
	PhysicalAddress reservedPhysical[MAX_USER_RESERVED_PAGES];
	assert(p->reservedBase % PAGE_SIZE == 0 && p->reservedEnd % PAGE_SIZE == 0);
	uintptr_t r;
	int i;
	for(r = p->reservedBase, i = 0; r < p->reservedEnd; r += PAGE_SIZE, i++){
		reservedPhysical[i] = translatePage(p, r, 1);
	}
	for(r = p->reservedBase, i = 0; r < p->reservedEnd; r += PAGE_SIZE, i++){
		releasePhysicalPages(reservedPhysical[i]);
	}
	DELETE(p);
}

int _mapPage_LP(
	PageManager *p, MemoryBlockManager *physical,
	void *linearAddress, PhysicalAddress physicalAddress, size_t size,
	PageAttribute attribute
){
	size_t s;
	for(s = 0; s < size; s += PAGE_SIZE){
		uintptr_t l_addr = ((uintptr_t)linearAddress) + s;
		PhysicalAddress p_addr = {physicalAddress.value + s};
		int result = setPage(p, physical, l_addr, p_addr, attribute, NONE_AUTO_RELEASE_PAGE);
		if(result == 0){
			break;
		}
	}
	EXPECT(s >= size);
	return 1;
	ON_ERROR;
	_unmapPage_LP(p, physical, linearAddress, s);
	return 0;
}

void _unmapPage_LP(PageManager *p, MemoryBlockManager *physical, void *linearAddress, size_t size){
	size_t s = size;
	while(s != 0){
		s -= PAGE_SIZE;
		invalidatePage(p, ((uintptr_t)linearAddress) + s);
	}
	sendINVLPG(p->physicalPD.value, (uintptr_t)linearAddress, size);
	// the pages are not yet released by linear memory manager
	// it is safe to keep address in PTE, and
	// separate _unmapPage_LP & releasePhysicalPages
	// TODO:
	s = size;
	while(s != 0){
		s -= PAGE_SIZE;
		releaseInvalidatedPage(p, physical, ((uintptr_t)linearAddress) + s);
	}
}

int _mapPage_L(
	PageManager *p, MemoryBlockManager *physical,
	void *linearAddress, size_t size,
	PageAttribute attribute
){
	uintptr_t l_addr = (uintptr_t)linearAddress;
	assert(size % PAGE_SIZE == 0);
	size_t s;
	for(s = 0; s < size; s += PAGE_SIZE){
		PhysicalAddress p_addr = _allocatePhysicalPages(physical, PAGE_SIZE);
		if(p_addr.value == UINTPTR_NULL){
			break;
		}
		int result = setPage(p, physical, l_addr + s, p_addr, attribute, AUTO_RELEASE_PAGE);
		if(result != 1){
			_releasePhysicalPages(physical, p_addr);
			break;
		}
	}
	EXPECT(s >= size);

	return 1;

	ON_ERROR;
	_unmapPage_L(p, physical, linearAddress, s);
	return 0;
}

void _unmapPage_L(PageManager *p, MemoryBlockManager *physical, void *linearAddress, size_t size){
	_unmapPage_LP(p, physical, linearAddress, size);
	//TODO: remove this function
}

