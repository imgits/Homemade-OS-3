#include"multiprocessor/spinlock.h"

typedef struct Semaphore Semaphore;

// Semaphore must be in global kernel memory
Semaphore *createSemaphore(void);

void deleteSemaphore(Semaphore *s);
/*
void syscall_acquireSemaphore(Semaphore *s);
void syscall_releaseSemaphore(Semaphore *s);
*/
void acquireSemaphore(Semaphore *s);
void releaseSemaphore(Semaphore *s);
int getSemaphoreValue(Semaphore *s);