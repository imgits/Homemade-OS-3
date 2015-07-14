#include"io.h"
#include"fifo.h"
#include"task/task.h"
#include"multiprocessor/processorlocal.h"
#include"multiprocessor/spinlock.h"
#include"memory/memory.h"
#include"common.h"
#include"interrupt/handler.h"
#include"interrupt/controller/pic.h"
#include"assembly/assembly.h"

typedef struct TimerEvent{
	IORequest this;
	uint64_t countDownTicks;
	// period = 0 for one-shot timer
	uint64_t tickPeriod;
	volatile int isSentToTask;
	Spinlock *lock;
	struct TimerEvent **prev, *next;
}TimerEvent;

struct TimerEventList{
	Spinlock lock;
	TimerEvent *head;
};

static int cancelTimerEvent(IORequest *ior){
	TimerEvent *te = ior->timerEvent;
	acquireLock(te->lock);
	if(IS_IN_DQUEUE(te)){
		REMOVE_FROM_DQUEUE(te);
	}
	releaseLock(te->lock);
	// TODO: DELETE?
	return 1;
}

static int finishTimerEvent(IORequest *ior, __attribute__((__unused__)) uintptr_t *returnValues){
	if(ior->timerEvent->tickPeriod == 0){ // not periodic
		DELETE(ior->timerEvent);
	}
	else{
		acquireLock(ior->timerEvent->lock);
		putPendingIO(ior);
		ior->timerEvent->isSentToTask = 0;
		releaseLock(ior->timerEvent->lock);
	}
	return 0;
}

static TimerEvent *createTimerEvent(
	HandleIORequest callback, Task *t, uint64_t periodTicks
){
	TimerEvent *NEW(te);
	if(te == NULL){
		return NULL;
	}
	initIORequest(&te->this, te, callback, t, cancelTimerEvent, finishTimerEvent);
	te->countDownTicks = 0;
	te->tickPeriod = periodTicks;
	te->isSentToTask = 0;
	te->prev = NULL;
	te->next = NULL;
	return te;
}

static void addTimerEvent(TimerEventList* tel, uint64_t waitTicks, TimerEvent *te){
	te->countDownTicks = waitTicks;
	te->isSentToTask = 0;
	te->lock = &(tel->lock);
	acquireLock(&tel->lock);
	ADD_TO_DQUEUE(te, &(tel->head));
	releaseLock(&tel->lock);
}

// TODO:systemCall_sleep(uint64_t millisecond);
uintptr_t setAlarm(uint64_t millisecond, uint64_t isPeriodic){
	if(millisecond > 1000000000 * (uint64_t)1000){
		return IO_REQUEST_FAILURE;
	}
	const uint64_t tick = (millisecond * TIMER_FREQUENCY) / 1000;
	Task *t = processorLocalTask();
	TimerEvent *te = createTimerEvent(resumeTaskByIO, t, (isPeriodic? tick: 0));
	if(te == NULL){
		return IO_REQUEST_FAILURE;
	}
	IORequest *ior = &te->this;
	putPendingIO(ior);
	addTimerEvent(processorLocalTimer(), tick, te);
	return (uintptr_t)ior;
}

int sleep(uint64_t millisecond){
	uintptr_t te = setAlarm(millisecond, 0);
	if(te == IO_REQUEST_FAILURE){
		return 0;
	}
	IORequest *te2 = waitIO(processorLocalTask()); // TODO: if there are other pending requests?
	assert(((uintptr_t)te2) == te);
	uint32_t rv[1];
	int rvCount = te2->finish(te2, rv);
	assert(rvCount == 0);
	return 1;
}

static void handleTimerEvents(TimerEventList *tel){
	TimerEvent *periodList = NULL;
	acquireLock(&tel->lock);
	TimerEvent **prev = &(tel->head);
	while(*prev != NULL){
		TimerEvent *curr = *prev;
		if(curr->countDownTicks > 0){
			curr->countDownTicks--;
			prev = &((*prev)->next);
			continue;
		}
		REMOVE_FROM_DQUEUE(curr);
		if(curr->isSentToTask == 0){
			curr->isSentToTask = 1;
			curr->countDownTicks = curr->tickPeriod;
			curr->this.handle(&curr->this);
		}
#ifndef NDEBUG
		else{
			assert(curr->tickPeriod > 0);
			printk("warning: skip periodic timer event\n");
		}
#endif
		if(curr->tickPeriod > 0){
			ADD_TO_DQUEUE(curr, &periodList);
		}
	}
	APPEND_TO_DQUEUE(&periodList, prev);
	releaseLock(&tel->lock);
}

static void timerHandler(InterruptParam *p){
	// kprintf("interrupt #%d (timer), arg = %x\n", toChar(p.vector), p.argument);
	processorLocalPIC()->endOfInterrupt(p);
	handleTimerEvents((TimerEventList*)p->argument);
	schedule();
	sti();
}

TimerEventList *createTimer(){
	TimerEventList *NEW(tel);
	tel->lock = initialSpinlock;
	tel->head = NULL;
	return tel;
}

void setTimerHandler(TimerEventList *tel, InterruptVector *v){
	setHandler(v, timerHandler, (uintptr_t)tel);
}
