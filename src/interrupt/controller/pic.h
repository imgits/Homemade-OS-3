#include<std.h>

typedef struct MemoryManager MemoryManager;
typedef struct InterruptTable InterruptTable;
typedef union PIC PIC;
typedef struct TimerEventList TimerEventList;
typedef struct ProcessorLocal ProcessorLocal;
// use PIC8259 or APIC
PIC *initPIC(MemoryManager *m, InterruptTable *t, TimerEventList *timer, ProcessorLocal *pl);
