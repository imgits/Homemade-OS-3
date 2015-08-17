#include<std.h>
#include"memory/memory.h"
#include"multiprocessor/spinlock.h"

// taskmanager.c
typedef struct TaskQueue{
	struct Task *volatile head;
}TaskQueue;

#define INITIAL_TASK_QUEUE {NULL}
extern const TaskQueue initialTaskQueue;

// put task to the last position of the queue
void pushQueue(TaskQueue *q, struct Task *t);
// get the first task in the queue
struct Task *popQueue(TaskQueue *q);

// assume interrupt disabled
void taskSwitch(void (*afterTaskSwitchFunc)(struct Task*, uintptr_t), uintptr_t arg);

// semaphore.c
typedef struct SystemCallTable SystemCallTable;
void initSemaphore(SystemCallTable *systemCallTable);
