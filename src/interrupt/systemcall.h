#ifndef SYSTEMCALL_H_INCLUDED
#define SYSTEMCALL_H_INCLUDED

#include"handler.h"
enum SystemCall{
	// reserved
	// SYSCALL_TEST = 0
	SYSCALL_TASK_DEFINED = 1,
	SYSCALL_ACQUIRE_SEMAPHORE = 2,
	SYSCALL_RELEASE_SEMAPHORE = 3,
	//SYSCALL_REGISTER_SERVICE = 4,
	SYSCALL_QUERY_SERVICE = 5,
	SYSCALL_WAIT_IO = 6,
	SYSCALL_DISCOVER_DISK = 7,
	SYSCALL_ALLOCATE_HEAP = 8,
	SYSCALL_RELEASE_HEAP = 9,
	// runtime registration
	NUMBER_OF_RESERVED_SYSTEM_CALLS = 16,
	NUMBER_OF_SYSTEM_CALLS = 32
};
#define SYSCALL_SERVICE_BEGIN ((int)NUMBER_OF_RESERVED_SYSTEM_CALLS)
#define SYSCALL_SERVICE_END ((int)NUMBER_OF_SYSTEM_CALLS)

typedef InterruptHandler SystemCallFunction;

// see interruptdescriptor.c
uintptr_t systemCall1(/*enum SystemCall*/int systemCallNumber);
uintptr_t systemCall2(int systemCallNumber, uintptr_t *arg1);
uintptr_t systemCall3(int systemCallNumber, uintptr_t *arg1, uintptr_t *arg2);
uintptr_t systemCall4(int systemCallNumber, uintptr_t *arg1, uintptr_t *arg2, uintptr_t *arg3);
uintptr_t systemCall5(int systemCallNumber, uintptr_t *arg1, uintptr_t *arg2, uintptr_t *arg3, uintptr_t *arg4);
uintptr_t systemCall6(int systemCallNumber, uintptr_t *arg1, uintptr_t *arg2, uintptr_t *arg3, uintptr_t *arg4, uintptr_t *arg5);

#define SYSTEM_CALL_ARGUMENT_0(P) ((P)->regs.edx)
#define SYSTEM_CALL_ARGUMENT_1(P) ((P)->regs.ecx)
#define SYSTEM_CALL_ARGUMENT_2(P) ((P)->regs.ebx)
#define SYSTEM_CALL_ARGUMENT_3(P) ((P)->regs.esi)
#define SYSTEM_CALL_ARGUMENT_4(P) ((P)->regs.edi)

#define SYSTEM_CALL_RETURN_VALUE_0(P) ((P)->regs.eax)
#define SYSTEM_CALL_RETURN_VALUE_1(P) ((P)->regs.edx)
#define SYSTEM_CALL_RETURN_VALUE_2(P) ((P)->regs.ecx)
#define SYSTEM_CALL_RETURN_VALUE_3(P) ((P)->regs.ebx)

#define SYSTEM_CALL_MAX_RETURN_COUNT (6)

typedef struct SystemCallTable SystemCallTable;
// reserved system call
void registerSystemCall(
	SystemCallTable *s,
	enum SystemCall systemCall,
	SystemCallFunction func,
	uintptr_t arg
);
// runtime registration system call
enum ServiceNameError{
	INVALID_NAME = -1024,
	SERVICE_EXISTING,
	SERVICE_NOT_EXISTING,
	TOO_MANY_SERVICES,
};

#define MAX_NAME_LENGTH (16)
typedef char ServiceName[MAX_NAME_LENGTH];

// return system call number
int registerService(
	SystemCallTable *systemCallTable,
	const char *name,
	SystemCallFunction func,
	uintptr_t arg
);
int systemCall_queryService(const char *name);

typedef struct InterruptTable InterruptTable;
SystemCallTable *initSystemCall(InterruptTable *t);

#define KEYBOARD_SERVICE_NAME ("keyboard")
#define MOUSE_SERVICE_NAME ("mouse")
#define VIDEO_SERVICE_NAME ("video")
#define KERNEL_CONSOLE_SERVICE_NAME ("kernelconsole")
#define PCI_SERVICE_NAME ("enumeratepci")
#define AHCI_SERVICE_NAME ("ahci")

#endif
