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

static void cancelTimerEvent(IORequest *ior){
	TimerEvent *te = ior->instance;
	acquireLock(te->lock);
	if(IS_IN_DQUEUE(te)){ // not expire
		REMOVE_FROM_DQUEUE(te);
	}
	releaseLock(te->lock);
	DELETE(te);
}

static int finishTimerEvent(IORequest *ior, __attribute__((__unused__)) uintptr_t *returnValues){
	TimerEvent *te = ior->instance;
	if(te->tickPeriod == 0){ // not periodic
		DELETE(te);
	}
	else{
		acquireLock(te->lock);
		setCancellable(ior, 1);
		pendIO(ior);
		te->isSentToTask = 0;
		releaseLock(te->lock);
	}
	return 0;
}

static TimerEvent *createTimerEvent(uint64_t periodTicks){
	TimerEvent *NEW(te);
	if(te == NULL){
		return NULL;
	}
	initIORequest(&te->this, te, cancelTimerEvent, finishTimerEvent);
	te->countDownTicks = 0;
	te->tickPeriod = periodTicks;
	te->isSentToTask = 0;
	te->lock = NULL;
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

static void setAlarmHandler(InterruptParam *p){
	uint64_t millisecond = COMBINE64(SYSTEM_CALL_ARGUMENT_0(p), SYSTEM_CALL_ARGUMENT_1(p));
	uintptr_t isPeriodic = SYSTEM_CALL_ARGUMENT_2(p);
	EXPECT(millisecond <= 1000000000 * (uint64_t)1000);
	uint64_t tick = (millisecond * TIMER_FREQUENCY) / 1000;
	if(tick == 0)
		tick = 1;
	TimerEvent *te = createTimerEvent((isPeriodic? tick: 0));
	EXPECT(te != NULL);
	IORequest *ior = &te->this;
	setCancellable(ior, 1);
	pendIO(ior);
	addTimerEvent(processorLocalTimer(), tick, te);
	SYSTEM_CALL_RETURN_VALUE_0(p) = (uintptr_t)ior;
	return;
	ON_ERROR;
	ON_ERROR;
	SYSTEM_CALL_RETURN_VALUE_0(p) = IO_REQUEST_FAILURE;
}

uintptr_t systemCall_setAlarm(uint64_t millisecond, int isPeriodic){
	return systemCall4(SYSCALL_SET_ALARM, LOW64(millisecond), HIGH64(millisecond), (uintptr_t)isPeriodic);
}

int sleep(uint64_t millisecond){
	uintptr_t te = systemCall_setAlarm(millisecond, 0);
	if(te == IO_REQUEST_FAILURE){
		return 0;
	}
	uintptr_t te2 = systemCall_waitIO(te);
	assert(te2 == te);
	return 1;
}

static void handleTimerEvents(TimerEventList *tel){
	TimerEvent *periodicList = NULL;
	acquireLock(&tel->lock);
	TimerEvent **prev = &(tel->head);
	while(*prev != NULL){
		TimerEvent *curr = *prev;
		if(curr->countDownTicks > 0){
			curr->countDownTicks--;
			prev = &(curr->next);
			continue;
		}
		REMOVE_FROM_DQUEUE(curr);
		if(curr->isSentToTask == 0){
			curr->isSentToTask = 1;
			curr->countDownTicks = curr->tickPeriod;
			finishIO(&curr->this);
		}
#ifndef NDEBUG
		else{
			assert(curr->tickPeriod > 0);
			printk("warning: skip periodic timer event\n");
		}
#endif
		if(curr->tickPeriod > 0){
			ADD_TO_DQUEUE(curr, &periodicList);
		}
	}
	// append periodicList to *prev
	*prev = periodicList;
	if(periodicList != NULL){
		periodicList->prev = prev;
		//periodicList = NULL;
	}
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

void initTimer(SystemCallTable *systemCallTable){
	registerSystemCall(systemCallTable, SYSCALL_SET_ALARM, setAlarmHandler, 0);
}
