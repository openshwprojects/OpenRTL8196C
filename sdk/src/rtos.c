/*
 * rtos.c - Cooperative multi-tasking scheduler & software timers for RTL8196C.
 *
 * Task 0 represents the main() boot thread (using the hardware boot stack).
 * Additional tasks created via xTaskCreate / rtos_create_thread receive
 * heap-allocated stacks (minimum 24 KB) and switch cooperatively whenever
 * any task calls vTaskDelay / rtos_delay_milliseconds / delay_ms / rtos_yield.
 */
#include "rtos.h"
#include "timer.h"
#include "heap.h"
#include "libc.h"

#define MAX_TASKS  24
#define MAX_TIMERS 16
#define MIN_TASK_STACK_BYTES (24u * 1024u)

enum task_state {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_SUSPENDED,
    TASK_ZOMBIE
};

struct rtos_ctx {
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7;
    uint32_t gp;
    uint32_t sp;
    uint32_t fp;
    uint32_t ra;
};

struct rtos_task {
    enum task_state state;
    uint32_t        handle_id;
    char            name[24];
    struct rtos_ctx ctx;
    uint8_t        *stack_base;
    uint32_t        stack_size;
    uint32_t        wake_ms;
};

extern void     rtos_switch_ctx(struct rtos_ctx *old_ctx, const struct rtos_ctx *new_ctx);
extern void     rtos_task_trampoline(void);
extern uint32_t rtos_get_gp(void);

static struct rtos_task tasks[MAX_TASKS];
static int current_task = 0;
static int rtos_started = 0;
static int in_bg_poll = 0;
static uint32_t next_task_gen = 1;
static void (*bg_poll_fn)(void) = 0;
static beken_timer_t *active_timers[MAX_TIMERS];

#define MAX_SYS_TIMEOUTS 8
struct sys_to_entry {
    int      active;
    uint32_t fire_ms;
    void   (*handler)(void *arg);
    void    *arg;
};
static struct sys_to_entry sys_timeouts[MAX_SYS_TIMEOUTS];

static int slot_from_handle(TaskHandle_t h)
{
    if (!h)
        return current_task;
    uint32_t id = (uint32_t)(uintptr_t)h;
    int slot = (int)(id & 0xFFu) - 1;
    if (slot <= 0 || slot >= MAX_TASKS)
        return -1;
    if (tasks[slot].state == TASK_UNUSED || tasks[slot].handle_id != id)
        return -1;
    return slot;
}

void rtos_init(void)
{
    if (rtos_started)
        return;
    memset(tasks, 0, sizeof(tasks));
    memset(active_timers, 0, sizeof(active_timers));
    memset(sys_timeouts, 0, sizeof(sys_timeouts));
    tasks[0].state = TASK_READY;
    tasks[0].handle_id = 1;
    strcpy(tasks[0].name, "main");
    tasks[0].wake_ms = 0;
    current_task = 0;
    rtos_started = 1;
}

void rtos_register_poll(void (*fn)(void))
{
    bg_poll_fn = fn;
}

static void reap_zombies(void)
{
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_ZOMBIE && i != current_task) {
            if (tasks[i].stack_base) {
                free(tasks[i].stack_base);
                tasks[i].stack_base = 0;
            }
            tasks[i].handle_id = 0;
            tasks[i].state = TASK_UNUSED;
        }
    }
}

static int in_timer_cb = 0;

static void pump_background(void)
{
    if (in_bg_poll)
        return;
    in_bg_poll = 1;

    if (bg_poll_fn)
        bg_poll_fn();

    in_bg_poll = 0;

    if (in_timer_cb)
        return;
    in_timer_cb = 1;

    uint32_t now = timer_ms();
    for (int i = 0; i < MAX_TIMERS; i++) {
        beken_timer_t *t = active_timers[i];
        if (t && t->running && t->cb && (int32_t)(now - t->next_fire_ms) >= 0) {
            t->next_fire_ms = now + t->period_ms;
            t->cb(t->arg);
        }
    }
    for (int i = 0; i < MAX_SYS_TIMEOUTS; i++) {
        if (sys_timeouts[i].active && (int32_t)(now - sys_timeouts[i].fire_ms) >= 0) {
            void (*fn)(void *) = sys_timeouts[i].handler;
            void *a            = sys_timeouts[i].arg;
            sys_timeouts[i].active = 0;
            if (fn)
                fn(a);
        }
    }

    in_timer_cb = 0;
}

void rtos_yield(void)
{
    if (!rtos_started)
        rtos_init();
    if (in_bg_poll)
        return;

    pump_background();
    reap_zombies();

    if (in_timer_cb)
        return;

    uint32_t now = timer_ms();
    for (int step = 1; step <= MAX_TASKS; step++) {
        int idx = (current_task + step) % MAX_TASKS;
        if (tasks[idx].state == TASK_READY &&
            (int32_t)(now - tasks[idx].wake_ms) >= 0) {
            if (idx != current_task) {
                int prev = current_task;
                current_task = idx;
                rtos_switch_ctx(&tasks[prev].ctx, &tasks[idx].ctx);
                reap_zombies();
            }
            return;
        }
    }
}

void rtos_task_exit(void)
{
    tasks[current_task].state = TASK_ZOMBIE;
    tasks[current_task].handle_id = 0;
    for (;;) {
        pump_background();
        uint32_t now = timer_ms();
        for (int step = 1; step < MAX_TASKS; step++) {
            int idx = (current_task + step) % MAX_TASKS;
            if (tasks[idx].state == TASK_READY &&
                (int32_t)(now - tasks[idx].wake_ms) >= 0) {
                int prev = current_task;
                current_task = idx;
                rtos_switch_ctx(&tasks[prev].ctx, &tasks[idx].ctx);
            }
        }
    }
}

BaseType_t xTaskCreate(TaskFunction_t pxTaskCode, const char * const pcName,
                       uint32_t usStackDepth, void * const pvParameters,
                       UBaseType_t uxPriority, TaskHandle_t * const pxCreatedTask)
{
    (void)uxPriority;
    if (!rtos_started)
        rtos_init();
    reap_zombies();

    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;

    uint32_t stack_bytes = usStackDepth * 4u;
    if (stack_bytes < MIN_TASK_STACK_BYTES)
        stack_bytes = MIN_TASK_STACK_BYTES;

    uint8_t *stack = (uint8_t *)malloc(stack_bytes);
    if (!stack)
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;

    memset(stack, 0, stack_bytes);
    struct rtos_task *t = &tasks[slot];
    memset(t, 0, sizeof(*t));
    uint32_t hid  = (next_task_gen++ << 8) | (uint32_t)(slot + 1);
    t->state      = TASK_READY;
    t->handle_id  = hid;
    t->stack_base = stack;
    t->stack_size = stack_bytes;
    t->wake_ms    = timer_ms();
    if (pcName)
        strncpy(t->name, pcName, sizeof(t->name) - 1);

    uintptr_t sp_top = ((uintptr_t)stack + stack_bytes) & ~(uintptr_t)7u;
    sp_top -= 32u; /* MIPS O32 ABI 16-byte arg area + alignment padding */

    t->ctx.s0 = (uint32_t)(uintptr_t)pxTaskCode;
    t->ctx.s1 = (uint32_t)(uintptr_t)pvParameters;
    t->ctx.gp = rtos_get_gp();
    t->ctx.sp = (uint32_t)sp_top;
    t->ctx.fp = (uint32_t)sp_top;
    t->ctx.ra = (uint32_t)(uintptr_t)rtos_task_trampoline;

    if (pxCreatedTask)
        *pxCreatedTask = (TaskHandle_t)(uintptr_t)hid;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t xTaskToDelete)
{
    int slot = slot_from_handle(xTaskToDelete);
    if (slot <= 0 || slot >= MAX_TASKS)
        return;
    if (slot == current_task) {
        rtos_task_exit();
    } else {
        if (tasks[slot].stack_base) {
            free(tasks[slot].stack_base);
            tasks[slot].stack_base = 0;
        }
        tasks[slot].handle_id = 0;
        tasks[slot].state = TASK_UNUSED;
    }
}

void vTaskSuspend(TaskHandle_t xTaskToSuspend)
{
    int slot = slot_from_handle(xTaskToSuspend);
    if (slot <= 0 || slot >= MAX_TASKS)
        return;
    tasks[slot].state = TASK_SUSPENDED;
    if (slot == current_task) {
        while (tasks[slot].state == TASK_SUSPENDED) {
            pump_background();
            uint32_t now = timer_ms();
            for (int step = 1; step < MAX_TASKS; step++) {
                int idx = (current_task + step) % MAX_TASKS;
                if (tasks[idx].state == TASK_READY &&
                    (int32_t)(now - tasks[idx].wake_ms) >= 0) {
                    int prev = current_task;
                    current_task = idx;
                    rtos_switch_ctx(&tasks[prev].ctx, &tasks[idx].ctx);
                    break;
                }
            }
        }
    }
}

void vTaskResume(TaskHandle_t xTaskToResume)
{
    int slot = xTaskToResume ? slot_from_handle(xTaskToResume) : -1;
    if (slot <= 0 || slot >= MAX_TASKS)
        return;
    if (tasks[slot].state == TASK_SUSPENDED)
        tasks[slot].state = TASK_READY;
}

void rtos_delay_milliseconds(uint32_t num_ms)
{
    if (!rtos_started)
        rtos_init();

    uint32_t start = timer_ms();
    tasks[current_task].wake_ms = start + num_ms;

    do {
        rtos_yield();
    } while ((uint32_t)(timer_ms() - start) < num_ms);
}

void vTaskDelay(TickType_t xTicksToDelay)
{
    rtos_delay_milliseconds(xTicksToDelay);
}

TickType_t xTaskGetTickCount(void)
{
    return timer_ms();
}

void vTaskSetTimeOutState(TimeOut_t * const pxTimeOut)
{
    if (pxTimeOut) {
        pxTimeOut->xOverflowCount = 0;
        pxTimeOut->xTimeOnEntering = timer_ms();
    }
}

uint32_t rtos_get_time(void)
{
    return timer_ms();
}

void sys_msleep(uint32_t ms)
{
    rtos_delay_milliseconds(ms);
}

uint32_t sys_now(void)
{
    return timer_ms();
}

OSStatus rtos_create_thread(beken_thread_t *thread, uint8_t priority,
                            const char *name, beken_thread_function_t function,
                            uint32_t stack_size, beken_thread_arg_t arg)
{
    TaskHandle_t h = 0;
    BaseType_t rc = xTaskCreate(function, name, stack_size / 4u, arg, priority, &h);
    if (thread)
        *thread = h;
    return (rc == pdPASS) ? kNoErr : kGeneralErr;
}

OSStatus rtos_delete_thread(beken_thread_t *thread)
{
    TaskHandle_t h = thread ? *thread : 0;
    if (thread)
        *thread = 0;
    vTaskDelete(h);
    return kNoErr;
}

OSStatus rtos_suspend_thread(beken_thread_t *thread)
{
    vTaskSuspend(thread ? *thread : 0);
    return kNoErr;
}

/* Cooperative mutexes */
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    uint32_t *m = (uint32_t *)malloc(sizeof(uint32_t));
    if (m) *m = 0;
    return (SemaphoreHandle_t)m;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xBlockTime)
{
    uint32_t *m = (uint32_t *)xSemaphore;
    if (!m) return pdTRUE;
    uint32_t start = timer_ms();
    while (*m) {
        if (xBlockTime != portMAX_DELAY && (uint32_t)(timer_ms() - start) >= xBlockTime)
            return pdFALSE;
        rtos_yield();
    }
    *m = 1;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t xSemaphore)
{
    uint32_t *m = (uint32_t *)xSemaphore;
    if (m) *m = 0;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t xSemaphore)
{
    if (xSemaphore)
        free(xSemaphore);
}

OSStatus rtos_init_mutex(beken_mutex_t *mutex)
{
    if (!mutex) return kGeneralErr;
    *mutex = xSemaphoreCreateMutex();
    return *mutex ? kNoErr : kGeneralErr;
}

OSStatus rtos_lock_mutex(beken_mutex_t *mutex)
{
    if (!mutex || !*mutex) return kNoErr;
    return xSemaphoreTake(*mutex, portMAX_DELAY) == pdTRUE ? kNoErr : kGeneralErr;
}

OSStatus rtos_unlock_mutex(beken_mutex_t *mutex)
{
    if (!mutex || !*mutex) return kNoErr;
    xSemaphoreGive(*mutex);
    return kNoErr;
}

OSStatus rtos_deinit_mutex(beken_mutex_t *mutex)
{
    if (mutex && *mutex) {
        vSemaphoreDelete(*mutex);
        *mutex = 0;
    }
    return kNoErr;
}

/* Software timers */
OSStatus rtos_init_timer(beken_timer_t *timer, uint32_t time_ms,
                         timer_handler_t function, void *arg)
{
    if (!timer) return kGeneralErr;
    timer->period_ms    = time_ms ? time_ms : 1;
    timer->next_fire_ms = timer_ms() + timer->period_ms;
    timer->cb           = function;
    timer->arg          = arg;
    timer->running      = 0;
    timer->initialized  = 1;

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (active_timers[i] == timer)
            return kNoErr;
    }
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!active_timers[i]) {
            active_timers[i] = timer;
            break;
        }
    }
    return kNoErr;
}

OSStatus rtos_start_timer(beken_timer_t *timer)
{
    if (!timer || !timer->initialized) return kGeneralErr;
    timer->next_fire_ms = timer_ms() + timer->period_ms;
    timer->running = 1;
    return kNoErr;
}

OSStatus rtos_stop_timer(beken_timer_t *timer)
{
    if (!timer) return kGeneralErr;
    timer->running = 0;
    return kNoErr;
}

OSStatus rtos_reload_timer(beken_timer_t *timer)
{
    if (!timer) return kGeneralErr;
    timer->next_fire_ms = timer_ms() + timer->period_ms;
    timer->running = 1;
    return kNoErr;
}

OSStatus rtos_deinit_timer(beken_timer_t *timer)
{
    if (!timer) return kGeneralErr;
    timer->running = 0;
    timer->initialized = 0;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (active_timers[i] == timer)
            active_timers[i] = 0;
    }
    return kNoErr;
}

int rtos_is_timer_running(beken_timer_t *timer)
{
    return timer ? timer->running : 0;
}

int rtos_is_timer_init(beken_timer_t *timer)
{
    return timer ? timer->initialized : 0;
}

void sys_timeout(uint32_t msecs, void (*handler)(void *arg), void *arg)
{
    if (!handler)
        return;
    for (int i = 0; i < MAX_SYS_TIMEOUTS; i++) {
        if (!sys_timeouts[i].active) {
            sys_timeouts[i].active  = 1;
            sys_timeouts[i].fire_ms = timer_ms() + msecs;
            sys_timeouts[i].handler = handler;
            sys_timeouts[i].arg     = arg;
            return;
        }
    }
}

void sys_untimeout(void (*handler)(void *arg), void *arg)
{
    for (int i = 0; i < MAX_SYS_TIMEOUTS; i++) {
        if (sys_timeouts[i].active &&
            sys_timeouts[i].handler == handler &&
            sys_timeouts[i].arg == arg) {
            sys_timeouts[i].active = 0;
        }
    }
}

