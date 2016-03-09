#include"interrupt/systemcall.h"
#include"interrupt/interrupt.h"
#include"multiprocessor/processorlocal.h"
#include"common.h"
#include"file.h"
#include"task/task.h"
#include"resource/resource.h"

// disk driver

// MBR & EBR
#pragma pack(1)
struct MBR{
	uint8_t bootCode[446];
	struct PartitionEntry{
		uint8_t active; // 0x80 = active
		uint8_t startHead;
		uint16_t startSector: 6;
		uint16_t startCydlinder: 10;
		uint8_t systemID;
		uint8_t endHead;
		uint16_t endSector: 6;
		uint16_t endCylinder: 10;
		uint32_t startLBA;
		uint32_t sectorCount;
	}partition[4];
	uint16_t signature;
}__attribute__((__packed__));
#pragma pack()

#define MBR_SIGNATRUE (0xaa55)

static_assert(sizeof(struct MBR) == 512);
static_assert(sizeof(struct PartitionEntry) == 16);

// assume all arguments are valid
static int addDiskPartition(
	const char *fileName, uintptr_t nameLength,
	DiskPartitionType diskType, uint64_t startLBA, uint64_t sectorCount, uintptr_t sectorSize
){
	EXPECT(diskType >= 0 && diskType < MAX_DISK_TYPE && nameLength <= MAX_FILE_ENUM_NAME_LENGTH);
	FileEnumeration fe;
	initFileEnumeration(&fe, fileName, nameLength);
	fe.diskPartition.type = diskType;
	fe.diskPartition.startLBA = startLBA;
	fe.diskPartition.sectorCount = sectorCount;
	fe.diskPartition.sectorSize = sectorSize;
	int ok = createAddResource(RESOURCE_DISK_PARTITION, &fe);
	EXPECT(ok);
	return 1;

	ON_ERROR;
	ON_ERROR;
	return 0;
}

static void _readPartitions(
	uintptr_t fileHandle, uint64_t relativeLBA,
	const char *fileName, uintptr_t nameLength,
	uint64_t sectorCount, uintptr_t sectorSize
){
	struct MBR *buffer = systemCall_allocateHeap(sizeof(*buffer), KERNEL_NON_CACHED_PAGE);
	EXPECT(buffer != NULL);
	uintptr_t readSize = sectorSize;
	uintptr_t ior1 = syncSeekReadFile(fileHandle, buffer, relativeLBA * sectorSize, &readSize);
	EXPECT(ior1 != IO_REQUEST_FAILURE && sectorSize == readSize);

	if(buffer->signature != MBR_SIGNATRUE){
		printk("disk %x LBA %u is not a bootable partition\n", fileHandle, (int)relativeLBA);
	}

	int i;
	for(i = 0; i < 4; i++){
		struct PartitionEntry *pe = buffer->partition + i;
		const uint64_t lba = relativeLBA + pe->startLBA;
		//printk("type %x, lba %u, sector count %u ", pe->systemID, lba, pe->sectorCount);
		if(pe->systemID == MBR_EMPTY){
			//printk("(empty partition)\n");
			continue;
		}
		if(lba == 0 || lba <= relativeLBA || lba - relativeLBA > sectorCount){
			//printk("(invalid startLBA)\n");
			continue;
		}
		if(pe->sectorCount == 0 || pe->sectorCount > sectorCount - (lba - relativeLBA)){
			//printk("(invalid sectorCount)\n");
			continue;
		}
		//printk("\n");
		if(pe->systemID == MBR_EXTENDED){
			_readPartitions(fileHandle, lba, fileName, nameLength, pe->sectorCount, sectorSize);
		}
		if(addDiskPartition(fileName, nameLength, pe->systemID, lba, pe->sectorCount, sectorSize) == 0){
			printk("warning: cannot register disk %x to RESOURCE_DISK_PARTITION\n", fileHandle);
		}
	}

	systemCall_releaseHeap(buffer);
	return;
	ON_ERROR;
	systemCall_releaseHeap(buffer);
	ON_ERROR;
	printk("cannot read partitions\n");
}

void readPartitions(const char *fileName, uintptr_t nameLength, uint64_t sectorCount, uintptr_t sectorSize){
	uintptr_t fileHandle = syncOpenFile(fileName);
	assert(fileHandle != IO_REQUEST_FAILURE);
	_readPartitions(fileHandle, 0, fileName, nameLength, sectorCount, sectorSize);
	uintptr_t r = syncCloseFile(fileHandle);
	assert(r != IO_REQUEST_FAILURE);
}

static int matchDiskType(const FileEnumeration *fe, uintptr_t t){
	return fe->diskPartition.type == t;
}

uintptr_t enumNextDiskPartition(uintptr_t f, DiskPartitionType t, FileEnumeration *fe){
	return enumNextResource(f, fe, t, matchDiskType);
}

#define MAX_FILE_SERVICE_NAME_LENGTH (8)

typedef struct FileSystem{
	FileNameFunctions fileNameFunctions;
	char name[MAX_FILE_SERVICE_NAME_LENGTH];
	uintptr_t nameLength;

	struct FileSystem **prev, *next;
}FileSystem;

static struct{
	FileSystem *head;
	Spinlock lock;
}fsList = {NULL, INITIAL_SPINLOCK};

int addFileSystem(const FileNameFunctions *fileNameFunctions, const char *name, size_t nameLength){
	EXPECT(nameLength <= MAX_FILE_SERVICE_NAME_LENGTH);
	FileSystem *NEW(fs);
	EXPECT(fs != NULL);
	MEMSET0(fs);
	strncpy(fs->name, name, nameLength);
	fs->nameLength = nameLength;
	fs->fileNameFunctions = *fileNameFunctions;
	acquireLock(&fsList.lock);
	ADD_TO_DQUEUE(fs, &fsList.head);
	releaseLock(&fsList.lock);
	FileEnumeration fe;
	initFileEnumeration(&fe, name, nameLength);
	int ok = createAddResource(RESOURCE_FILE_SYSTEM, &fe);
	EXPECT(ok);
	return 1;

	ON_ERROR;
	// TODO: remove fs
	// DELETE(fs);
	ON_ERROR;
	ON_ERROR;
	return 0;
}

static FileSystem *searchFileSystem(const char *name, uintptr_t nameLength){
	acquireLock(&fsList.lock);
	FileSystem *fs;
	for(fs = fsList.head; fs != NULL; fs = fs->next){
		if(isStringEqual(fs->name, fs->nameLength, name, nameLength)){
			break;
		}
	}
	releaseLock(&fsList.lock);
	return fs;
}

// OpenedFile

struct OpenedFile{
	void *instance;
	uintptr_t handle;
	FileFunctions fileFunctions;

	Spinlock lock;
	uint32_t deleteOnCompletion;
	uint32_t ioCount;
	//uint64_t readByteCount, writeByteCount;
	uint64_t offset;

	CloseFileRequest *cfr;
	OpenFileManager *fileManager;
	//Task *task;
	OpenedFile *next, **prev;
};

static void initOpenedFile(OpenedFile *of, CloseFileRequest *cfr/*, Task *task, */){
	of->instance = NULL; // see initOpenedFile2
	of->handle = (uintptr_t)&of->handle;
	of->lock = initialSpinlock;
	// delete if failed to open
	of->deleteOnCompletion = 1;
	of->ioCount = 1;
	of->offset = 0;
	of->cfr = cfr;

	of->fileManager = NULL; // see initOpenedFile2
	//ofr->task = task;
	MEMSET0(&of->fileFunctions); // see initOpenedFile2
	of->next = NULL;
	of->prev = NULL;
}

//#define IS_OPENED_FILE_INITIALIZED(OF) ((OF)->fileManager != NULL)

static void initOpenedFile2(OpenedFile *of, void *instance, const FileFunctions *fileFunctions, OpenFileManager *ofm){
	of->deleteOnCompletion = 0;
	of->instance = instance;
	of->fileFunctions = (*fileFunctions);
	addToOpenFileList(ofm, of);
}

static int setFileClosing(OpenedFile *of){
	acquireLock(&of->lock);
	int ok = (of->deleteOnCompletion == 0 && of->ioCount == 0);
	if(ok){
		of->deleteOnCompletion = 1;
		of->ioCount = 1;
	}
	releaseLock(&of->lock);
	return ok;
}

static int addFileIOCount(OpenedFile *of, int v){
	acquireLock(&of->lock);
	int doDelete = ((of->deleteOnCompletion != 0 && of->ioCount + v == 0));
	int ok = (of->deleteOnCompletion == 0 || of->ioCount + v == 0);
	if(ok){
		of->ioCount += v;
	}
	releaseLock(&of->lock);
	if(doDelete){
		assert(ok && v == -1);
		DELETE(of);
	}
	return ok;
}

static void addFileOffset(OpenedFile *of, uintptr_t v){
	acquireLock(&of->lock);
	of->offset += v;
	releaseLock(&of->lock);
}

uint64_t getFileOffset(OpenedFile *of){
	acquireLock(&of->lock);
	uint64_t r = of->offset;
	releaseLock(&of->lock);
	return r;
}

// IMPROVE: openFileHashTable
struct OpenFileManager{
	OpenedFile *openFileList;
	Spinlock lock;
	int referenceCount;
};

OpenFileManager *createOpenFileManager(void){
	OpenFileManager *NEW(ofm);
	if(ofm == NULL){
		return NULL;
	}
	ofm->openFileList = NULL;
	ofm->lock = initialSpinlock;
	ofm->referenceCount = 0;
	return ofm;
}

void deleteOpenFileManager(OpenFileManager *ofm){
	assert(ofm->openFileList == NULL && ofm->referenceCount == 0 && isAcquirable(&ofm->lock));
	DELETE(ofm);
}

int addOpenFileManagerReference(OpenFileManager *ofm, int n){
	acquireLock(&ofm->lock);
	ofm->referenceCount += n;
	int refCnt = ofm->referenceCount;
	releaseLock(&ofm->lock);
	return refCnt;
}

static OpenedFile *searchOpenFileList(OpenFileManager *ofm, uintptr_t handle, int isClosing){
	int ok = 0;
	acquireLock(&ofm->lock);
	OpenedFile *of;
	for(of = ofm->openFileList; of != NULL; of = of->next){
		if(of->handle == handle){
			ok = (isClosing? setFileClosing(of): addFileIOCount(of, 1));
			break;
		}
	}
	releaseLock(&ofm->lock);
	return (ok? of: NULL);
}


void addToOpenFileList(OpenFileManager *ofm, OpenedFile *of){
	assert(of->fileManager == NULL);
	of->fileManager = ofm;
	acquireLock(&ofm->lock);
	ADD_TO_DQUEUE(of, &ofm->openFileList);
	releaseLock(&ofm->lock);
}

void removeFromOpenFileList(OpenedFile *of){
	assert(of->fileManager != NULL);
	acquireLock(&of->fileManager->lock);
	REMOVE_FROM_DQUEUE(of);
	releaseLock(&of->fileManager->lock);
}

uintptr_t getFileHandle(OpenedFile *of){
	return of->handle;
}

void *getFileInstance(OpenedFile *of){
	return of->instance;
}

// buffer memory

static void bufferToPageRange(uintptr_t bufferBegin, size_t bufferSize,
	uintptr_t *pageBegin, uintptr_t *pageOffset, size_t *pageSize){
	*pageBegin = FLOOR(bufferBegin, PAGE_SIZE);
	*pageOffset = bufferBegin - (*pageBegin);
	*pageSize = CEIL(bufferBegin + bufferSize, PAGE_SIZE) - (*pageBegin);
}

// call unmapPages(kernelLinear, mappedPage) to release
static int mapBufferToKernel(const void *buffer, uintptr_t size, void **mappedPage, void **mappedBuffer){
	uintptr_t pageOffset, pageBegin;
	size_t pageSize;
	bufferToPageRange((uintptr_t)buffer, size, &pageBegin, &pageOffset, &pageSize);
	*mappedPage = checkAndMapExistingPages(
		kernelLinear, getTaskLinearMemory(processorLocalTask()),
		pageBegin, pageSize, KERNEL_PAGE, 0);
	if(*mappedPage == NULL){
		return 0;
	}
	*mappedBuffer = (void*)(pageOffset + ((uintptr_t)*mappedPage));
	return 1;
}

/*
static PhysicalAddressArray *reserveBufferPages(void *buffer, uintptr_t bufferSize, uintptr_t *bufferOffset){
	uintptr_t pageBegin, pageSize;
	bufferToPageRange((uintptr_t)buffer, bufferSize, &pageBegin, bufferOffset, &pageSize);
	return checkAndReservePages(getTaskLinearMemory(processorLocalTask()), (void*)pageBegin, pageSize);
}
*/

// FileIORequest

struct FileIORequest{
	IORequest ior;
	void *acceptCancelArg;
	CancelFileIO *cancelFileIO;
	AcceptFileIO *acceptFileIO;
	void *instance;
	OpenedFile *file;
	int returnCount;
	uintptr_t returnValues[0];
};

struct RWFileRequest{
	int isWrite: 1;
	int updateOffset: 1;
	void *mappedPage;
	struct FileIORequest fior;
	uintptr_t returnValues[1];
};

struct FileIORequest2{
	struct FileIORequest fior;
	uintptr_t returnValues[2];
};

struct OpenFileRequest{
	struct OpenFileManager *fileManager;
	void *mappedPage;
	struct FileIORequest ofior;
	uintptr_t returnValues[1];
};

struct CloseFileRequest{
	struct FileIORequest cfior;
	//uintptr_t returnValues[0];
};

static_assert(MEMBER_OFFSET(struct FileIORequest, returnValues) ==
	MEMBER_OFFSET(RWFileRequest, returnValues) - MEMBER_OFFSET(RWFileRequest, fior));
static_assert(MEMBER_OFFSET(struct FileIORequest, returnValues) ==
	MEMBER_OFFSET(OpenFileRequest, returnValues) - MEMBER_OFFSET(OpenFileRequest, ofior));

void defaultAcceptFileIO(__attribute__((__unused__)) void *instance){
}

void defaultCancelFileIO(__attribute__((__unused__)) void *instance){
}

static void cancelDeleteFileIO(void *instance){
	struct FileIORequest *r0 = instance;
	r0->cancelFileIO(r0->acceptCancelArg);
	DELETE(r0->instance);
}

static int acceptDeleteFileIO(void *instance, uintptr_t *returnValue){
	struct FileIORequest *r0 = instance;
	int r = r0->returnCount, i;
	for(i = 0; i < r; i++){
		returnValue[i] = r0->returnValues[i];
	}

	r0->acceptFileIO(r0->acceptCancelArg);
	DELETE(r0->instance);
	return r;
}

static void initFileIO(struct FileIORequest *fior, void *instance, OpenedFile *file){
	initIORequest(&fior->ior, fior, cancelDeleteFileIO, acceptDeleteFileIO);
	//fior->ofr = ofr;
	fior->instance = instance;
	fior->file = file;
	assert(file != NULL);
	fior->acceptCancelArg = fior;
	fior->cancelFileIO = defaultCancelFileIO;
	fior->acceptFileIO = defaultAcceptFileIO;
}

static OpenFileRequest *createOpenFileIO(OpenedFile *openingFile, void *mappedNamePage){
	OpenFileRequest *NEW(ofr);
	if(ofr == NULL)
		return NULL;
	initFileIO(&ofr->ofior, ofr, openingFile);
	ofr->fileManager = getOpenFileManager(processorLocalTask());
	ofr->mappedPage = mappedNamePage;
	return ofr;
}
/*
static FileIORequest0 *createFileIO0(OpenedFile *file){
	FileIORequest0 *NEW(r0);
	if(r0 == NULL)
		return NULL;
	initFileIO(&r0->fior, r0, file);
	return r0;
}
*/

static RWFileRequest *createRWFileIO(OpenedFile *file, int doWrite, int updateOffset, void *mappedBufferPage){
	RWFileRequest *NEW(rwfr);
	if(rwfr == NULL)
		return NULL;
	initFileIO(&rwfr->fior, rwfr, file);
	rwfr->isWrite = doWrite;
	rwfr->updateOffset = updateOffset;
	rwfr->mappedPage = mappedBufferPage;
	return rwfr;
}

static FileIORequest2 *createFileIO2(OpenedFile *file){
	FileIORequest2 *NEW(r2);
	if(r2 == NULL)
		return NULL;
	initFileIO(&r2->fior, r2, file);
	return r2;
}

static CloseFileRequest *createCloseFileIO(OpenedFile *file){
	CloseFileRequest *NEW(cfr);
	if(cfr == NULL)
		return NULL;
	initFileIO(&cfr->cfior, cfr, file);
	return cfr;
}

static void setFileIOFunctions(struct FileIORequest *fior, void *arg, CancelFileIO *cancelFileIO, AcceptFileIO *acceptFileIO){
	fior->acceptCancelArg = arg;
	fior->cancelFileIO = cancelFileIO;
	fior->acceptFileIO = acceptFileIO;
}

void setRWFileIOFunctions(RWFileRequest *rwfr, void *arg, CancelFileIO *cancelFileIO, AcceptFileIO *acceptFileIO){
	setFileIOFunctions(&rwfr->fior, arg, cancelFileIO, acceptFileIO);
}

static void pendFileIO(struct FileIORequest *fior){
	pendIO(&fior->ior);
}

// also delete FileIORequest. see cancelDeleteFileIO
static void cancelFailedFileIO(struct FileIORequest *fior){
	assert(isCancellable(&fior->ior) == 0);
	setCancellable(&fior->ior, 1);
	if(tryCancelIO(&fior->ior) == 0){
		panic("cannot cancel failed file io");
	}
}

static void completeFileIO(struct FileIORequest *fior, int returnCount, ...){
	// delete OpenedFile here
	int ok = addFileIOCount(fior->file, -1);
	assert(ok);

	fior->returnCount = returnCount;
	va_list va;
	va_start(va, returnCount);
	int i;
	for(i = 0; i < returnCount; i++){
		fior->returnValues[i] = va_arg(va, uintptr_t);
	}
	va_end(va);

	completeIO(&fior->ior);
}

void completeFileIO0(FileIORequest2 *r0){
	completeFileIO(&r0->fior, 0);
}

void completeRWFileIO(RWFileRequest *r1, uintptr_t rwByteCount, uintptr_t addOffset){
	assert(r1->mappedPage != NULL);
	unmapPages(kernelLinear, r1->mappedPage);
	r1->mappedPage = NULL;
	if(r1->updateOffset){
		addFileOffset(r1->fior.file, addOffset);
	}
	completeFileIO(&r1->fior, 1, rwByteCount);
}

void completeFileIO1(FileIORequest2 *r2, uintptr_t v0){
	completeFileIO(&r2->fior, 1, v0);
}

void completeFileIO2(FileIORequest2 *r2, uintptr_t v0, uintptr_t v1){
	completeFileIO(&r2->fior, 2, v0, v1);
}

void completeFileIO64(FileIORequest2 *r2, uint64_t v0){
	completeFileIO2(r2, LOW64(v0), HIGH64(v0));
}

static void _completeOpenFile(OpenFileRequest *r1, void *fileInstance, const FileFunctions *ff, int ok){
	OpenedFile *of = r1->ofior.file;
	assert(of->fileManager == NULL && r1->mappedPage != NULL);
	unmapPages(kernelLinear, r1->mappedPage);
	r1->mappedPage = NULL;
	if(ok){
		initOpenedFile2(of, fileInstance, ff, r1->fileManager);
		completeFileIO(&r1->ofior, 1, getFileHandle(of));
	}
	else{
		DELETE(of->cfr);
		// if not ok, delete OpenedFile here
		completeFileIO(&r1->ofior, 1, IO_REQUEST_FAILURE);
	}
}

void failOpenFile(OpenFileRequest *r1){
	_completeOpenFile(r1, NULL, NULL, 0);
}

void completeOpenFile(OpenFileRequest *r1, void *fileInstance, const FileFunctions *ff){
	_completeOpenFile(r1, fileInstance, ff, 1);
}

void completeCloseFile(CloseFileRequest* r0){
	OpenedFile *of = r0->cfior.file;
	assert(of->ioCount == 1);
	removeFromOpenFileList(of);
	completeFileIO(&r0->cfior, 0);
}

// call DELETE if fail
static int createOpenFileRequests(OpenedFile **of, CloseFileRequest **cfr, OpenFileRequest **ofr, void *mappedNamePage){
	OpenedFile *NEW(of2);
	EXPECT(of2 != NULL);
	CloseFileRequest *cfr2 = createCloseFileIO(of2);
	EXPECT(cfr2 != NULL);
	OpenFileRequest *ofr2 = createOpenFileIO(of2, mappedNamePage);
	EXPECT(ofr2 != NULL);
	initOpenedFile(of2, cfr2);
	(*of) = of2;
	(*cfr) = cfr2;
	(*ofr) = ofr2;
	return 1;
	//DELETE(ofr2);
	ON_ERROR;
	DELETE(cfr2);
	ON_ERROR;
	DELETE(of2);
	ON_ERROR;
	return 0;
}

void closeAllOpenFileRequest(OpenFileManager *ofm){
	assert(ofm->referenceCount == 0);
	while(1){
		acquireLock(&ofm->lock);
		OpenedFile *of = ofm->openFileList;
		releaseLock(&ofm->lock);
		if(of == NULL)
			break;
		assert(of->fileFunctions.close != NULL);
		uintptr_t r = syncCloseFile(of->handle);
		if(r == IO_REQUEST_FAILURE){
			printk("warning: cannot close file %x", of->handle);
			REMOVE_FROM_DQUEUE(of);
		}
	}
}

#define _UNUSED __attribute__((__unused__))

int dummyOpen(_UNUSED OpenFileRequest *ofr, _UNUSED const char *name, _UNUSED uintptr_t nameLength, _UNUSED OpenFileMode openMode){
	return 0;
}
int dummyRead(_UNUSED RWFileRequest *rwfr, _UNUSED OpenedFile *of, _UNUSED uint8_t *buffer, _UNUSED uintptr_t bufferSize){
	return 0;
}
int dummyWrite(_UNUSED RWFileRequest *rwfr, _UNUSED OpenedFile *of, _UNUSED const uint8_t *buffer, _UNUSED uintptr_t bufferSize){
	return 0;
}
int dummySeekRead(_UNUSED RWFileRequest *rwfr, _UNUSED OpenedFile *of, _UNUSED uint8_t *buffer, _UNUSED uint64_t position, _UNUSED uintptr_t bufferSize){
	return 0;
}
int dummySeekWrite(_UNUSED RWFileRequest *rwfr, _UNUSED OpenedFile *of, _UNUSED const uint8_t *buffer, _UNUSED uint64_t position, _UNUSED uintptr_t bufferSize){
	return 0;
}
int dummyGetParameter(_UNUSED FileIORequest2 *fior2, _UNUSED OpenedFile *of, _UNUSED uintptr_t parameterCode){
	return 0;
}
void dummyClose(_UNUSED CloseFileRequest *cfr, _UNUSED OpenedFile *of){
	panic("dummyClose");
}
#undef _UNUSED

int seekReadByOffset(RWFileRequest *rwfr, OpenedFile *of, uint8_t *buffer, uintptr_t bufferSize){
	return of->fileFunctions.seekRead(rwfr, of, buffer, getFileOffset(of), bufferSize);
}

int seekWriteByOffset(RWFileRequest *rwfr, OpenedFile *of, const uint8_t *buffer, uintptr_t bufferSize){
	return of->fileFunctions.seekWrite(rwfr, of, buffer, getFileOffset(of), bufferSize);
}


#define NULL_OR(R) ((R) == NULL? (NULL): &(R)->fior)
// arg0 = str; arg1 = strLen
static IORequest *dispatchFileNameCommand(
	FileSystem *fs,
	const char *fileName, uintptr_t nameLen, InterruptParam *p, void *mappedNamePage
){
	const FileNameFunctions *ff = &fs->fileNameFunctions;
	struct FileIORequest *fior = NULL;
	switch(SYSTEM_CALL_NUMBER(p)){
	case SYSCALL_OPEN_FILE:
		{
			OpenedFile *openingFile;
			OpenFileRequest *ofr;
			CloseFileRequest *cfr;
			int ok = createOpenFileRequests(&openingFile, &cfr, &ofr, mappedNamePage);
			if(!ok)
				break;
			pendFileIO(&ofr->ofior);
			OpenFileMode m = {value: SYSTEM_CALL_ARGUMENT_2(p)};
			ok = ff->open(ofr, fileName, nameLen, m);

			if(!ok){
				cancelFailedFileIO(&ofr->ofior);
				DELETE(cfr);
				DELETE(openingFile);
				break;
			}
			fior = &ofr->ofior;
		}
		break;
	default:
		fior = NULL;
	}
	if(fior != NULL){
		return &fior->ior;
	}
	return IO_REQUEST_FAILURE;
}

static RWFileRequest *dispatchRWFileCommand(OpenedFile *of, const InterruptParam *p){
	const uintptr_t scNumber = SYSTEM_CALL_NUMBER(p);
	const uintptr_t buffer = SYSTEM_CALL_ARGUMENT_1(p);
	const uintptr_t size = SYSTEM_CALL_ARGUMENT_2(p);
	void *mappedPage;
	void *mappedBuffer;
	// see completeRWFileIO
	int bufferOK = mapBufferToKernel((const void*)buffer, size, &mappedPage, &mappedBuffer);
	EXPECT(bufferOK);
	const int isWrite = (scNumber == SYSCALL_WRITE_FILE || scNumber == SYSCALL_SEEK_WRITE_FILE);
	const int updateOffset = (scNumber == SYSCALL_READ_FILE || scNumber == SYSCALL_WRITE_FILE);
	const FileFunctions *f = &of->fileFunctions;
	RWFileRequest *rwfr = createRWFileIO(of, isWrite, updateOffset, mappedPage);
	EXPECT(rwfr != NULL);
	pendFileIO(&rwfr->fior);
	int rwOK;
	switch(SYSTEM_CALL_NUMBER(p)){
	case SYSCALL_READ_FILE:
		rwOK = f->read(rwfr, of, (uint8_t*)mappedBuffer, size);
		break;
	case SYSCALL_WRITE_FILE:
		rwOK = f->write(rwfr, of, (uint8_t*)mappedBuffer, size);
		break;
	case SYSCALL_SEEK_READ_FILE:
		rwOK = f->seekRead(rwfr, of, (uint8_t*)mappedBuffer,
			COMBINE64(SYSTEM_CALL_ARGUMENT_4(p), SYSTEM_CALL_ARGUMENT_3(p)), size);
		break;
	case SYSCALL_SEEK_WRITE_FILE:
		rwOK = f->seekWrite(rwfr, of, (uint8_t*)mappedBuffer,
			COMBINE64(SYSTEM_CALL_ARGUMENT_4(p), SYSTEM_CALL_ARGUMENT_3(p)), size);
		break;
	default:
		panic("impossible");
	}
	EXPECT(rwOK);
	return rwfr;
	ON_ERROR;
	cancelFailedFileIO(&rwfr->fior);
	ON_ERROR;
	unmapPages(kernelLinear, mappedPage);
	ON_ERROR;
	return NULL;
}

static IORequest *dispatchFileHandleCommand(const InterruptParam *p){
	const int isClosing = (SYSTEM_CALL_NUMBER(p) == SYSCALL_CLOSE_FILE);
	// file handle to OpenedFile
	OpenFileManager *ofm = getOpenFileManager(processorLocalTask());
	OpenedFile *of = searchOpenFileList(ofm, SYSTEM_CALL_ARGUMENT_0(p), isClosing);

	if(of == NULL)
		return IO_REQUEST_FAILURE;
	// close file
	const FileFunctions *f = &of->fileFunctions;
	struct FileIORequest *fior = NULL;
	RWFileRequest *rwfr;
	FileIORequest2 *r2;
	CloseFileRequest *cfr;
	// check address and map buffer to kernel
	switch(SYSTEM_CALL_NUMBER(p)){
	case SYSCALL_CLOSE_FILE:
		cfr = of->cfr;
		// close cannot fail or be cancelled unless file does not exist
		pendFileIO(&cfr->cfior);
		f->close(cfr, of);
		fior = &cfr->cfior;
		assert(isCancellable(&fior->ior) == 0);
		break;
	case SYSCALL_READ_FILE:
	case SYSCALL_WRITE_FILE:
	case SYSCALL_SEEK_READ_FILE:
	case SYSCALL_SEEK_WRITE_FILE:
		rwfr = dispatchRWFileCommand(of, p);
		if(rwfr != NULL){
			fior = &rwfr->fior;
		}
		break;
	case SYSCALL_GET_FILE_PARAMETER:
		r2 = createFileIO2(of);
		if(r2 != NULL){
			pendFileIO(&r2->fior);
			int ok = f->getParameter(r2, of, SYSTEM_CALL_ARGUMENT_1(p));
			if(!ok){
				cancelFailedFileIO(&r2->fior);
				break;
			}
			fior = &r2->fior;
		}
		break;
	default:
		fior = NULL;
	}
	if(fior != NULL){
		return &fior->ior;
	}
	// if failed
	assert(isClosing == 0);
	addFileIOCount(of, -1);
	return IO_REQUEST_FAILURE;
}

#undef NULL_OR

static_assert(sizeof(OpenFileMode) == sizeof(uintptr_t));

uintptr_t systemCall_openFile(const char *fileName, uintptr_t fileNameLength, OpenFileMode openMode){
	return systemCall4(SYSCALL_OPEN_FILE, (uintptr_t)fileName, fileNameLength, openMode.value);
}

uintptr_t syncOpenFileN(const char *fileName, uintptr_t nameLength, OpenFileMode openMode){
	uintptr_t handle;
	uintptr_t r = systemCall_openFile(fileName, nameLength, openMode);
	if(r == IO_REQUEST_FAILURE)
		return r;
	if(r != systemCall_waitIOReturn(r, 1, &handle))
		return IO_REQUEST_FAILURE;
	return handle;
}

uintptr_t syncOpenFile(const char *fileName){
	return syncOpenFileN(fileName, strlen(fileName), OPEN_FILE_MODE_0);
}

uintptr_t syncEnumerateFileN(const char *fileName, uintptr_t nameLength){
	OpenFileMode m = OPEN_FILE_MODE_0;
	m.enumeration = 1;
	return syncOpenFileN(fileName, nameLength, m);
}

uintptr_t syncEnumerateFile(const char *fileName){
	return syncEnumerateFileN(fileName, strlen(fileName));
}

uintptr_t systemCall_closeFile(uintptr_t handle){
	return systemCall2(SYSCALL_CLOSE_FILE, handle);
}

uintptr_t syncCloseFile(uintptr_t handle){
	uintptr_t r;
	r = systemCall_closeFile(handle);
	if(r == IO_REQUEST_FAILURE)
		return r;
	if(r != systemCall_waitIO(r))
		return IO_REQUEST_FAILURE;
	return handle;
}

uintptr_t systemCall_readFile(uintptr_t handle, void *buffer, uintptr_t bufferSize){
	return systemCall4(SYSCALL_READ_FILE, handle, (uintptr_t)buffer, bufferSize);
}

uintptr_t syncReadFile(uintptr_t handle, void *buffer, uintptr_t *bufferSize){
	uintptr_t r;
	r = systemCall_readFile(handle, buffer, *bufferSize);
	if(r == IO_REQUEST_FAILURE)
		return r;
	if(r != systemCall_waitIOReturn(r, 1, bufferSize))
		return IO_REQUEST_FAILURE;
	return handle;
}

uintptr_t systemCall_writeFile(uintptr_t handle, const void *buffer, uintptr_t bufferSize){
	return systemCall4(SYSCALL_WRITE_FILE, handle, (uintptr_t)buffer, bufferSize);
}

uintptr_t syncWriteFile(uintptr_t handle, const void *buffer, uintptr_t *bufferSize){
	uintptr_t r;
	r = systemCall_writeFile(handle, buffer, *bufferSize);
	if(r == IO_REQUEST_FAILURE)
		return r;
	if(r != systemCall_waitIOReturn(r, 1, bufferSize))
		return IO_REQUEST_FAILURE;
	return handle;
}

uintptr_t systemCall_seekReadFile(uintptr_t handle, void *buffer, uint64_t position, uintptr_t bufferSize){
	return systemCall6(SYSCALL_SEEK_READ_FILE, handle, (uintptr_t)buffer, bufferSize,
		LOW64(position), HIGH64(position));
}

uintptr_t syncSeekReadFile(uintptr_t handle, void *buffer, uint64_t position, uintptr_t *bufferSize){
	uintptr_t r;
	r = systemCall_seekReadFile(handle, buffer, position, *bufferSize);
	if(r == IO_REQUEST_FAILURE)
		return r;
	if(r != systemCall_waitIOReturn(r, 1, bufferSize))
		return IO_REQUEST_FAILURE;
	return handle;
}

uintptr_t systemCall_seekWriteFile(uintptr_t handle, void *buffer, uint64_t position, uintptr_t bufferSize){
	return systemCall6(SYSCALL_SEEK_READ_FILE, handle, (uintptr_t)buffer, bufferSize,
		LOW64(position), HIGH64(position));
}

uintptr_t systemCall_getFileParameter(uintptr_t handle, enum FileParameter parameterCode){
	return systemCall3(SYSCALL_GET_FILE_PARAMETER, handle, parameterCode);
}

uintptr_t syncSizeOfFile(uintptr_t handle, uint64_t *size){
	uintptr_t r, sizeLow, sizeHigh;
	r = systemCall_getFileParameter(handle, FILE_PARAM_SIZE);
	if(r == IO_REQUEST_FAILURE)
		return r;
	if(r != systemCall_waitIOReturn(r, 2, &sizeLow, &sizeHigh))
		return IO_REQUEST_FAILURE;
	*size = COMBINE64(sizeHigh, sizeLow);
	return handle;
}

uintptr_t syncWritableSizeOfFile(uintptr_t handle, uintptr_t *size){
	uintptr_t r, writableSize;
	r = systemCall_getFileParameter(handle, FILE_PARAM_WRITABLE_SIZE);
	if(r == IO_REQUEST_FAILURE)
		return r;
	if(r != systemCall_waitIOReturn(r, 1, &writableSize))
		return IO_REQUEST_FAILURE;
	*size = writableSize;
	return handle;
}

void initFileEnumeration(FileEnumeration *fileEnum, const char *name, uintptr_t nameLength){
	fileEnum->nameLength = nameLength;
	memcpy(fileEnum->name, name, sizeof(name[0]) * fileEnum->nameLength);
}

static void FileNameCommandHandler(InterruptParam *p){
	sti();
	const void *userFileName = (const void*)SYSTEM_CALL_ARGUMENT_0(p);
	uintptr_t nameLength = SYSTEM_CALL_ARGUMENT_1(p);
	void *mappedPage;
	void *mappedBuffer;
	// see completeOpenFileRequest
	int ok = mapBufferToKernel(userFileName, nameLength, &mappedPage, &mappedBuffer);
	EXPECT(ok);
	const char *fileName = (const char*)mappedBuffer;

	uintptr_t i;
	for(i = 0; i < nameLength && fileName[i] != ':'; i++);
	EXPECT(i < nameLength);
	FileSystem *fileSystem = searchFileSystem(fileName, i);
	//if(fileSystem == NULL){
	//	for(i = 0; i < nameLength && fileName[i] != ':'; i++)
	//		printk("%c",fileName[i]);
	//	printk("\n");
	//}
	EXPECT(fileSystem != NULL);
	IORequest *ior = dispatchFileNameCommand(fileSystem, fileName + i + 1, nameLength - i - 1, p, mappedPage);
	EXPECT(ior != IO_REQUEST_FAILURE);
	SYSTEM_CALL_RETURN_VALUE_0(p) = (uintptr_t)ior;
	return;

	ON_ERROR;
	ON_ERROR;
	ON_ERROR;
	unmapPages(kernelLinear, mappedPage);
	ON_ERROR;
	SYSTEM_CALL_RETURN_VALUE_0(p) = IO_REQUEST_FAILURE;
}

static void FileHandleCommandHandler(InterruptParam *p){
	sti();
	IORequest *ior = dispatchFileHandleCommand(p);
	SYSTEM_CALL_RETURN_VALUE_0(p) = (uintptr_t)ior;
}

void initFile(SystemCallTable *s){
	registerSystemCall(s, SYSCALL_OPEN_FILE, FileNameCommandHandler, -1);
	registerSystemCall(s, SYSCALL_CLOSE_FILE, FileHandleCommandHandler, 1);
	registerSystemCall(s, SYSCALL_READ_FILE, FileHandleCommandHandler, 2);
	registerSystemCall(s, SYSCALL_WRITE_FILE, FileHandleCommandHandler, 3);
	registerSystemCall(s, SYSCALL_SEEK_READ_FILE, FileHandleCommandHandler, 5);
	registerSystemCall(s, SYSCALL_SEEK_WRITE_FILE, FileHandleCommandHandler, 6);
	registerSystemCall(s, SYSCALL_GET_FILE_PARAMETER, FileHandleCommandHandler, 7);
}
