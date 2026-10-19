#ifndef SCHLUDER_H
#define SCHLUDER_H

#include <stdint.h>

/* Кооперативний планувальник з справжнім перемиканням контексту:
 * у кожної задачі свій стек і збережений esp. Задача віддає процесор
 * через sched_yield() / task_sleep_ms() або завершується (return / task_exit).
 * Слот 0 — це «kernel»: головний потік ядра (шелл / GUI), стек якого виділив boot.asm. */

#define MAX_TASKS        16        /* разом із головним потоком ядра */
#define TASK_STACK_SIZE  8192      /* байтів на задачу (виділяється з kmalloc) */
#define TASK_NAME_LEN    16

typedef enum {
    TASK_FREE = 0,                 /* слот вільний */
    TASK_READY,                    /* готова до запуску */
    TASK_RUNNING,                  /* зараз виконується */
    TASK_SLEEPING,                 /* спить до wake_at */
    TASK_ZOMBIE                    /* завершилась, стек ще не звільнено */
} task_state_t;

typedef struct {
    int          id;
    const char  *name;
    task_state_t state;
    uint32_t     runs;             /* скільки разів задачу ставили на процесор */
    uint32_t     stack_size;       /* 0 для головного потоку ядра */
    uint32_t     stack_used;       /* пік використання стеку, байтів */
} sched_info_t;

void  scheduler_init(void);                         /* викликати після kheap_init() */

/* Створює задачу. Повертає її id (>0) або -1, якщо немає слота / пам'яті. */
int   add_tasks(void (*func)(void), const char *name);

/* Знімає задачу за id. 0 — успіх, -1 — такої немає (або це головний потік). */
int   task_remove(int id);

void  task_exit(void);                              /* завершити поточну задачу */
void  sched_yield(void);                            /* віддати процесор наступній готовій */
void  task_sleep_ms(uint32_t ms);                   /* заснути, поки інші працюють */

int   task_current_id(void);
int   scheduler_count(void);                        /* скільки задач живе (з ядром) */
int   scheduler_info(int slot, sched_info_t *out);  /* 1, якщо слот зайнятий */
const char *task_state_name(task_state_t s);

/* switch.asm */
void  switch_context(uint32_t *old_esp, uint32_t new_esp);

#endif
