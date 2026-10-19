//*кооперативний планувальник MerdenOS (round-robin, окремий стек на кожну задачу)
#include <stdint.h>
#include <stddef.h>

#ifdef SCHED_HOST_TEST
#include "sched_host.h"            /* заглушки для тесту на хості: kmalloc/kfree/час/паніка */
#else
#include "../headers/schluder.h"
#include "../headers/memory.h"
#include "../headers/kernel-panic.h"
#include "../headers/i2c_dw.h"  /* i2c_now() / i2c_cycles_per_ms(): годинник на TSC */
#endif

//*константи
#define STACK_FILL     0xA5u              /* патерн для виміру піку стеку */
#define STACK_GUARD    16u                /* байтів «канарки» на дні стеку */
#define GUARD_MAGIC    0xDEADC0DEu

//*одна задача
typedef struct {
    uint32_t     esp;                     /* має бути першим: switch_context пише сюди */
    uint8_t     *stack;                   /* NULL для головного потоку ядра */
    void       (*fn)(void);
    char         name[TASK_NAME_LEN];
    int          id;
    task_state_t state;
    uint64_t     wake_at;                 /* у тактах TSC (ділити 64-бітне без libgcc не можна) */
    uint32_t     runs;
} task_t;

//*загальні змінні
static task_t   tasks[MAX_TASKS];
static int      current   = 0;            /* індекс слота поточної задачі */
static int      next_id   = 1;
static int      live      = 0;            /* зайняті слоти (включно з зомбі) */
static uint32_t start_flags = 0;          /* EFLAGS, з якими стартує нова задача */

//*вимкнення/відновлення переривань (зараз ядро працює поллінгом, але це безпечно і для IRQ)
#ifndef SCHED_HOST_TEST
static inline uint32_t irq_save(void)
{
    uint32_t f;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(f) : : "memory");
    return f;
}
static inline void irq_restore(uint32_t f)
{
    if (f & 0x200u)
        __asm__ volatile ("sti" : : : "memory");
}
static inline void cpu_relax(void) { __asm__ volatile ("pause"); }
#endif

static uint64_t sched_now(void) { return i2c_now(); }

static void copy_name(char *dst, const char *src)
{
    int i = 0;
    if (!src) src = "task";
    while (src[i] && i < TASK_NAME_LEN - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

//*ініт планувальника: слот 0 — поточний потік ядра
void scheduler_init(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].esp     = 0;
        tasks[i].stack   = NULL;
        tasks[i].fn      = NULL;
        tasks[i].name[0] = 0;
        tasks[i].id      = -1;
        tasks[i].state   = TASK_FREE;
        tasks[i].wake_at = 0;
        tasks[i].runs    = 0;
    }

    copy_name(tasks[0].name, "kernel");
    tasks[0].id    = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].runs  = 1;

    current = 0;
    next_id = 1;
    live    = 1;
}

static void task_trampoline(void);

//*створення задачі
int add_tasks(void (*func)(void), const char *name)
{
    if (!func)
        return -1;

    uint32_t flags = irq_save();

    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_FREE) { slot = i; break; }
    }
    if (slot < 0) { irq_restore(flags); return -1; }

    uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!stack) { irq_restore(flags); return -1; }

    //*дно стеку: «канарка» (ловить переповнення), далі патерн для виміру піку
    uint32_t *guard = (uint32_t *)stack;
    for (uint32_t i = 0; i < STACK_GUARD / 4; i++) guard[i] = GUARD_MAGIC;
    for (uint32_t i = STACK_GUARD; i < TASK_STACK_SIZE; i++) stack[i] = STACK_FILL;

    //*стартовий кадр: [edi esi ebx ebp] [ret -> trampoline] [фальшиве повернення]
    //*на вході в trampoline (esp + 4) вирівняно на 16, як вимагає i386 ABI
    uint32_t *sp = (uint32_t *)(((uintptr_t)stack + TASK_STACK_SIZE) & ~(uintptr_t)15);
    *--sp = 0;                              /* адреса повернення для trampoline (не використовується) */
    *--sp = (uint32_t)(uintptr_t)task_trampoline;
    *--sp = 0;                              /* ebp */
    *--sp = 0;                              /* ebx */
    *--sp = 0;                              /* esi */
    *--sp = 0;                              /* edi */

    task_t *t = &tasks[slot];
    t->esp     = (uint32_t)(uintptr_t)sp;
    t->stack   = stack;
    t->fn      = func;
    copy_name(t->name, name);
    t->id      = next_id++;
    t->wake_at = 0;
    t->runs    = 0;
    t->state   = TASK_READY;
    live++;

    int id = t->id;
    irq_restore(flags);
    return id;
}

//*звільнити стеки задач, що завершилися (крім тієї, на чиєму стеку ми зараз)
static void reap(void)
{
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_ZOMBIE && i != current) {
            kfree(tasks[i].stack);
            tasks[i].stack = NULL;
            tasks[i].fn    = NULL;
            tasks[i].id    = -1;
            tasks[i].state = TASK_FREE;
            live--;
        }
    }
}

//*round-robin: шукаємо від наступного слота; сплячих, чий час вийшов, будимо
static int pick_next(void)
{
    uint64_t now = sched_now();

    for (int k = 1; k <= MAX_TASKS; k++) {
        int i = (current + k) % MAX_TASKS;      /* k == MAX_TASKS дає саму поточну */
        task_t *t = &tasks[i];

        if (t->state == TASK_SLEEPING && now >= t->wake_at)
            t->state = TASK_READY;
        if (t->state == TASK_READY)
            return i;
    }
    return -1;
}

static void check_guard(task_t *t)
{
    if (!t->stack)
        return;
    uint32_t *guard = (uint32_t *)t->stack;
    for (uint32_t i = 0; i < STACK_GUARD / 4; i++) {
        if (guard[i] != GUARD_MAGIC)
            kernel_panic(FOS_ERROR_UNKNOWN);   /* стек задачі переповнився */
    }
}

//*головна функція: стан поточної задачі вже виставлено (READY/SLEEPING/ZOMBIE), IRQ вимкнені;
//*caller_flags — EFLAGS від irq_save() викликача
static void schedule(uint32_t caller_flags)
{
    check_guard(&tasks[current]);
    reap();

    int n;
    while ((n = pick_next()) < 0)
        cpu_relax();                           /* усі сплять — чекаємо найближчого пробудження */

    if (n == current) {
        tasks[n].state = TASK_RUNNING;
        return;
    }

    task_t *old = &tasks[current];
    task_t *nw  = &tasks[n];

    nw->state = TASK_RUNNING;
    nw->runs++;
    current = n;

    start_flags = caller_flags;                /* нова задача стартує з тим самим станом IF, що й той, хто її запустив */

    switch_context(&old->esp, nw->esp);
}

//*точка входу нової задачі
static void task_trampoline(void)
{
    irq_restore(start_flags);
    tasks[current].fn();
    task_exit();
    for (;;) cpu_relax();                      /* сюди не доходимо */
}

void sched_yield(void)
{
    if (live <= 1)
        return;                                /* швидкий шлях: крім ядра нікого немає */

    uint32_t flags = irq_save();
    if (tasks[current].state == TASK_RUNNING)
        tasks[current].state = TASK_READY;
    schedule(flags);
    irq_restore(flags);
}

void task_sleep_ms(uint32_t ms)
{
    if (ms == 0) { sched_yield(); return; }

    uint32_t flags = irq_save();
    tasks[current].wake_at = sched_now() + (uint64_t)ms * i2c_cycles_per_ms();
    tasks[current].state   = TASK_SLEEPING;
    schedule(flags);
    irq_restore(flags);
}

void task_exit(void)
{
    if (current == 0)
        return;                                /* головний потік ядра завершитись не може */

    uint32_t flags = irq_save();               /* назад уже не повертаємось */
    tasks[current].state = TASK_ZOMBIE;
    schedule(flags);
    for (;;) cpu_relax();
}

//*зняття задачі за id (стара task_remove шукала не за тим і виходила після слота 0)
int task_remove(int id)
{
    if (id <= 0)
        return -1;

    uint32_t flags = irq_save();

    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_FREE || tasks[i].state == TASK_ZOMBIE || tasks[i].id != id)
            continue;

        if (i == current) {                    /* задача знімає саму себе */
            tasks[i].state = TASK_ZOMBIE;
            schedule(flags);
            for (;;) cpu_relax();
        }

        kfree(tasks[i].stack);                 /* чужий стек — звільняємо одразу */
        tasks[i].stack = NULL;
        tasks[i].fn    = NULL;
        tasks[i].id    = -1;
        tasks[i].state = TASK_FREE;
        live--;
        irq_restore(flags);
        return 0;
    }

    irq_restore(flags);
    return -1;
}

int task_current_id(void)    { return tasks[current].id; }
int scheduler_count(void)    { return live; }

const char *task_state_name(task_state_t s)
{
    switch (s) {
    case TASK_READY:    return "ready";
    case TASK_RUNNING:  return "running";
    case TASK_SLEEPING: return "sleeping";
    case TASK_ZOMBIE:   return "zombie";
    default:            return "free";
    }
}

int scheduler_info(int slot, sched_info_t *out)
{
    if (slot < 0 || slot >= MAX_TASKS || !out || tasks[slot].state == TASK_FREE)
        return 0;

    task_t *t = &tasks[slot];
    out->id         = t->id;
    out->name       = t->name;
    out->state      = t->state;
    out->runs       = t->runs;
    out->stack_size = t->stack ? TASK_STACK_SIZE : 0;
    out->stack_used = 0;

    if (t->stack) {                            /* пік = від дна до першого зміненого байта */
        uint32_t i = STACK_GUARD;
        while (i < TASK_STACK_SIZE && t->stack[i] == STACK_FILL) i++;
        out->stack_used = TASK_STACK_SIZE - i;
    }
    return 1;
}
