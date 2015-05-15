#include"common.h"
#include"memory.h"
#include"memory_private.h"
#include"page.h"
#include"assembly/assembly.h"

#define PAGE_TABLE_SIZE (4096)
#define PAGE_TABLE_LENGTH (1024)

typedef struct{
	uint8_t present: 1;
	uint8_t writable: 1;
	uint8_t userAccessible: 1;
	uint8_t writeThrough: 1;
	uint8_t cacheDisabled: 1;
	uint8_t accessed: 1;
	uint8_t dirty: 1;
	uint8_t zero: 1;
	uint8_t global: 1;
	uint8_t unused: 3;
	uint8_t address0_4: 4;
	uint16_t address4_20: 16;
}PageTableEntry;
typedef struct PageTable{
	volatile PageTableEntry entry[PAGE_TABLE_LENGTH];
}PageTable;

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
	uint8_t unused: 3;
	uint8_t address0_4: 4;
	uint16_t address4_20: 16;
}PageDirectoryEntry;
typedef struct PageDirectory{
	volatile PageDirectoryEntry entry[PAGE_TABLE_LENGTH];
}PageDirectory;

static_assert(sizeof(PageDirectoryEntry) == 4);
static_assert(sizeof(PageTableEntry) == 4);
static_assert(sizeof(PageDirectory) == PAGE_TABLE_SIZE);
static_assert(sizeof(PageTable) == PAGE_TABLE_SIZE);
static_assert(PAGE_TABLE_SIZE % MIN_BLOCK_SIZE == 0);


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

static void invlpg(uintptr_t linearAddress){
	__asm__(
	"invlpg %0\n"
	:
	:"m"(*(uint8_t*)linearAddress)
	);
}

// not necessary to invlpg when changing present flag from 0 to 1
#define PD_INDEX(ADDRESS) ((int)(((ADDRESS) >> 22) & (PAGE_TABLE_LENGTH - 1)))
#define PT_INDEX(ADDRESS) ((int)(((ADDRESS) >> 12) & (PAGE_TABLE_LENGTH - 1)))

static void setPDE(
	volatile PageDirectoryEntry *targetPDE, enum PageType type, PhysicalAddress pt_physical
){
	PageDirectoryEntry pde;
	pde.present = 1;
	pde.writable = (type & WRITABLE_PAGE_FLAG? 1: 0);
	pde.userAccessible = (type & USER_PAGE_FLAG? 1: 0);
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
	// invlpg(linearAddress);
}

#define ASSIGN_AND_INVLPG(TARGET, VALUE, L_ADDRESS) \
do{\
	int needINVLPG = ((TARGET).present != 0);\
	(TARGET) = (VALUE);\
	if(needINVLPG){\
		invlpg(L_ADDRESS);\
	}\
}while(0)

static void setPTE(
	volatile PageTableEntry *targetPTE, enum PageType type, PhysicalAddress physicalAddress,
	uintptr_t linearAddress
){
	PageTableEntry pte;
	pte.present = 1;
	pte.writable = (type & WRITABLE_PAGE_FLAG? 1: 0);
	pte.userAccessible = (type & USER_PAGE_FLAG? 1: 0);
	pte.writeThrough = 0;
	pte.cacheDisabled = 0;
	pte.accessed = 0;
	pte.dirty = 0;
	pte.zero = 0;
	pte.global = 0;//(type & GLOBAL_PAGE_FLAG? 1: 0);
	pte.unused = 0;
	setPTEAddress(&pte, physicalAddress);
	ASSIGN_AND_INVLPG(*targetPTE, pte, linearAddress);
}

static void invalidatePTE(volatile PageTableEntry *targetPTE, uintptr_t linearAddress){
	PageTableEntry pte;
	*((uint32_t*)&pte) = 0;
	ASSIGN_AND_INVLPG(*targetPTE, pte, linearAddress);
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

// kernel page table
typedef struct PageTableSet{
	PageDirectory pd;
	PageTable pt[PAGE_TABLE_LENGTH];
}PageTableSet;

// pd->entry[i] map to pt[(i + ptIndexBase) % PAGE_TABLE_LENGTH]
// pdIndexBase = (4G-&pageManager)>>22, so that &pageManager is at pt[0]
struct PageManager{
	PhysicalAddress physicalPD;
	uintptr_t reservedBase;
	uintptr_t reservedEnd;
	int pdIndexBase;
	PageTableSet *page;
	const PageTableSet *pageInUserSpace;
};

PageManager *kernelPageManager;
static uintptr_t kLinearBegin, kLinearEnd;


uint32_t toCR3(PageManager *p){
	// bit 3: write through
	// bit 4: cache disable
	return p->physicalPD.value;
}

static PageTable *linearAddressOfPageTable(PageManager *p, uintptr_t linear){
	int index = ((PD_INDEX(linear) + p->pdIndexBase) & (PAGE_TABLE_LENGTH - 1));
	return p->page->pt + index;
}

PhysicalAddress translatePage(PageManager *p, uintptr_t linearAddress){
	int i1 = PD_INDEX(linearAddress);
	int i2 = PT_INDEX(linearAddress);
	if(isPDEPresent(p->page->pd.entry + i1) == 0){
		panic("translatePage: page not present");
	}
	PageTable *pt = linearAddressOfPageTable(p, linearAddress);
	return getPTEAddress(pt->entry + i2);
}

int setPage(
	PageManager *p,
	MemoryBlockManager *physical,
	uintptr_t linearAddress, PhysicalAddress physicalAddress,
	enum PageType pageType
){
	assert((physicalAddress.value & 4095) == 0 && (linearAddress & 4095) == 0);
	int i1 = PD_INDEX(linearAddress);
	int i2 = PT_INDEX(linearAddress);
	PageTable *pt_linear = linearAddressOfPageTable(p, linearAddress);
	if(isPDEPresent(p->page->pd.entry + i1) == 0){
		PhysicalAddress pt_physical = _allocatePhysicalPages(physical, sizeof(PageTable));
		if(pt_physical.value == UINTPTR_NULL){
			return 0;
		}
		setPDE(p->page->pd.entry + i1, USER_WRITABLE_PAGE, pt_physical);
#ifndef NDEBUG
		uintptr_t pt_i1 = PD_INDEX((uintptr_t)pt_linear);
		assert(isPDEPresent(p->page->pd.entry + pt_i1));
#endif
		if(setPage(p, physical, (uintptr_t)pt_linear, pt_physical, KERNEL_PAGE) == 0){
			assert(0); // pt_linear must present in kernel page directory
		}
		MEMSET0(pt_linear);
	}
	assert((((uintptr_t)pt_linear) & 4095) == 0);

	setPTE(pt_linear->entry + i2, pageType, physicalAddress, linearAddress);
	assert(pt_linear->entry[i2].present == 1);

	return 1;
}

void invalidatePage(
	PageManager *p,
	MemoryBlockManager *physical,
	uintptr_t linear
){
	int i1 = PD_INDEX(linear);
	int i2 = PT_INDEX(linear);
	PageTable *pt_linear = linearAddressOfPageTable(p, linear);
	PhysicalAddress pt_physical = getPDEAddress(p->page->pd.entry + i1);
	assert(isPDEPresent(p->page->pd.entry + i1));
	assert(isPTEPresent(pt_linear->entry + i2));

	invalidatePTE(pt_linear->entry + i2, linear);
	if(0){// do not release even if the PageTable is empty
		//invalidatePDE(p->page->pd.entry + i1);
		releaseBlock(physical , pt_physical.value);
	}
}

static size_t evaluateSizeOfPageTableSet(uintptr_t reservedBase, uintptr_t reservedEnd){
	size_t s = 0;
	// PageDirectory
	s += sizeof(PageDirectory);
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
		physical = translatePage(kernelPageManager, (uintptr_t)linear);
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
	assert(((uintptr_t)tables) % PAGE_TABLE_SIZE == 0);
	assert(((uintptr_t)tablesLoadAddress) % PAGE_TABLE_SIZE == 0);

	p->reservedBase = reservedBase;
	p->reservedEnd = reservedEnd;
	p->page = tables;
	p->pageInUserSpace = tablesLoadAddress;
	p->physicalPD = linearToPhysical(mapping, &(tables->pd));
	p->pdIndexBase = (PAGE_TABLE_LENGTH - PD_INDEX(reservedBase)) & (PAGE_TABLE_LENGTH - 1);
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
		int pdIndex = PD_INDEX(a * PAGE_SIZE * PAGE_TABLE_LENGTH);
		PageTable *kpt = linearAddressOfPageTable(p, a * PAGE_SIZE * PAGE_TABLE_LENGTH);
		MEMSET0(kpt);
		PhysicalAddress kpt_physical = linearToPhysical(mapping, kpt);
		setPDE(pts->pd.entry + pdIndex, KERNEL_PAGE, kpt_physical);
	}
}

static void copyPageManagerPD(
	PageManager *dst, const PageManager *src,
	uintptr_t linearBegin, uintptr_t linearEnd
){
	uintptr_t a, b = PD_INDEX(linearBegin), e = PD_INDEX(linearEnd);
	for(a = b; a<=e; a++){
		assert(isPDEPresent(dst->page->pd.entry + a) == 0);
		assert(isPDEPresent(src->page->pd.entry + a) != 0);
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
		PageTable *kpt = linearAddressOfPageTable(p, a);
		int ptIndex = PT_INDEX(a);
		PhysicalAddress kp_physical = linearToPhysical(mapping, (void*)(a - linearBegin + mappedlinearBegin));
		setPTE(kpt->entry + ptIndex, KERNEL_PAGE, kp_physical, a);
	}
}

int _mapExistingPages(
	MemoryBlockManager *physical, PageManager *dst, PageManager *src,
	uintptr_t dstLinear, uintptr_t srcLinear, size_t size
){
	assert(srcLinear % PAGE_SIZE == 0 && dstLinear % PAGE_SIZE == 0 && size % PAGE_SIZE == 0);
	uintptr_t s;
	for(s = 0; s < size; s += PAGE_SIZE){
		PhysicalAddress p = translatePage(src, srcLinear + s);
		if(setPage(dst, physical, dstLinear + s, p, KERNEL_PAGE) == 0){
			break;
		}
	}
	EXPECT(s >= size);

	return 1;

	ON_ERROR;
	while(s != 0){
		s -= PAGE_SIZE;
		invalidatePage(dst, physical, dstLinear + s);
	}
	return 0;
}

void unmapUserPageTableSet(PageManager *p){
	unmapPageToPhysical(p->page);
	p->page = (PageTableSet*)p->pageInUserSpace;
}

PageManager *initKernelPageTable(
	uintptr_t manageBase, uintptr_t manageBegin, uintptr_t manageEnd,
	uintptr_t kernelLinearBegin, uintptr_t kernelLinearEnd
){
	assert(kernelLinearBegin % (PAGE_SIZE * PAGE_TABLE_LENGTH) == 0 && kernelLinearEnd % PAGE_SIZE == 0);
	assert(manageBase >= kernelLinearBegin && manageEnd <= kernelLinearEnd);
	assert(kernelLinearBegin == KERNEL_LINEAR_BEGIN);

	kernelPageManager = (PageManager*)manageBegin;
	kLinearBegin = kernelLinearBegin;
	kLinearEnd = kernelLinearEnd;

	manageBegin += sizeof(*kernelPageManager);
	if(manageBegin % PAGE_TABLE_SIZE != 0){
		manageBegin += (PAGE_TABLE_SIZE - (manageBegin % PAGE_TABLE_SIZE));
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

// user page table

const size_t sizeOfPageTableSet = sizeof(PageTableSet);

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
	PageTableSet *pts = allocateAndMapPages(evalSize);
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

// TODO
void deleteUserPageTable(PageManager *p){
	assert(getCR3() == p->physicalPD.value);
	int pdIndex;
	for(pdIndex = 0; pdIndex < PAGE_TABLE_LENGTH; pdIndex++){
		if(isPDEPresent(p->page->pd.entry + pdIndex) == 0){
			continue;
		}
		PageTable *pt = linearAddressOfPageTable(p, pdIndex * PAGE_TABLE_LENGTH * PAGE_SIZE);
		int ptIndex;
		for(ptIndex = 0; ptIndex < PAGE_TABLE_LENGTH; ptIndex++){
			if(isPTEPresent(pt->entry + ptIndex) == 0)
				continue;
			uintptr_t linear = (pdIndex * PAGE_TABLE_LENGTH + ptIndex) * PAGE_SIZE;
			if(linear >= p->reservedBase && linear < p->reservedEnd)
				continue;
			// do not release the page
			//invalidatePage(linear);
		}
		if(((uintptr_t)pt) >= p->reservedBase && ((uintptr_t)pt) < p->reservedEnd)
			continue;
		// unmap and release the page
	}

	DELETE(p);
}

int _mapPageFromLinear(PageManager *p, MemoryBlockManager *physical, void *linearAddress, size_t size){
	uintptr_t l_addr = (uintptr_t)linearAddress;
	assert(size % PAGE_SIZE == 0);
	size_t s;
	for(s = 0; s < size; s += PAGE_SIZE){
		PhysicalAddress p_addr = _allocatePhysicalPages(physical, PAGE_SIZE);
		if(p_addr.value == UINTPTR_NULL){
			break;
		}
		int result = setPage(p, physical, l_addr + s, p_addr, USER_WRITABLE_PAGE);
		if(result != 1){
			releasePhysicalPages(p_addr);
			break;
		}
	}
	EXPECT(s >= size);

	return 1;

	ON_ERROR;
	_unmapPageFromLinear(p, physical, linearAddress, s);
	return 0;
}

void _unmapPageFromLinear(PageManager *p, MemoryBlockManager *physical, void *linearAddress, size_t size){
	uintptr_t l_addr = (uintptr_t)linearAddress;
	size_t s = size;
	while(s != 0){
		s -= PAGE_SIZE;
		PhysicalAddress p_addr = translatePage(p, l_addr + s);
		invalidatePage(p, physical, l_addr + s);
		_releasePhysicalPages(physical, p_addr);
	}
}

int _mapPage_LP(
	PageManager *p, MemoryBlockManager *physical,
	void *linearAddress, PhysicalAddress physicalAddress, size_t size
){

	size_t s;
	for(s = 0; s < size; s += PAGE_SIZE){
		uintptr_t l_addr = ((uintptr_t)linearAddress) + s;
		PhysicalAddress p_addr = {physicalAddress.value + s};
		int result = setPage(p, physical, l_addr, p_addr, USER_WRITABLE_PAGE);
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
		invalidatePage(p, physical, ((uintptr_t)linearAddress) + s);
	}
}



/*
UserPageTable *_mapUserPageTable(LinearMemoryManager *m, SlabManager *s, PhysicalAddress p){
	UserPageTable *userPageTable = allocateSlab(s, sizeof(UserPageTable));
	if(userPageTable == NULL){
		goto error_userPageTable;
	}
	userPageTable->p_pd = p;
	userPageTable->pd = mapPhysical(m, p, sizeof(PageDirectory));
	if(userPageTable->pd == NULL){
		goto error_pd;
	}

	userPageTable->p_pt.value = UINTPTR_NULL;
	userPageTable->pdIndex = -1;
	userPageTable->pt = mapPhysical(m, userPageTable->p_pt, sizeof(PageTable));
	if(userPageTable->pt == NULL){
		goto error_pt;
	}

	return userPageTable;

	error_pt:
	unmapPhysical(m, userPageTable->pd);
	error_pd:
	releaseSlab(s, userPageTable);
	error_userPageTable:
	return NULL;
}

PhysicalAddress _unmapUserPageTable(LinearMemoryManager *m, SlabManager *s, UserPageTable *p){
	assert(p->pd != NULL);
	PhysicalAddress p_userPT = translatePage(m->page, p->pd);
	unmapPhysical(m, p->pt);
	unmapPhysical(m, p->pd);
	releaseSlab(s, p);
	return p_userPT;
}

UserPageTable *_createUserPageTable(LinearMemoryManager *m, SlabManager *s){
	PhysicalAddress p_userPT = allocatePhysical(m, sizeof(PageDirectory));
	if(p_userPT.value == UINTPTR_NULL){
		goto error_physical;
	}
	UserPageTable *l_userPT = _mapUserPageTable(m, s, p_userPT);
	if(l_userPT == NULL){
		goto error_linear;
	}
	MEMSET0(l_userPT->pd);

	return l_userPT;

	error_linear:
	releasePhysical(m, p_userPT);
	error_physical:
	return NULL;
}

void _deleteUserPageTable(LinearMemoryManager *m, SlabManager *s, UserPageTable *p){
//TODO: releasePhysical PT
	PhysicalAddress p_userPT = _unmapUserPageTable(m, s, p);
	releasePhysical(m, p_userPT);
}

static void remapUserPageTable(
	LinearMemoryManager *m, UserPageTable *p,
	int pdIndex
){
	if(p->pdIndex == pdIndex){
		return;
	}
	PhysicalAddress pt_physical = getPDEAddress(p->pd->entry + pdIndex);
	for(s = 0; s < sizeof(PageTable); s += PAGE_SIZE){
		PhysicalAddress pt_physical_s = {pt_physical.value + s};
		if(setKernelPage(m, ((uintptr_t)(p->pt)) + s, pt_physical_s) == 0){// TODO: see mapPhysical
			assert(0); // remap existing physical page but returns 0
		}
	}
	p->pdIndex = pdIndex;
}

int _setUserPage(
	LinearMemoryManager *m, UserPageTable *p,
	void *linearAddress, PhysicalAddress physicalAddress
){
	int i1 = PD_INDEX((uintptr_t)linearAddress);
	int i2 = PT_INDEX((uintptr_t)linearAddress);
	if(isPDEPresent(p->pd->entry + i1) == 0){
		PhysicalAddress pt_physical = allocatePhysical(m, sizeof(PageTable));
		if(pt_physical.value == UINTPTR_NULL){
			return 0;
		}
		setPDE(p->pd->entry + i1, USER_WRITABLE_PAGE, pt_physical);

	}

	remapUserPageTable(m, p, i1);
	MEMSET0(p->pt);
	setPTE(p->pt->entry + i2, USER_WRITABLE_PAGE, physicalAddress, (uintptr_t)linearAddress);
	return 1;
}

void _invalidateUserPage(
	LinearMemoryManager *m, UserPageTable *p,
	void *linearAddress
){
	int i1 = PD_INDEX((uintptr_t)linearAddress);
	int i2 = PT_INDEX((uintptr_t)linearAddress);
	assert(isPDEPresent(p->pd->entry + i1));
	remapUserPageTable(m, p, i1);
	assert(isPTEPresent(p->pt->entry + i2));

	invalidatePTE(p->pt->entry, (uintptr_t)linearAddress); // TODO: invlpg ???
	if(0){
		//set PDE not present
		//release physical
	}
}
*/
