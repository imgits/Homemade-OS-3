#include"common.h"
#include"memory/memory.h"
#include"io/io.h"
#include"file.h"
#include"interrupt/systemcall.h"
#include"task/semaphore.h"
#include"task/task.h"
#include"multiprocessor/processorlocal.h"
#include"multiprocessor/spinlock.h"

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
	uint32_t rootCluster; // first data cluster. typically = 2
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

//static struct SlabManager *slab = NULL;

typedef struct FAT32DiskPartition{
	uintptr_t diskFileHandle;
	uint64_t startLBA;
	uint64_t firstDataLBA;
	uintptr_t sectorSize;
	char partitionName;
	const FATBootSector *bootRecord;
	uint32_t *fat;

	struct FAT32DiskPartition **prev, *next;
}FAT32DiskPartition;

static void initRootDirEntry(FATDirEntry *d, uint32_t rootCluster){
	MEMSET0(d);
	d->clusterLow = ((rootCluster) & 0xffff);
	d->clusterHigh = ((rootCluster >> 16) & 0xffff);
	d->attribute = FAT_DIRECTORY;
}

static uint32_t getBeginCluster(const FATDirEntry *d){
	return ((uint32_t)d->clusterLow) + (((uint32_t)d->clusterHigh) << 16);
}

static uintptr_t getClusterSize(const FAT32DiskPartition *dp){
	return dp->bootRecord->sectorsPerCluster * dp->sectorSize;
}

static uint64_t clusterToLBA(const FAT32DiskPartition *dp, uint32_t cluster){
	return dp->firstDataLBA + (cluster - 2) * (uint64_t)dp->bootRecord->sectorsPerCluster;
}

static uint32_t *loadFAT32(const FATBootSector *br, const FAT32DiskPartition *dp){
	const size_t fatSize = br->ebr32.sectorsPerFAT32 * br->bytesPerSector; // what is the actual length of fat?
	const uint64_t fatBeginLBA = dp->startLBA + br->reservedSectorCount;
	const uintptr_t sectorsPerPage = PAGE_SIZE / dp->sectorSize;
	//TODO: ahci driver accepts non-aligned buffer
	uint32_t *fat = systemCall_allocateHeap(CEIL(fatSize, PAGE_SIZE), KERNEL_NON_CACHED_PAGE);
	EXPECT(fat != NULL);
	unsigned p;
	for(p = 0; p * PAGE_SIZE < fatSize; p++){
		uintptr_t readSize = PAGE_SIZE;
		uintptr_t rwDisk = syncSeekReadFile(dp->diskFileHandle,
			(void*)(((uintptr_t)fat) + PAGE_SIZE * p), fatBeginLBA + p * sectorsPerPage, &readSize);
		if(rwDisk == IO_REQUEST_FAILURE || readSize != PAGE_SIZE){
			printk("warning: failed to read FAT\n");
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

#define END_OF_CLUSTER (0x0ffffff8)
#define BAD_CLUSTER (0x0ffffff7)

static uint32_t nextClusterByFAT(uint32_t cluster, const FAT32DiskPartition *dp){
	return dp->fat[cluster];
}

static int isValidCluster(uint32_t cluster, const FAT32DiskPartition *dp){
	const uint32_t clustersPerFAT = (dp->bootRecord->ebr32.sectorsPerFAT32 * dp->bootRecord->bytesPerSector) / sizeof(dp->fat[0]);
	if(cluster >= END_OF_CLUSTER || cluster >= clustersPerFAT) // end of cluster chain
		return 0;
	if(dp->fat[cluster] == BAD_CLUSTER)
		return 0;
	return 1;
}

#undef BAD_CLUSTER
#undef END_OF_CLUSTER

static uint32_t countClusterByFAT(uint32_t cluster, const FAT32DiskPartition *dp){
	uint32_t clusterCount;
	for(clusterCount = 0; isValidCluster(cluster, dp); clusterCount++){
		cluster = nextClusterByFAT(cluster, dp);
	}
	return clusterCount;
}

// currently not support long file name
#define FAT_SHORT_NAME_LENGTH (11)

static int toFATFileName(char *newName, const char *name, uintptr_t length){
	uintptr_t i;
	for(i = length - 1; i > 0 && name[i] != '.'; i--){ // find last dot
	}
	uintptr_t mainLen, ext;
	if(name[i] == '.'){
		mainLen = i;
		ext = i + 1;
	}
	else{
		mainLen = ext = length;
	}
	if(mainLen > 8 || length - ext > 3)
		return 0;
	for(i = 0; i < FAT_SHORT_NAME_LENGTH; i++)
		newName[i] = ' ';
	for(i = 0 ; i < mainLen; i++)
		newName[i] = toupper(name[i]);
	for(i = 0; i < length - ext; i++)
		newName[8 + i] = toupper(name[ext + i]);
	return 1;
}

static FATDirEntry *searchDirectory(
	FATDirEntry *dir, uintptr_t dirLength,
	const char *name, uintptr_t length
){
	char formattedName[FAT_SHORT_NAME_LENGTH];
	toFATFileName(formattedName, name, length);
	FATDirEntry *e = NULL;
	unsigned p;
	for(p = 0; p < dirLength; p++){
		if(dir[p].fileName[0] == 0) // end of directory
			break;
		if(dir[p].fileName[0] == 0xe5) // empty entry
			continue;
		if(strncmp((const char*)dir[p].fileName, formattedName, FAT_SHORT_NAME_LENGTH) == 0){
			e = dir + p;
		}
		/*
		int b;
		for(b = 0; b < 11; b++){
			printk("%c", dir[p].fileName[b]);
		}
		printk(" attr %x",dir[p].attribute);
		printk(" cluster %x %x size %d", dir[p].clusterHigh, dir[p].clusterLow, dir[p].fileSize);
		printk("\n");
		*/
	}
	return e;
}

static FAT32DiskPartition *createFATPartition(uintptr_t fileHandle, uint64_t startLBA, uintptr_t sectorSize, char partitionName){
	FAT32DiskPartition *NEW(dp);
	EXPECT(dp != NULL);
	dp->diskFileHandle = fileHandle;
	dp->startLBA = startLBA;
	dp->sectorSize = sectorSize;
	dp->partitionName = partitionName;
	const uintptr_t readSize = CEIL(sizeof(FATBootSector), dp->sectorSize);
	//TODO: dp->bootRecord is in user space, and thus not accessible in interrupt handler
	FATBootSector *br = systemCall_allocateHeap(readSize, KERNEL_NON_CACHED_PAGE);
	EXPECT(br != NULL);
	dp->bootRecord = br;
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
	dp->fat = fat;
	//const unsigned fatEntryCount = br->ebr32.sectorsPerFAT32 * (sectorSize / 4);
	//unsigned p;
	dp->firstDataLBA = dp->startLBA +
		(uint64_t)br->reservedSectorCount + br->ebr32.sectorsPerFAT32 * (uint64_t)br->fatCount;
	//printk("read fat ok\n");
	dp->prev = NULL;
	dp->next = NULL;
	return dp;
	//systemCall_releaseHeap(fat);
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

struct FAT32DiskPartitionList{
	Task *mainTask;
	FAT32DiskPartition *head;
	Spinlock lock;
}fat32List = {NULL, NULL, INITIAL_SPINLOCK};

static FAT32DiskPartition *searchFAT32DiskPartition(const char *fileName, uintptr_t *index, uintptr_t nameLength){
	EXPECT(nameLength == 1 || (nameLength > 1 && fileName[1] == '/'));
	FAT32DiskPartition *f;
	acquireLock(&fat32List.lock);
	for(f = fat32List.head; f != NULL; f = f->next){
		if(f->partitionName == fileName[0])
			break;
	}
	releaseLock(&fat32List.lock);
	EXPECT(f != NULL);
	*index = 1;
	return f;
	ON_ERROR;
	ON_ERROR;
	return NULL;
}

static void addFAT32DiskPartition(FAT32DiskPartition *dp){
	acquireLock(&fat32List.lock);
	ADD_TO_DQUEUE(dp, &fat32List.head);
	releaseLock(&fat32List.lock);
}

// FAT file

typedef struct FATFile{
	// search key
	uint32_t beginCluster;
	const FAT32DiskPartition *diskPartition;
	// lock
	ReaderWriterLock *rwLock;
	int referenceCount;

	struct FATFile *next, **prev;
}FATFile;

struct FATFileList{
	FATFile *head;
	Spinlock lock;
}fatFileList = {NULL, INITIAL_SPINLOCK};

static FATFile *searchCreateFATFile(const FAT32DiskPartition *dp, uint32_t cluster, int refCnt){
	acquireLock(&fatFileList.lock);
	FATFile *ff;
	for(ff = fatFileList.head; ff != NULL; ff = ff->next){
		if(ff->beginCluster == cluster && ff->diskPartition == dp)
			break;
	}
	while(ff != NULL){
		ff->referenceCount += refCnt;
		releaseLock(&fatFileList.lock);
		return ff;
	}
	NEW(ff);
	if(ff == NULL){
		releaseLock(&fatFileList.lock);
		return NULL;
	}
	ff->beginCluster = cluster;
	ff->rwLock = createReaderWriterLock(1);
	if(ff->rwLock == NULL){
		releaseLock(&fatFileList.lock);
		DELETE(ff);
		return NULL;
	}
	ff->referenceCount = refCnt;
	ff->diskPartition = dp;
	ff->next = NULL;
	ff->prev = NULL;
	ADD_TO_DQUEUE(ff, &fatFileList.head);
	releaseLock(&fatFileList.lock);
	return ff;
}

static int addFATFileReference(FATFile *ff, int refCnt){
	acquireLock(&fatFileList.lock);
	ff->referenceCount += refCnt;
	int r = ff->referenceCount;
	int needDelete = (r == 0);
	if(needDelete){
		REMOVE_FROM_DQUEUE(ff);
	}
	releaseLock(&fatFileList.lock);
	if(needDelete){
		deleteReaderWriterLock(ff->rwLock);
		DELETE(ff);
	}
	return r;
}

typedef struct{
	OpenFileRequest ofr;
	uint32_t offset;
	FATDirEntry dirEntry;
	FATFile *shared;
}OpenedFATFile;

static OpenedFATFile *createOpenedFATFile(const FileFunctions *ff,
	const FAT32DiskPartition *dp, const FATDirEntry *dir){
	OpenedFATFile *NEW(f);
	EXPECT(f != NULL);
	initOpenFileRequest(&f->ofr, f, ff);
	f->offset = 0;
	f->dirEntry = (*dir);
	f->shared = searchCreateFATFile(dp, getBeginCluster(dir), 1);
	EXPECT(f->shared != NULL);
	return f;
	//addFATFileReference(f->shared, -1);
	ON_ERROR;
	DELETE(f);
	ON_ERROR;
	return NULL;
}

static void deleteOpenedFATFile(OpenedFATFile *f){
	addFATFileReference(f->shared, -1);
	DELETE(f);
}

// openFAT

typedef struct{
	uintptr_t nameLength;
	OpenFileMode mode;
	OpenedFATFile *file; // NULL if failed to open
	OpenFileManager *fileManager;
	IORequest ior;
	char fileName[];
}OpenFATRequest;

static int finishOpenFAT(IORequest *ior, uintptr_t *returnValues){
	OpenFATRequest *ofr = ior->instance;
	returnValues[0] = (ofr->file == NULL? IO_REQUEST_FAILURE: getFileHandle(&ofr->file->ofr));
	DELETE(ofr);
	return 1;
}

static void openFATTask(void *p);

static IORequest *openFAT(const char *fileName, uintptr_t nameLength, OpenFileMode mode){
	OpenFATRequest *ofr = allocateKernelMemory(sizeof(*ofr) + nameLength);
	EXPECT(ofr != NULL);
	initIORequest(&ofr->ior, ofr, notSupportCancelIORequest, finishOpenFAT);
	strncpy(ofr->fileName, fileName, nameLength);
	ofr->nameLength = nameLength;
	ofr->mode = mode;
	ofr->file = NULL;
	ofr->fileManager = getOpenFileManager(processorLocalTask());
	Task *t = createSharedMemoryTask(openFATTask, &ofr, sizeof(ofr), fat32List.mainTask);
	EXPECT(t != NULL);
	pendIO(&ofr->ior);
	resume(t);
	return &ofr->ior;
	// delete task
	ON_ERROR;
	ON_ERROR;
	return IO_REQUEST_FAILURE;
}

// readFAT

typedef struct{
	uintptr_t inputRWSize, outputRWSize;
	uintptr_t bufferOffset;
	PhysicalAddressArray *pa;
	OpenedFATFile *file;
	IORequest ior;
}RWFATRequest;

static int finishRWFAT(IORequest* ior, uintptr_t *returnValues){
	RWFATRequest *f = ior->instance;
	returnValues[0] = f->outputRWSize;
	deletePhysicalAddressArray(f->pa);
	DELETE(f);
	return 1;
}

static void rwFATTask(void *rwfrPtr);

static IORequest *readFAT(OpenFileRequest *ofr, uint8_t *buffer, uintptr_t readSize){
	RWFATRequest *NEW(rwfr);
	EXPECT(rwfr != NULL);
	initIORequest(&rwfr->ior, rwfr, notSupportCancelIORequest, finishRWFAT);
	rwfr->file = ofr->instance;
	rwfr->inputRWSize = readSize;
	rwfr->outputRWSize = 0;
	rwfr->pa = reserveBufferPages(buffer, readSize, &rwfr->bufferOffset);
	EXPECT(rwfr->pa != NULL);
	Task *t = createSharedMemoryTask(rwFATTask, &rwfr, sizeof(rwfr), fat32List.mainTask);
	EXPECT(t != NULL);
	pendIO(&rwfr->ior);
	resume(t);
	return &rwfr->ior;
	// delete task
	ON_ERROR;
	deletePhysicalAddressArray(rwfr->pa);
	ON_ERROR;
	DELETE(rwfr);
	ON_ERROR;
	return IO_REQUEST_FAILURE;
}

static uintptr_t readByFAT(const FAT32DiskPartition *dp, void *buffer, uint32_t beginCluster,
	uint32_t readOffset, uint32_t readSize){
	const uint32_t readFileEnd = readOffset + readSize;
	const uintptr_t clusterSize = getClusterSize(dp);
	void *clusterBuffer = systemCall_allocateHeap(clusterSize, USER_NON_CACHED_PAGE);
	EXPECT(clusterBuffer != NULL);
	uint32_t fileIndex = 0;
	uintptr_t bufferIndex = 0;
	uint32_t cluster = beginCluster;
	while(fileIndex < readFileEnd && isValidCluster(cluster, dp)){
		if(/*fileIndex < readFileEnd &&*/fileIndex + clusterSize > readOffset){
			// read next cluster to buffer1
			uintptr_t readDiskSize = clusterSize;
			uintptr_t ret = syncSeekReadFile(dp->diskFileHandle, clusterBuffer, clusterToLBA(dp, cluster), &readDiskSize);
			if(readDiskSize != clusterSize || ret == IO_REQUEST_FAILURE)
				break;
			// copy clusterBuffer to buffer
			uintptr_t copyBegin = (fileIndex > readOffset ? fileIndex: readOffset);
			uintptr_t copyEnd = (fileIndex + clusterSize < readFileEnd? fileIndex + clusterSize: readFileEnd);
			memcpy((void*)(((uintptr_t)buffer) + bufferIndex),
				(const void*)(((uintptr_t)clusterBuffer) + copyBegin % clusterSize), copyEnd - copyBegin);
			bufferIndex += copyEnd - copyBegin;
		}
		fileIndex += clusterSize;
		cluster = nextClusterByFAT(cluster, dp);
	}

	systemCall_releaseHeap(clusterBuffer);
	return bufferIndex;

	// systemCall_releaseHeap(clusterBuffer);
	ON_ERROR;
	return 0;
}

static void rwFATTask(void *rwfrPtr){
	LinearMemoryManager *lm = getTaskLinearMemory(processorLocalTask());
	RWFATRequest *rwfr = *(RWFATRequest**)rwfrPtr;
	OpenedFATFile *f = rwfr->file;
	void *bufferPage2 = mapReservedPages(lm, rwfr->pa, USER_WRITABLE_PAGE);
	EXPECT(bufferPage2 != NULL);
	acquireReaderLock(f->shared->rwLock);
	// acquire semaphore or rwlock
	uint32_t readFileBegin = f->offset;
	uint32_t readFileSize;
	if(rwfr->inputRWSize > f->dirEntry.fileSize - f->offset){
		readFileSize = f->dirEntry.fileSize - f->offset;
	}
	else{
		readFileSize = rwfr->inputRWSize;
	}
	f->offset += readFileSize;
	rwfr->outputRWSize = readByFAT(f->shared->diskPartition,
		(void*)(((uintptr_t)bufferPage2) + rwfr->bufferOffset),
		f->shared->beginCluster, readFileBegin, readFileSize);
	releaseReaderWriterLock(f->shared->rwLock);
	EXPECT(rwfr->outputRWSize == readFileSize);

	unmapPages(lm, bufferPage2);
	finishIO(&rwfr->ior);
	systemCall_terminate();

	ON_ERROR;
	unmapPages(lm, bufferPage2);
	ON_ERROR;
	finishIO(&rwfr->ior);
	systemCall_terminate();
}

// sizeOfFAT

typedef struct{
	uint32_t sizeOfFile;
	IORequest ior;
}SizeOfFATRequest;

static int finishSizeOfFAT(IORequest *ior, uintptr_t *returnValues){
	SizeOfFATRequest *sofr = ior->instance;
	returnValues[0] = sofr->sizeOfFile;
	returnValues[1] = 0;
	DELETE(sofr);
	return 2;
}

static IORequest *sizeOfFAT(OpenFileRequest *ofr){
	OpenedFATFile *f = ofr->instance;
	SizeOfFATRequest *NEW(sofr);
	if(sofr == NULL){
		return IO_REQUEST_FAILURE;
	}
	initIORequest(&sofr->ior, sofr, notSupportCancelIORequest, finishSizeOfFAT);
	sofr->sizeOfFile = f->dirEntry.fileSize;
	pendIO(&sofr->ior);
	finishIO(&sofr->ior);
	return &sofr->ior;
}

// closeFAT

typedef struct{
	OpenedFATFile *file;
	OpenFileManager *fileManager;
	IORequest ior;
}CloseFATRequest;

static int finishCloseFAT(IORequest *ior, __attribute__((__unused__)) uintptr_t *returnValues){
	CloseFATRequest *cfr = ior->instance;
	removeFromOpenFileList(cfr->fileManager, &cfr->file->ofr);
	deleteOpenedFATFile(cfr->file);
	DELETE(cfr);
	return 0;
}

static IORequest *closeFAT(OpenFileRequest *ofr){
	OpenedFATFile *f = ofr->instance;
	// TODO: rwlock. if there are pending io request, wait until they finish
	CloseFATRequest *NEW(cfr);
	initIORequest(&cfr->ior, cfr, notSupportCancelIORequest, finishCloseFAT);
	cfr->file = f;
	cfr->fileManager = getOpenFileManager(processorLocalTask());
	pendIO(&cfr->ior);
	finishIO(&cfr->ior);
	return &cfr->ior;
}

static int skipSlash(const char *s, uintptr_t i, uintptr_t len){
	while(i < len && s[i] == '/')
		i++;
	return i;
}
static int skipNonSlash(const char *s, uintptr_t i, uintptr_t len){
	while(i < len && s[i] != '/')
		i++;
	return i;
}

static int nextLevelDirectory(FATDirEntry *d, const FAT32DiskPartition *dp,
	const char *name, uintptr_t length){
	FATFile *ff = searchCreateFATFile(dp, getBeginCluster(d), 1);
	EXPECT(ff != NULL);
	acquireReaderLock(ff->rwLock);
	const uint32_t clusterCount = countClusterByFAT(ff->beginCluster, dp);
	const uint32_t allocateSize = clusterCount * dp->bootRecord->sectorsPerCluster * dp->sectorSize;
	FATDirEntry *dirEntry = systemCall_allocateHeap(allocateSize, USER_NON_CACHED_PAGE);
	if(dirEntry == NULL){
		releaseReaderWriterLock(ff->rwLock);
	}
	EXPECT(dirEntry != NULL);
	uintptr_t readSize = readByFAT(dp, dirEntry, ff->beginCluster, 0, allocateSize);
	releaseReaderWriterLock(ff->rwLock);
	EXPECT(readSize == allocateSize);
	FATDirEntry *newDirEntry = searchDirectory(dirEntry,
		allocateSize / sizeof(FATDirEntry), name, length);
	EXPECT(newDirEntry != NULL);
	*d = *newDirEntry;
	systemCall_releaseHeap(dirEntry);
	addFATFileReference(ff, -1);
	return 1;

	ON_ERROR;
	ON_ERROR;
	systemCall_releaseHeap(dirEntry);
	ON_ERROR;
	addFATFileReference(ff, -1);
	ON_ERROR;
	return 0;
}

static void openFATTask(void *p){
	OpenFATRequest *ofr = *(OpenFATRequest**)p;
	uintptr_t nameIndex = 0;
	FAT32DiskPartition *dp = searchFAT32DiskPartition(ofr->fileName, &nameIndex, ofr->nameLength);
	EXPECT(dp != NULL);

	FATDirEntry d;
	initRootDirEntry(&d, dp->bootRecord->ebr32.rootCluster);
	int ok = 1;
	while(ok){
		nameIndex = skipSlash(ofr->fileName, nameIndex, ofr->nameLength);
		uintptr_t nextNameIndex = skipNonSlash(ofr->fileName, nameIndex, ofr->nameLength);
		if(nameIndex == nextNameIndex){ // end of string
			ok = ((d.attribute & (FAT_DIRECTORY | FAT_VOLUME_ID)) == 0 &&
				d.attribute != FAT_LONG_FILE_NAME); // is a file
			break;
		}
		if(d.attribute != FAT_DIRECTORY){ // is not a directory
			ok = 0;
			break;
		}
		ok = nextLevelDirectory(&d, dp, ofr->fileName + nameIndex, nextNameIndex - nameIndex);
		nameIndex = nextNameIndex;
	}
	EXPECT(ok);

	FileFunctions ff = INITIAL_FILE_FUNCTIONS;
	ff.read = readFAT;
	ff.sizeOf = sizeOfFAT;
	ff.close = closeFAT;
	OpenedFATFile *file = createOpenedFATFile(&ff, dp, &d);
	EXPECT(file != NULL);

	ofr->file = file;
	addToOpenFileList(ofr->fileManager, &file->ofr);
	finishIO(&ofr->ior);
	systemCall_terminate();

	//deleteOpenedFATFile(file);
	ON_ERROR;
	ON_ERROR;
	ON_ERROR;
	// ofr->file = NULL;
	finishIO(&ofr->ior);
	printk("open FAT failed\n");
	systemCall_terminate();
}

void fatService(void){
	fat32List.mainTask = processorLocalTask();
	//slab = createUserSlabManager();
	uintptr_t discoverFAT = systemCall_discoverDisk(MBR_FAT32);
	assert(discoverFAT != IO_REQUEST_FAILURE);
	uintptr_t startLBALow;
	uintptr_t startLBAHigh;
	FileNameFunctions ff = INITIAL_FILE_NAME_FUNCTIONS;
	ff.open = openFAT;
	if(addFileSystem(&ff, "fat", strlen("fat")) != 1){
		printk("add file system failure\n");
	}
	int i;
	for(i = 'C'; i <= 'Z'; i++){
		uintptr_t diskFileHandle, sectorSize;
		uintptr_t discoverFAT2 = systemCall_waitIOReturn(
			discoverFAT, 4,
			&startLBALow, &startLBAHigh, &diskFileHandle, &sectorSize);
		if(discoverFAT != discoverFAT2){
			printk("discover disk failure\n");
			continue;
		}
		FAT32DiskPartition *dp = createFATPartition(diskFileHandle, COMBINE64(startLBALow, startLBAHigh), sectorSize, i);
		if(dp == NULL){
			continue;
		}
		addFAT32DiskPartition(dp);
	}
	printk("too many fat systems\n");
	while(1){
		sleep(1000);
	}
	panic("cannot initialize FAT32 service");
}

#ifndef NDEBUG
void testFAT(void);
void testFAT(void){
	uintptr_t fileHandle;
	int a;
	for(a = 3; a > 0; a--){
		sleep(1000);
		printk("test open fat...\n");
		fileHandle = syncOpenFile("fat:C/FDOS/watTCP.cfg");
		if(fileHandle != IO_REQUEST_FAILURE){
			printk("test open fat ok...\n");
			break;
		}
		printk("test open fat failed...\n");
	}
	assert(a > 0);
	uint64_t fileSize = 0;
	uintptr_t r = syncSizeOfFile(fileHandle, &fileSize);
	assert(r == fileHandle);
	printk("fat file size = %d\n", (uintptr_t)fileSize);
	uintptr_t totalReadSize = 0;
	while(1){
		char str[33] = "abcd";
		uintptr_t readSize = 32;
		r = syncReadFile(fileHandle, str, &readSize);
		assert(r == fileHandle);
		totalReadSize += readSize;
		str[readSize] = '\0';
		printk("%s", str);
		if(readSize != 32)
			break;
	}
	assert(fileSize == totalReadSize);
	r = syncCloseFile(fileHandle);
	assert(r == fileHandle);
	printk("test close fat ok\n");
	systemCall_terminate();
}
#endif
