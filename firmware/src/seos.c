#include <platform.h>
#include <hostIntf.h>
#include <syscall.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <printf.h>
#include <eventQ.h>
#include <timer.h>
#include <stdio.h>
#include <seos.h>
#include <heap.h>
#include <slab.h>
#include <cpu.h>



/*
 * Since locking is difficult to do right for adding/removing listeners and such
 * since it can happen in interrupt context and not, and one such operation can
 * interrupt another, and we do have a working event queue, we enqueue all the
 * requests and then deal with them in the main code only when the event bubbles
 * up to the front of the quque. This allows us to not need locks around the
 * data structures.
 */

struct Task {
    /* pointers may become invalid. Tids do not. Zero tid -> not a valid task */
    uint32_t tid;

    uint16_t subbedEvtCount;
    uint16_t subbedEvtListSz;
    uint32_t *subbedEvents; /* NULL for invalid tasks */

    /* App entry points */
    struct AppEntry funcs;

    /* for some basic number of subbed events, the array is stored directly here. after that, a heap chunk is used */
    uint32_t subbedEventsInt[MAX_EMBEDDED_EVT_SUBS];
};

union DeferredAction {
    struct {
        uint32_t tid;
        uint32_t evt;
    } evtSub;
    struct {
        OsDeferCbkF callback;
        void *cookie;
    } deferred;
};

#define EVT_SUBSCRIBE_TO_EVT         0x00000000
#define EVT_UNSUBSCRIBE_TO_EVT       0x00000001
#define EVT_DEFERRED_CALLBACK        0x00000002



static struct EvtQueue *mEvtsInternal, *mEvtsExternal;
static struct SlabAllocator* mDeferedActionsSlab;
static struct Task mTasks[MAX_TASKS];
static uint32_t mNextTid = 1;

static void osInit(void)
{
    cpuInit();
    heapInit();
    platInitialize();

    osLog(LOG_INFO, "SEOS Initializing\n");

    /* init task list */
    memset(mTasks, 0, sizeof(mTasks));

    /* create the queues */
    if (!evtQueueSubsystemInit() || !(mEvtsInternal = evtQueueAlloc(512)) || !(mEvtsExternal = evtQueueAlloc(256))) {
        osLog(LOG_INFO, "events failed to init\n");
        return;
    }

    mDeferedActionsSlab = slabAllocatorNew(sizeof(union DeferredAction), 4, 32 /* for now? */);
    if (!mDeferedActionsSlab) {
        osLog(LOG_INFO, "deferred actions list failed to init\n");
        return;
    }
}

static void osStartTasks(void)
{
    extern struct AppEntry __app_start, __app_end;
    struct AppEntry *app;
    uint32_t i, nTasks = 0;

    osLog(LOG_INFO, "SEOS Registering tasks\n");
    for (app = &__app_start; app != &__app_end && nTasks < MAX_TASKS; app++, nTasks++) {
        memcpy(&mTasks[nTasks].funcs, app, sizeof(*app));
        mTasks[nTasks].subbedEvtListSz = MAX_EMBEDDED_EVT_SUBS;
        mTasks[nTasks].subbedEvents = mTasks[nTasks].subbedEventsInt;
        mTasks[nTasks].tid = mNextTid++;
    }

    osLog(LOG_INFO, "SEOS Starting tasks\n");
    for (i = 0; i < nTasks; i++)
        mTasks[i].funcs.start(mTasks[i].tid);
}

static struct Task* osTaskFindByTid(uint32_t tid)
{
    uint32_t i;

    for(i = 0; i < MAX_TASKS; i++)
        if (mTasks[i].tid == tid)
            return mTasks + i;

    return NULL;
}

static void osInternalEvtHandle(uint32_t evtType, void *evtData)
{
    union DeferredAction *da = (union DeferredAction*)evtData;
    struct Task *task;
    uint32_t i;

    switch (evtType) {
    case EVT_SUBSCRIBE_TO_EVT:
    case EVT_UNSUBSCRIBE_TO_EVT:
        /* get task */
        task = osTaskFindByTid(da->evtSub.tid);
        if (!task)
            break;

        /* find if subscribed to this evt */
        for (i = 0; i < task->subbedEvtCount && task->subbedEvents[i] != da->evtSub.evt; i++);

        /* if unsub & found -> unsub */
        if (evtType == EVT_UNSUBSCRIBE_TO_EVT && i != task->subbedEvtCount)
            task->subbedEvents[i] = task->subbedEvents[--task->subbedEvtCount];
        /* if sub & not found -> sub */
        else if (evtType == EVT_SUBSCRIBE_TO_EVT && i == task->subbedEvtCount) {
            if (task->subbedEvtListSz == task->subbedEvtCount) { /* enlarge the list */
                uint32_t newSz = (task->subbedEvtListSz * 3 + 1) / 2;
                uint32_t *newList = heapAlloc(sizeof(uint32_t[newSz])); /* grow by 50% */
                if (newList) {
                    memcpy(newList, task->subbedEvents, sizeof(uint32_t[task->subbedEvtListSz]));
                    if (task->subbedEvents != task->subbedEventsInt)
                        heapFree(task->subbedEvents);
                    task->subbedEvents = newList;
                    task->subbedEvtListSz = newSz;
                }
            }
            if (task->subbedEvtListSz > task->subbedEvtCount) { /* have space ? */
                task->subbedEvents[task->subbedEvtCount++] = da->evtSub.evt;
            }
        }
        break;

    case EVT_DEFERRED_CALLBACK:
        da->deferred.callback(da->deferred.cookie);
        break;
    }
}

static void osExpApiEvtqSubscribe(uintptr_t *retValP, va_list args)
{
    uint32_t tid = va_arg(args, uint32_t);
    uint32_t evtType = va_arg(args, uint32_t);

    *retValP = osEventSubscribe(tid, evtType);
}

static void osExpApiEvtqUnsubscribe(uintptr_t *retValP, va_list args)
{
    uint32_t tid = va_arg(args, uint32_t);
    uint32_t evtType = va_arg(args, uint32_t);

    *retValP = osEventUnsubscribe(tid, evtType);
}

static void osExpApiEvtqEnqueue(uintptr_t *retValP, va_list args)
{
    uint32_t evtType = va_arg(args, uint32_t);
    void *evtData = va_arg(args, void*);
    EventFreeF evtFreeF = va_arg(args, EventFreeF);
    bool external = va_arg(args, bool);

    *retValP = osEnqueueEvt(evtType, evtData, evtFreeF, external);
}

static void osExpApiEvtqFuncDeferCbk(void *data)
{
    struct UserspaceCallback *ucbk = (struct UserspaceCallback*)data;

    syscallUserspaceCallbackCall(ucbk, NULL, NULL, NULL, NULL);
    syscallUserspaceCallbackFree(ucbk);
}

static void osExpApiEvtqFuncDefer(uintptr_t *retValP, va_list args)
{
    OsDeferCbkF userCbk = va_arg(args, OsDeferCbkF);
    void *userData = va_arg(args, void*);
    struct UserspaceCallback *ucbk;

    *retValP = false;
    ucbk = syscallUserspaceCallbackAlloc(userCbk, (uintptr_t)userData, 0, 0, 0);
    if (ucbk) {
        if (osDefer(osExpApiEvtqFuncDeferCbk, ucbk))
            *retValP = true;
        else
            syscallUserspaceCallbackFree(ucbk);
    }
}

static void osExpApiLogLog(uintptr_t *retValP, va_list args)
{
    enum LogLevel level = va_arg(args, int /* enums promoted to ints in va_args in C */);
    const char *str = va_arg(args, const char*);

    osLogv(level, str, args);
}

static void osExportApi(void)
{
    static const struct SyscallTable osMainEvtqTable = {
        .numEntries = SYSCALL_OS_MAIN_EVTQ_LAST,
        .entry = {
            [SYSCALL_OS_MAIN_EVTQ_SUBCRIBE]   = { .func = osExpApiEvtqSubscribe,   },
            [SYSCALL_OS_MAIN_EVTQ_UNSUBCRIBE] = { .func = osExpApiEvtqUnsubscribe, },
            [SYSCALL_OS_MAIN_EVTQ_ENQUEUE]    = { .func = osExpApiEvtqEnqueue,     },
            [SYSCALL_OS_MAIN_EVTQ_FUNC_DEFER] = { .func = osExpApiEvtqFuncDefer,   },
        },
    };

    static const struct SyscallTable osMainLogTable = {
        .numEntries = SYSCALL_OS_MAIN_LOG_LAST,
        .entry = {
            [SYSCALL_OS_MAIN_LOG_LOG]   = { .func = osExpApiLogLog,   },
        },
    };

    static const struct SyscallTable osMainTable = {
        .numEntries = SYSCALL_OS_MAIN_LAST,
        .entry = {
            [SYSCALL_OS_MAIN_EVENTQ] =  { .subtable = (struct SyscallTable*)&osMainEvtqTable, },
            [SYSCALL_OS_MAIN_LOGGING] = { .subtable = (struct SyscallTable*)&osMainLogTable,  },
        },
    };
    static const struct SyscallTable osTable = {
        .numEntries = SYSCALL_OS_LAST,
        .entry = {
            [SYSCALL_OS_MAIN] = { .subtable = (struct SyscallTable*)&osMainTable, },
        },
    };

    if (!syscallAddTable(SYSCALL_DOMAIN_OS, 0, (struct SyscallTable*)&osTable))
        osLog(LOG_ERROR, "Failed to export OS base API");
}

void abort(void)
{
    /* this is necessary for va_* funcs... */
    osLog(LOG_ERROR, "Abort called");
    while(1);
}

void __attribute__((noreturn)) osMain(void)
{
    EventFreeF evtFree;
    uint32_t evtType, i, j;
    void *evtData;

    platDisableInterrupts();
    timInit();
    osInit();
    osExportApi();
    osStartTasks();
    hostIntfRequest();
    platEnableInterrupts();

    //broadcast boot completed
    (void)osEnqueueEvt(EVT_BOOT_COMPLETED, NULL, NULL, false);

    while (true) {

        /* get an event */
        if (!evtQueueDequeue(mEvtsInternal, &evtType, &evtData, &evtFree, true))
            continue;

        if (evtType < EVT_NO_FIRST_USER_EVENT) { /* no need for discardable check. all internal events arent discardable */
            /* handle deferred actions and other reserved events here */
            osInternalEvtHandle(evtType, evtData);
        }
        else {
            /* send this event to all tasks who want it (decimation could happen here) */
            for (i = 0; i < MAX_TASKS; i++) {
                if (!mTasks[i].subbedEvents) /* only check real tasks */
                    continue;
                for (j = 0; j < mTasks[i].subbedEvtCount; j++) {
                    if (mTasks[i].subbedEvents[j] == evtType) {
                        mTasks[i].funcs.handle(evtType, evtData);
                        break;
                    }
                }
            }
        }

        /* free it */
        if (evtFree)
            evtFree(evtData);
    }
}

static void osDeferredActionFreeF(void* event)
{
    slabAllocatorFree(mDeferedActionsSlab, event);
}

static bool osEventSubscribeUnsubscribe(uint32_t tid, uint32_t evtType, bool sub)
{
    union DeferredAction *act = slabAllocatorAlloc(mDeferedActionsSlab);

    if (!act)
        return false;
    act->evtSub.evt = evtType;
    act->evtSub.tid = tid;

    if (osEnqueueEvt(sub ? EVT_SUBSCRIBE_TO_EVT : EVT_UNSUBSCRIBE_TO_EVT, act, osDeferredActionFreeF, false))
        return true;

    slabAllocatorFree(mDeferedActionsSlab, act);
    return false;
}

bool osEventSubscribe(uint32_t tid, uint32_t evtType)
{
    return osEventSubscribeUnsubscribe(tid, evtType, true);
}

bool osEventUnsubscribe(uint32_t tid, uint32_t evtType)
{
    return osEventSubscribeUnsubscribe(tid, evtType, false);
}

bool osDefer(OsDeferCbkF callback, void *cookie)
{
    union DeferredAction *act = slabAllocatorAlloc(mDeferedActionsSlab);
    if (!act)
            return false;

    act->deferred.callback = callback;
    act->deferred.cookie = cookie;

    if (osEnqueueEvt(EVT_DEFERRED_CALLBACK, act, osDeferredActionFreeF, false))
        return true;

    slabAllocatorFree(mDeferedActionsSlab, act);
    return false;
}

bool osEnqueueEvt(uint32_t evtType, void *evtData, EventFreeF evtFreeF, bool external)
{
    return evtQueueEnqueue(external ? mEvtsExternal : mEvtsInternal, evtType, evtData, evtFreeF);
}

static bool osLogPutcharF(void* userData, char c)
{
    platLogPutchar(c);
    return true;
}

void osLogv(enum LogLevel level, const char *str, va_list vl)
{
    osLogPutcharF(NULL, level);
    cvprintf(osLogPutcharF, NULL, str, vl);
}

void osLog(enum LogLevel level, const char *str, ...)
{
    va_list vl;

    va_start(vl, str);
    osLogv(level, str, vl);
    va_end(vl);
}