#include"common.h"
#include"memory.h"
#include"memory/page.h"
#include"assembly/assembly.h"
#include"multiprocessor/spinlock.h"
#include"memory_private.h"

// BIOS address range functions

enum AddressRangeType{
	USABLE = 1,
	RESERVED = 2,
	ACPI_RECLAIMABLE = 3,
	ACPI_NVS = 4,
	BAD_MEMORY = 5
};
static_assert(sizeof(enum AddressRangeType) == 4);

typedef struct AddressRange{
	uint64_t base;
	uint64_t size;
	enum AddressRangeType type;
	uint32_t extra;
}AddressRange;
static_assert(sizeof(AddressRange) == 24);

extern const AddressRange *const addressRange;
extern const int addressRangeCount;

#define OS_MAX_ADDRESS (((uintptr_t)0xffffffff) - ((uintptr_t)0xffffffff) % MIN_BLOCK_SIZE)
static_assert(OS_MAX_ADDRESS % MIN_BLOCK_SIZE == 0);

static uintptr_t findMaxAddress(void){
	int i;
	uint64_t maxAddr = 0;
	// kprintf("%d memory address ranges\n", addressRangeCount);
	for(i = 0; i < addressRangeCount; i++){
		const struct AddressRange *ar = addressRange + i;
		/*
		printk("type: %d base: %x %x size: %x %x\n", ar->type,
		(uint32_t)(ar->base >> 32), (uint32_t)(ar->base & 0xffffffff),
		(uint32_t)(ar->size >> 32), (uint32_t)(ar->size & 0xffffffff));
		 */
		if(ar->type == USABLE && ar->size != 0 &&
			maxAddr < ar->base + ar->size - 1){
			maxAddr = ar->base + ar->size - 1;
		}
	}
	if(maxAddr >= OS_MAX_ADDRESS){
		return OS_MAX_ADDRESS;
	}
	return maxAddr + 1;
}
/*
static uintptr_t findFirstUsableMemory(const size_t manageSize){
	int i;
	uintptr_t manageBase = OS_MAX_ADDRESS;
	for(i = 0; i < addressRangeCount; i++){
		const AddressRange *ar = addressRange + i;
		if(
		ar->type == USABLE &&
		ar->base <= OS_MAX_ADDRESS - manageSize && // address < 4GB
		ar->base >= (1 << 20) && // address >= 1MB
		ar->size >= manageSize &&
		ar->base < manageBase
		){
			manageBase = (uintptr_t)ar->base;
		}
	}
	if(manageBase == OS_MAX_ADDRESS){
		panic("no memory for memory manager");
	}
	return manageBase;
}
*/

// memory manager collection

PhysicalAddress _allocatePhysicalPages(MemoryBlockManager *physical, size_t size){
	size_t p_size = size;
	PhysicalAddress p_address = {allocateBlock(physical, &p_size)};
	return p_address;
}

void _releasePhysicalPages(MemoryBlockManager *physical, PhysicalAddress address){
	releaseBlock(physical, address.value);
}

void *_mapPage_P(LinearMemoryManager *m, PhysicalAddress physicalAddress, size_t size){
	// linear
	size_t l_size = size;
	void *linearAddress = (void*)allocateBlock(m->linear, &l_size);
	EXPECT(linearAddress != NULL);
	assert(((uintptr_t)linearAddress) % PAGE_SIZE == 0);
	// assume linear memory manager and page table are consistent
	// linearAddess[0 ~ l_size] are guaranteed to be available
	// it is safe to map pages of l_size
	int result = _mapPage_LP(m->page, m->physical, linearAddress, physicalAddress, l_size);
	EXPECT(result == 1);

	return linearAddress;

	ON_ERROR;
	releaseBlock(m->linear, (uintptr_t)linearAddress);
	ON_ERROR;
	return NULL;
}

void _unmapPage_P(LinearMemoryManager *m, void *l_address){
	size_t s = getAllocatedBlockSize(m->linear, (uintptr_t)l_address);
	_unmapPage_LP(m->page, m->physical, l_address, s);
	releaseBlock(m->linear, (uintptr_t)l_address);
}

void *_allocateAndMapPages(LinearMemoryManager *m, size_t size){
	// linear
	size_t l_size = size;
	void *linearAddress = (void*)allocateBlock(m->linear, &l_size);
	EXPECT(linearAddress != NULL);
	// physical
	int result = _mapPage_L(m->page, m->physical, linearAddress, l_size);
	EXPECT(result == 1);

	return linearAddress;

	ON_ERROR;
	releaseBlock(m->linear, (uintptr_t)linearAddress);
	ON_ERROR;
	return NULL;
}

void _unmapAndReleasePages(LinearMemoryManager *m, void* l_address){
	size_t s = getAllocatedBlockSize(m->linear, (uintptr_t)l_address);
	_unmapPage_L(m->page, m->physical, l_address, s);
	releaseBlock(m->linear, (uintptr_t)l_address);
}

// global memory manager
static LinearMemoryManager kernelMemory;
static SlabManager *kernelSlab = NULL;

void *allocateKernelMemory(size_t size){
	assert(kernelSlab != NULL);
	return allocateSlab(kernelSlab, size);
}

void releaseKernelMemory(void *address){
	assert(kernelSlab != NULL);
	releaseSlab(kernelSlab, address);
}

PhysicalAddress allocatePhysicalPages(size_t size){
	return _allocatePhysicalPages(kernelMemory.physical, size);
}
void releasePhysicalPages(PhysicalAddress address){
	_releasePhysicalPages(kernelMemory.physical, address);
}

void *mapPageToPhysical(PhysicalAddress address, size_t size){
	assert(size % PAGE_SIZE == 0);
	assert(kernelMemory.page != NULL && kernelMemory.linear != NULL);
	return _mapPage_P(&kernelMemory, address, size);
}

void unmapPageToPhysical(void *linearAddress){
	assert(kernelMemory.page != NULL && kernelMemory.linear != NULL);
	_unmapPage_P(&kernelMemory, linearAddress);
}

int mapExistingPages(
	PageManager *dst, PageManager *src,
	uintptr_t dstLinear, uintptr_t srcLinear, size_t size
){
	return _mapExistingPages(kernelMemory.physical, dst, src, dstLinear, srcLinear, size);
}

void *allocateAndMapPages(size_t size){
	return _allocateAndMapPages(&kernelMemory, size);
}
void unmapAndReleasePages(void *linearAddress){
	_unmapAndReleasePages(&kernelMemory, linearAddress);
}

/*
size_t getPhysicalMemoryUsage(){

}

size_t getKernelMemoryUsage(){

}
*/

int mapPage_L(PageManager *p, void *linearAddress, size_t size){
	return _mapPage_L(p, kernelMemory.physical, linearAddress, size);
}
void unmapPage_L(PageManager *p, void *linearAddress, size_t size){
	_unmapPage_L(p, kernelMemory.physical, linearAddress, size);
}
int mapPage_LP(PageManager *p, void *linearAddress, PhysicalAddress physicalAddress, size_t size){
	return _mapPage_LP(p, kernelMemory.physical, linearAddress, physicalAddress, size);
}
void unmapPage_LP(PageManager *p, void *linearAddress, size_t size){
	return _unmapPage_LP(p, kernelMemory.physical, linearAddress, size);
}


/*
UserPageTable *mapUserPageTable(PhysicalAddress p){
	return _mapUserPageTable(&kernelMemory, kernelSlab, p);
}
PhysicalAddress unmapUserPageTable(UserPageTable *p){
	return _unmapUserPageTable(&kernelMemory, kernelSlab, p);
}
UserPageTable *createUserPageTable(void){
	return _createUserPageTable(&kernelMemory, kernelSlab);
}
void deleteUserPageTable(UserPageTable *p){
	_deleteUserPageTable(&kernelMemory, kernelSlab, p);
}
*/
static void initUsableBlocks(MemoryBlockManager *m,
	const AddressRange *arArray1, int arLength1,
	const AddressRange *arArray2, int arLength2
){
	int b;
	const int bCount = getBlockCount(m);
	const uintptr_t firstBlockAddress = getFirstBlockAddress(m);
	for(b = 0; b < bCount; b++){
		uintptr_t blockAddress = firstBlockAddress + b * MIN_BLOCK_SIZE;
		assert(blockAddress + MIN_BLOCK_SIZE > blockAddress);
		int isInUnusable = 0, isInUsable = 0;
		int i;
		for(i = 0; isInUnusable == 0 && i < arLength1 + arLength2; i++){
			const AddressRange *ar = (i < arLength1? arArray1 + i: arArray2 + (i - arLength1));
			if( // completely covered by usable range
				ar->type == USABLE &&
				(ar->base <= blockAddress && ar->base + ar->size >= blockAddress + MIN_BLOCK_SIZE)
			){
				isInUsable = 1;
			}
			if( // partially covered by unusable range
				ar->type != USABLE &&
				!(ar->base >= blockAddress + MIN_BLOCK_SIZE || ar->base + ar->size <= blockAddress)
			){
				isInUnusable = 1;
			}
		}
		if(isInUnusable == 0 && isInUsable == 1){
			releaseBlock(m, blockAddress);
		}
	}
}

// kernel MemoryBlockManager
static MemoryBlockManager *initKernelPhysicalBlock(
	uintptr_t manageBase, uintptr_t manageBegin, uintptr_t manageEnd,
	uintptr_t minAddress, uintptr_t maxAddress
){
	MemoryBlockManager *m = createMemoryBlockManager(manageBegin, manageEnd - manageBegin, minAddress, maxAddress);
	const AddressRange extraAR[1] = {
		{manageBase - KERNEL_LINEAR_BEGIN, manageEnd - manageBase, RESERVED, 0}
	};
	initUsableBlocks(m, addressRange, addressRangeCount, extraAR, LENGTH_OF(extraAR));

	return m;
}

static MemoryBlockManager *initKernelLinearBlock(
	uintptr_t manageBase, uintptr_t manageBegin, uintptr_t manageEnd,
	uintptr_t minAddress, uintptr_t maxAddress
){
	MemoryBlockManager *m = createMemoryBlockManager(manageBegin, manageEnd - manageBegin, minAddress, maxAddress);
	const AddressRange extraAR[2] = {
		{manageBase, manageEnd - manageBase, RESERVED, 0},
		{minAddress, maxAddress, USABLE, 0}
	};
	initUsableBlocks(m, extraAR, 0, extraAR, LENGTH_OF(extraAR));

	return m;
}

#ifdef NDEBUG
#define testMemoryManager() do{}while(0)
#else
#define TEST_N (60)
static void testMemoryManager(void){
	uint8_t *p[TEST_N];
	int si[TEST_N];
	unsigned int r;
	int a, b, c;
	r=MIN_BLOCK_SIZE + 351;
	for(b=0;b<10;b++){
		for(a=0;a<TEST_N;a++){
			si[a]=r;
			p[a]=allocateKernelMemory(r);
			if(p[a] == NULL){
				// printk("a = %d, r = %d p[a] = %x\n", a, r, p[a]);
			}
			else{
				for(c=0;c<si[a]&&c<100;c++){
					p[a][c] =
					p[a][si[a]-c-1]= a+1;
				}
			}
			//r = 1 + (r*7 + 3) % (30 - 1);
			r = (r*79+3);
			if(r%5<3) r = r % 2048;
			else r = (r*17) % (MAX_BLOCK_SIZE - MIN_BLOCK_SIZE) + MIN_BLOCK_SIZE;
		}
		for(a=0;a<TEST_N;a++){
			int a2 = (a+r)%TEST_N;
			if(p[a2]==NULL)continue;
			for(c=0;c<si[a2]&&c<100;c++){
				if(p[a2][c] != a2+1 || p[a2][si[a2]-c-1] != a2+1){
					//printk("%x %x %d %d %d %d\n", p[a2], p[p[a2][c]-1],si[p[a2][c]-1], p[a2][c], p[a2][si[a2]-c-1], a2+1);
					panic("memory test failed");
				}
			}
			releaseKernelMemory((void*)p[a2]);
		}
	}
	//printk("test memory: ok\n");
	//printk("%x %x %x %x %x\n",a1,a2,a3, MIN_BLOCK_SIZE+(uintptr_t)a3,a4);
}
#endif

void initKernelMemory(void){
	assert(kernelMemory.linear == NULL && kernelMemory.page == NULL && kernelMemory.physical == NULL);
	// reserved... are linear address
	const uintptr_t reservedBase = KERNEL_LINEAR_BEGIN;
	uintptr_t reservedBegin = reservedBase + (1 << 20);
	uintptr_t reservedDirectMapEnd = reservedBase + (14 << 20);
	uintptr_t reservedEnd = reservedBase + (16 << 20);
	kernelMemory.physical = initKernelPhysicalBlock(
		reservedBase, reservedBegin, reservedDirectMapEnd,
		0, findMaxAddress()
	);
	reservedBegin = ((uintptr_t)kernelMemory.physical) + getBlockManagerMetaSize(kernelMemory.physical);

	kernelMemory.linear = initKernelLinearBlock(
		reservedBase, reservedBegin, reservedEnd,
		KERNEL_LINEAR_BEGIN, KERNEL_LINEAR_END
	);
	reservedBegin = ((uintptr_t)kernelMemory.linear) + getBlockManagerMetaSize(kernelMemory.linear);
	kernelMemory.page = initKernelPageTable(
		reservedBase, reservedBegin, reservedEnd,
		KERNEL_LINEAR_BEGIN, KERNEL_LINEAR_END
	);

	kernelSlab = createSlabManager(&kernelMemory);

	testMemoryManager();

}
