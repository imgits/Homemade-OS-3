#include"common.h"
#include"memory/memory.h"
#include"io/io.h"
#include"file.h"
#include"interrupt/systemcall.h"
#include"multiprocessor/processorlocal.h"

#pragma pack(1)

typedef struct __attribute__((__packed__)){
	uint8_t driveNumber;
	uint8_t reserved2;
	uint8_t signature; // 0x28 or 0x29
	uint32_t partitionSerialNumber;
	uint8_t partitionName[11];
	uint8_t fatName[8];
}FATExtBootRecord;

typedef struct __attribute__((__packed__)){
	FATExtBootRecord ext;
}FAT12BootRecord;

typedef struct __attribute((__packed__)){
	uint32_t sectorsPerFAT32;
	uint16_t flags; // bit 0~4: active FAT copies; bit 7: FAT mirroring disabled
	uint16_t version;
	uint32_t rootCluster;
	uint16_t fsInfoSector;
	uint16_t backupBootSector;
	uint8_t reserved[12];
	FATExtBootRecord ext;
}FAT32BootRecord;

typedef struct __attribute__((__packed__)){
	struct __attribute__((__packed__)){
		uint8_t jmp[3];
		uint8_t oemName[8];
		uint16_t bytesPerSector;
		uint8_t sectorsPerCluster;
		uint16_t reservedSectorCount;
		uint8_t fatCount;
		uint16_t rootEntryCount;
		uint16_t sectorCount;
		uint8_t mediaType;
		uint16_t sectorsPerFAT;
		uint16_t sectorsPerTrack;
		uint16_t headCount;
		uint32_t hiddenSectorCount;
		uint32_t SectorCount2;
	};
	union{
		struct{
			FAT12BootRecord ebr12;
			uint8_t fat12bootCode[448];
		};
		struct{
			FAT32BootRecord ebr32;
			uint8_t fat32BootCode[420];
		};
	};
	uint16_t bootSignature; // 0xaa55
}FATBootSector;

static_assert(sizeof(FATBootSector) == 512);

typedef struct __attribute__((__packed__)){
	uint8_t fileName[11];
	uint8_t attribute;
	uint8_t reserved;
	uint8_t createDecisecond;
	uint16_t createTime; // h:m:s = 5:6:5
	uint16_t createDate; // y:m:d = 7:4:5
	uint16_t accessDate;
	uint16_t clusterHigh;
	uint16_t modifyTime;
	uint16_t modifyDate;
	uint16_t clusterLow;
	uint32_t fileSize;
}FATDirEntry;

static_assert(sizeof(FATDirEntry) == 32);

typedef struct __attribute__((__packed__)){
	uint8_t position;
	uint16_t fileName0[5];
	uint8_t attribute;
	uint8_t type; // 0
	uint8_t checksum;
	uint16_t fileName5[6];
	uint16_t reserved; // 0
	uint16_t fileName11[2];
}LongFATDirEntry;

enum FATAttribute{
	FAT_READ_ONLY = 0x01,
	FAT_HIDDEN = 0x02,
	FAT_SYSTEM = 0x04,
	FAT_VOLUME_ID = 0x08,
	FAT_DIRECTORY = 0x10,
	FAT_ARCHIVE = 0x20,
	FAT_LONG_FILE_NAME = 0x0f
};

static_assert(sizeof(LongFATDirEntry) == 32);

#pragma pack()

static struct SlabManager *slab = NULL;

struct DiskParameter{
	uint64_t startLBA;
	uintptr_t diskFileHandle;
	uintptr_t sectorSize;
};

static uint32_t *loadFAT32(const FATBootSector *br, const struct DiskParameter *dp){
	const size_t fatSize = br->ebr32.sectorsPerFAT32 * br->bytesPerSector;
	const uint64_t fatBeginLBA = dp->startLBA + br->reservedSectorCount;
	const uintptr_t sectorsPerPage = PAGE_SIZE / dp->sectorSize;
	uint32_t *fat = systemCall_allocateHeap(CEIL(fatSize, PAGE_SIZE), KERNEL_NON_CACHED_PAGE);
	EXPECT(fat != NULL);
	unsigned p;
	for(p = 0; p * PAGE_SIZE < fatSize; p++){
		uintptr_t readSize = PAGE_SIZE;
		uintptr_t rwDisk = syncSeekReadFile(dp->diskFileHandle,
			(void*)(((uintptr_t)fat) + PAGE_SIZE * p), fatBeginLBA + p * sectorsPerPage, &readSize);
		if(rwDisk == IO_REQUEST_FAILURE || readSize != PAGE_SIZE){
			printk("warning: failed to read FAT\n"); //TODO
			break;
		}
	}
	EXPECT(p * PAGE_SIZE >= fatSize);
	return fat;
	ON_ERROR;
	systemCall_releaseHeap(fat);
	ON_ERROR;
	return NULL;
}

static void iterateDirectory(uint32_t beginClusterSector,
	uint32_t sectorsPerCluster, const struct DiskParameter *dp){
	const uintptr_t clusterSize = sectorsPerCluster * dp->sectorSize;
	FATDirEntry *rootDir = systemCall_allocateHeap(clusterSize, KERNEL_NON_CACHED_PAGE);
	EXPECT(rootDir != NULL);
	uintptr_t readSize = clusterSize;
	uintptr_t rwDisk = syncSeekReadFile(dp->diskFileHandle,
		rootDir, beginClusterSector, &readSize);
	EXPECT(rwDisk != IO_REQUEST_FAILURE && readSize == clusterSize);
	unsigned p;
	for(p = 0; p < clusterSize / sizeof(FATDirEntry); p++){
		if(rootDir[p].fileName[0] == 0)break;
		if(rootDir[p].fileName[0] == 0xe5)continue;
		int b;
		for(b = 0; b < 11; b++){
			printk("%c", rootDir[p].fileName[b]);
		}
		if(rootDir[p].attribute & FAT_DIRECTORY)
			printk(" (dir)");
		printk(" cluster %d %d size %d", rootDir[p].clusterHigh, rootDir[p].clusterLow, rootDir[p].fileSize);
		printk("\n");
	}
	systemCall_releaseHeap(rootDir);
	return;
	ON_ERROR;
	systemCall_releaseHeap(rootDir);
	ON_ERROR;
	printk("cannot iterate directory\n");
	return;
};

static struct DiskParameter *createFATPartition(uintptr_t fileHandle, uint64_t startLBA, uintptr_t sectorSize){
	struct DiskParameter *NEW(dp);
	EXPECT(dp != NULL);
	dp->diskFileHandle = fileHandle;
	dp->startLBA = startLBA;
	dp->sectorSize = sectorSize;
	const uintptr_t readSize = CEIL(sizeof(FATBootSector), dp->sectorSize);
	FATBootSector *br = systemCall_allocateHeap(readSize, KERNEL_NON_CACHED_PAGE);
	EXPECT(br != NULL);
	MEMSET0(br);
	uintptr_t actualReadSize = readSize;
	uintptr_t rwDisk = syncSeekReadFile(dp->diskFileHandle,
		br, dp->startLBA, &actualReadSize);
	EXPECT(rwDisk != IO_REQUEST_FAILURE && actualReadSize == readSize);
	EXPECT(br->ebr32.ext.signature == 0x28 || br->ebr32.ext.signature == 0x29);

	//const uint32_t sectorsPerFAT32 = br->ebr32.sectorsPerFAT32;
	//uint32_t bytesPerCluster = br->sectorsPerCluster * br->bytesPerSector;
	//printk("%x %x %x %x\n",br->ebr32.rootCluster, fatBeginLBA, sectorsPerFAT32, bytesPerCluster);

	uint32_t *fat = loadFAT32(br, dp);
	EXPECT(fat != NULL);
	//const unsigned fatEntryCount = br->ebr32.sectorsPerFAT32 * (sectorSize / 4);
	//unsigned p;
	const uint64_t rootClusterSector = dp->startLBA +
		br->reservedSectorCount + br->ebr32.sectorsPerFAT32 * br->fatCount +
		(br->ebr32.rootCluster-2) * br->sectorsPerCluster;
	iterateDirectory(rootClusterSector, br->sectorsPerCluster, dp);
	printk("read fat ok\n");
	systemCall_releaseHeap(fat);
	systemCall_releaseHeap((void*)br);
	return dp;
	//systemCall_releaseHeap(br);
	ON_ERROR;
	ON_ERROR;
	ON_ERROR;
	systemCall_releaseHeap((void*)br);
	ON_ERROR;
	ON_ERROR;
	printk("warning: read FAT32 failed\n");
	return NULL;
}

#define FAT32_SERVICE_NAME "fat32"
// TODO:
static IORequest *openFAT(
	__attribute__((__unused__)) const char *fileName, __attribute__((__unused__)) uintptr_t nameLength){
	return NULL;
}

void fatService(void){
	slab = createUserSlabManager(); // move it to user library
	uintptr_t discoverFAT = systemCall_discoverDisk(MBR_FAT32);
	assert(discoverFAT != IO_REQUEST_FAILURE);
	//int diskDriver;
	uintptr_t startLBALow;
	uintptr_t startLBAHigh;
	//uintptr_t diskCode;
	//uintptr_t sectorSize;
	if(addFileSystem(openFAT, "fat", strlen("fat")) != 1){
		printk("add file system failure\n");
	}
	while(1){
		uintptr_t diskFileHandle, sectorSize;
		uintptr_t discoverFAT2 = systemCall_waitIOReturn(
			discoverFAT, 4,
			&startLBALow, &startLBAHigh, &diskFileHandle, &sectorSize);
		if(discoverFAT != discoverFAT2){
			printk("discover disk failure\n");
			continue;
		}
		struct DiskParameter *dp = createFATPartition(diskFileHandle, COMBINE64(startLBALow, startLBAHigh), sectorSize);
		if(dp == NULL){
			continue;
		}//TODO:
	}
	printk("too many fat systems\n");
	while(1){
		sleep(1000);
	}
	panic("cannot initialize FAT32 service");
}
