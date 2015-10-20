#include<common.h>
#include"memory_private.h"
#include"multiprocessor/spinlock.h"

typedef uint16_t sizeorder_t;

typedef struct MemoryBlock{
	sizeorder_t sizeOrder;
	uint16_t unused;
	struct MemoryBlock**prev, *next;
}MemoryBlock;

typedef struct MemoryBlockManager{
	Spinlock lock;
	uintptr_t beginAddress;
	size_t blockStructSize;
	size_t blockStructOffset;
	int blockCount;
	size_t freeSize;
	MemoryBlock *freeBlock[MAX_BLOCK_ORDER - MIN_BLOCK_ORDER + 1];

	uint8_t blockArray[0];
}MemoryBlockManager;

void initMemoryBlock(MemoryBlock *voidMB);

void *indexToElement(MemoryBlockManager *m, int index);
void *blockToElement(MemoryBlockManager *m, const MemoryBlock *mb);
void *addressToElement(MemoryBlockManager *m, uintptr_t address);

uintptr_t elementToAddress(MemoryBlockManager *m, void *element);
uintptr_t blockToAddress(MemoryBlockManager *m, MemoryBlock *mb);
MemoryBlock *addressToBlock(MemoryBlockManager *m, uintptr_t address);

// return NULL if the block has no buddy (is the last block in the array)
MemoryBlock *getBuddy(MemoryBlockManager *m, const MemoryBlock *b);

size_t ceilAllocateOrder(size_t s);

size_t getFreeBlockSize(MemoryBlockManager *m);
int isAddressInRange(MemoryBlockManager *m, uintptr_t address);

// return NULL if fail
MemoryBlock *allocateBlock_noLock(MemoryBlockManager *m, size_t *size);
void releaseBlock_noLock(MemoryBlockManager *m, MemoryBlock *b);

typedef void(*InitMemoryBlockFunction)(void*);

void resetBlockArray(MemoryBlockManager *bm, int initialBlockCount, InitMemoryBlockFunction initBlockFunc);

void initMemoryBlockManager(
	MemoryBlockManager *bm,
	size_t blockStructSize,
	size_t blockStructOffset,
	uintptr_t beginAddr,
	uintptr_t endAddr,
	InitMemoryBlockFunction initBlockFunc
);

uintptr_t evaluateMemoryBlockManagerEnd(
	const MemoryBlockManager *bm,
	size_t blockStructSize,
	uintptr_t beginAddr,
	uintptr_t endAddr
);
