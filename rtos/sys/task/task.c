/**
 * @file task.h
 * Implements task creation, destruction, and scheduling
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <config.h>
#include <drivers/clock/clock.h>
#include <drivers/device/device.h>
#include <sys/err.h>
#include <sys/isr/isr.h>
#include <util/bitmask.h>
#include <util/list/list.h>
#include <util/logging/logging.h>

#include "task.h"

#define reg_t volatile uint32_t

/* Initial task register states */
#define INITIAL_xPSR 0x01000000 // T bit is set in EPSR (thumb instructions)
#define INITIAL_EXEC_RETURN 0xFFFFFFFD // Thread mode with process stack

/**
 * Task state enum
 */
typedef enum task_state {
    TASK_EXITED,  /*!< Task exited */
    TASK_BLOCKED, /*!< Task blocked and cannot run */
    TASK_READY,   /*!< Task is ready but not running */
    TASK_ACTIVE,  /*!< Task is running */
} task_state_t;

/**
 * Task control block. Keeps task status and recordkeeping information.
 */
typedef struct task_status {
    uint32_t *stack_ptr;       /*!< Task stack pointer. MUST be first entry*/
    char *stack_start;         /*!< Task stack start */
    char *stack_end;           /*!< End of task stack */
    void (*entry)(void *);     /*!< task entry point */
    void *arg;                 /*!< Task argument */
    task_state_t state;        /*!< state of task */
    const char *name;          /*!< Task name */
    bool stack_allocated;      /*!< Was the stack allocated? */
    block_reason_t blockcause; /*!< cause for task block */
    uint32_t priority;         /*!< Task priority */
    list_state_t list_state;   /*!< Task list state */
} task_status_t;

// Task control block lists
static task_status_t *active_task = NULL;
static list_t ready_tasks[RTOS_PRIORITY_COUNT] = {NULL};
static list_t blocked_tasks = NULL;
static list_t exited_tasks = NULL;

// Logging tag
const char *TAG = "task.c";
// Idle task name
const char *IDLE_TASK_NAME = "Idle Task";

// Static functions
static inline void set_pendsv();
static inline void trigger_svcall();
static void idle_entry(void *arg);
static uint32_t *initialize_task_stack(uint32_t *stack_ptr, void *return_pc,
                                       void *arg0);
static void task_exithandler();

/**
 * Creates a system task. Requires memory allocation to be enabled to succeed.
 * Task will be scheduled, but will not start immediately.
 * @param entry: task entry point. Must be a function taking a void* and
 * returning void
 * @param arg: task argument. May be NULL. Will be passed to the task entry
 * point function
 * @param cfg: task configuration structure. May be NULL
 * @return created task handle on success, or NULL on error
 */
task_handle_t task_create(void (*entry)(void *), void *arg,
                          task_config_t *cfg) {
    task_status_t *task;
    // Check parameters
    if (entry == NULL) {
        return NULL;
    }
    task = malloc(sizeof(task_status_t));
    if (task == NULL) {
        return NULL;
    }
    // Allocate task block
    if (cfg == NULL) {
        // Set default task parameters
        task->priority = DEFAULT_PRIORITY;
        task->name = "";
        task->stack_end = malloc(DEFAULT_STACKSIZE);
        task->stack_allocated = true;
        if (task->stack_end == NULL) {
            free(task);
            return NULL;
        }
        task->stack_start = task->stack_end + (DEFAULT_STACKSIZE - 1);
    } else {
        // Check priority
        if (cfg->task_priority > RTOS_PRIORITY_COUNT) {
            return NULL;
        }
        // Check if a stack was provided
        if (cfg->task_stack) {
            task->stack_end = cfg->task_stack;
            task->stack_allocated = false;
        } else {
            task->stack_end = malloc(cfg->task_stacksize);
            task->stack_allocated = true;
            if (task->stack_end == NULL) {
                free(task);
                return NULL;
            }
        }
        if (cfg->task_name) {
            task->name = cfg->task_name;
        } else {
            // Default value
            task->name = "";
        }
        // Calculate start of stack
        task->stack_start = task->stack_end + (cfg->task_stacksize - 1);
        task->priority = cfg->task_priority;
    }
    // Update task state and place in ready queue
    task->blockcause = BLOCK_NONE;
    task->state = TASK_READY;
    task->entry = entry;
    task->arg = arg;
    // Initialize task stack
    task->stack_ptr = initialize_task_stack((uint32_t *)task->stack_start,
                                            task->entry, task->arg);
    // Place this task into the ready queue (scheduler can select it)
    ready_tasks[task->priority] =
        list_append(ready_tasks[task->priority], task, &(task->list_state));
    if (ready_tasks[task->priority] == NULL) {
        LOG_E(TAG, "Could not append new task to ready list");
        free(task);
        if (task->stack_allocated) {
            free(task->stack_start);
        }
        return NULL;
    }
    // Return task handle
    return (task_handle_t)task;
};

/**
 * Starts the real time operating system. This function will not return.
 *
 * Once the RTOS starts, scheduled tasks will start executing based on priority.
 * If no tasks are scheduled, this function will essentially freeze the system
 * in the idle task.
 */
void rtos_start() {
    task_handle_t idle_task;
    task_config_t idle_task_cfg = DEFAULT_TASK_CONFIG;
    /* Create a task control block for idle process */
    idle_task_cfg.task_name = IDLE_TASK_NAME;
    idle_task_cfg.task_priority = IDLE_TASK_PRIORITY;
    idle_task_cfg.task_stacksize = IDLE_TASK_STACK_SIZE;
    idle_task = task_create(idle_entry, NULL, &idle_task_cfg);
    if (!idle_task) {
        LOG_E(TAG, "Could not create idle task");
        exit(ERR_SCHEDULER);
    }
    // Trigger an SVCall to start the scheduler. Will not return.
    trigger_svcall();
    LOG_E(TAG, "Scheduler returned without starting RTOS");
    exit(ERR_SCHEDULER);
}

/**
 * Yields task execution. This function will stop execution of the current
 * task, and yield execution to the highest priority task able to run
 */
void task_yield() {
    // Mark task as ready, not active
    active_task->state = TASK_READY;
    // Trigger a system context switch switch by setting pendsv bit
    set_pendsv();
}

/**
 * Destroys a task. Will stop task execution immediately.
 * @param task: Task handle to destroy
 */
void task_destroy(task_handle_t task) {
    task_status_t *tsk = (task_status_t *)task;
    // Check if the task handle is the active one
    if (tsk == active_task) {
        /**
         * We cannot free this task. Instead, place it in exited task list.
         * idle task will reap resources.
         */
        exited_tasks = list_append(exited_tasks, tsk, &(tsk->list_state));
        active_task = NULL;
        // Trigger an SVCall to switch to a new active task (not context switch)
        trigger_svcall();
    } else {
        // Remove task from list it is in
        if (tsk->state == TASK_BLOCKED) {
            blocked_tasks = list_remove(blocked_tasks, &(tsk->list_state));
        } else if (tsk->state == TASK_READY) {
            ready_tasks[tsk->priority] =
                list_remove(ready_tasks[tsk->priority], &(tsk->list_state));
        } else {
            LOG_W(TAG,
                  "Inactive destroyed task is not in blocked or ready list");
        }
        // Free resources of active task
        if (tsk->stack_allocated) {
            free(tsk->stack_end);
        }
        free(tsk);
    }
}

/**
 * Gets the active task. Used by system drivers
 * @return handle to active task
 */
task_handle_t get_active_task() { return (task_handle_t)active_task; }

/**
 * Blocks the running task, and switches to a new runnable one. This function
 * does not return. Used by system drivers.
 * @param reason: reason for task block
 */
void block_active_task(block_reason_t reason) {
    /**
     * Set block reason of task, and fire context switch. Context switch handler
     * will move task to correct list.
     */
    active_task->state = TASK_BLOCKED;
    active_task->blockcause = reason;
    set_pendsv();
}

/**
 * Unblocks a task. Caller must give correct reason task was blocked. If
 * reason is incorrect, this call has no effect. Used by system drivers.
 * Task will not run immediately unless it has higher priority than running task
 * and preemption is enabled.
 * @param task: task to unblock
 * @param reason: reason task was blocked.
 */
void unblock_task(task_handle_t task, block_reason_t reason) {
    task_status_t *tsk = (task_status_t *)task;
    /**
     * Ensure task block reason matches provided reason
     */
    if (tsk->state != TASK_BLOCKED || tsk->blockcause != reason) {
        return;
    }
    // Set task as ready
    tsk->state = TASK_READY;
    tsk->blockcause = BLOCK_NONE;
    // Move task to ready list
    blocked_tasks = list_remove(blocked_tasks, &(tsk->list_state));
    ready_tasks[tsk->priority] =
        list_append(ready_tasks[tsk->priority], tsk, &(tsk->list_state));
}

/**
 * SVCall handler. Enables the system tick, switches the processor to the
 * process stack, and starts the RTOS scheduler.
 *
 * This function SHOULD NOT BE CALLED BY THE USER. It is indended to run in
 * Handler mode, as the SVCall isr
 */
__attribute__((naked)) void SVCallHandler() {
    /**
     * This is a naked function, so that GCC will not generate prologue and
     * epilogue code, which can leave the stack in an invalid state when
     * using bx instructions
     */
    asm volatile(
        /* Reset the main stack pointer to initial value. */
        "lsr r0, %[VTOR], #0x7\n" // get exception vector address from VTOR
        "ldr r1, [r0]\n"          // load initial stack pointer from vectors
        "msr MSP, r1\n"           // set main stack pointer to initial value
        /* Select an active task to run, and enable systick */
        "cpsid i\n"               // Set primask to 1 to disable interrupts
        "stmfd sp!, {r0-r3}\n"    // Save caller saved regs to main stack
        "bl select_active_task\n" // break to function to select new active task
        "bl enable_systick\n"  // break to function to enable systick interrupt
        "ldmfd sp!, {r0-r3}\n" // Restore registers after function calls
        "cpsie i\n"            // Set primask to 0 to enable interrupts
        /* Active task now set. Restore its register state and switch to it */
        "ldr r0, %[active_task]\n" // Load active task struct
        "ldr r1, [r0]\n" // Load address of top of stack for active task
        /* Restore register state for task */
        "ldmfd r1!, {r4-r11, lr}\n" // Restore calle-saved registers
        "msr PSP, r1\n" // Load new stack pointer after restoring register
        /* Task lr value will force return into thread mode with psp enabled */
        /* Loading EXEC_RETURN value in $lr reg will force exception to exit */
        "bx lr\n" // Load EXEC_RETURN value into PC. Core will intercept call
        :
        : [ VTOR ] "r"(SCB->VTOR), [ active_task ] "m"(active_task));
}

/**
 * System context switch handler. Stores core registers for current
 * execution context, then selects the highest priority ready task to run.
 *
 * This function SHOULD NOT BE CALLED BY THE USER. It is indended to run in
 * Handler mode, as the PendSV isr
 */
__attribute__((naked)) void PendSVHandler() {
    /**
     * This is a naked function, so that GCC will not generate prologue and
     * epilogue code, which can leave the stack in an invalid state when
     * using bx instructions
     */
    /**
     * Save the context of the currently running task.
     * Assumes task is running with process stack
     */
    asm volatile(
        "mrs r0, psp\n"            // Load process stack pointer to r0
        "mov r1, %[active_task]\n" // Store memory address of active task
        "ldr r3, [r1]\n"           // Load value of stack_ptr

        "stmfd r0!, {r4-r11, lr}\n" // Save calle-saved registers
        "str r0, [r3]\n"            // Store the new top of the stack

        "cpsid i\n"               // Disable interrupts (set PRIMASK to 1)
        "stmfd sp!, {r0-r3}\n"    // Save caller saved regs to main stack
        "bl select_active_task\n" // Call function to select new active task
        "ldmfd sp!, {r0-r3}\n"    // Restore registers after function call
        "cpsie i\n"               // Reenable interrupt

        "ldr r3, [r1]\n" // Reload address of active task
        "ldr r2, [r3]\n" // Reload stack_ptr from active_task

        "ldmfd r2!, {r4-r11, lr}\n" // Restore calle-saved registers for task
        "msr psp, r2\n"             // Load r2 as the stack pointer

        "bx lr\n" // Exception return. Core will intercept load of
                  // 0xFXXXXXXX to PC and return from exception
        :
        : [ active_task ] "r"(&active_task));
}

/**
 * System tick handler. Handles periodic RTOS tasks, such as checking to see
 * if blocked tasks are now unblocked, and preempting tasks if enabled.
 *
 * This function SHOULD NOT BE CALLED BY THE USER. It is indended to run in
 * Handler mode, as the PendSV isr
 */
void SysTickHandler() {
    /**
     * TODO: check if preemption should occur (if enabled), and what tasks
     * are runnable
     */
    // asm volatile("bkpt"); // Temporary, for debugging
}

/**
 * This function should ONLY be called by internal routines.
 * Selects new active task from all the tasks in ready lists. Selects the
 * highest priority task available to run.
 * Does not update active task state, but will refer to task state when
 * placing it into blocked/ready list. Does update active task state.
 */
void select_active_task() {
    int i;
    task_status_t *new_active;
    /**
     * Examine task lists to find the highest priority list with tasks ready
     * to run
     */
    for (i = RTOS_PRIORITY_COUNT - 1; i > 0; i--) {
        // If task list has tasks, break
        if (ready_tasks[i] != NULL)
            break;
    }
    if (ready_tasks[i] == NULL) {
        /**
         * There is only one task (idle task). It should be active task, so just
         * leave it running
         */
        return;
    }
    // Select the head of this ready task list
    new_active = list_get_head(ready_tasks[i]);
    ready_tasks[i] = list_remove(ready_tasks[i], &(new_active->list_state));
    if (active_task != NULL) { // active task will be null on scheduler start
        /**
         * Based on the block state of the active task, store it in a blocked
         * list or the ready list
         */
        if (active_task->state == TASK_BLOCKED) {
            blocked_tasks = list_append(blocked_tasks, active_task,
                                        &(active_task->list_state));
        } else {
            // Append active task to appropriate ready list
            ready_tasks[active_task->priority] =
                list_append(ready_tasks[active_task->priority], active_task,
                            &(active_task->list_state));
        }
    }
    // Change the active task
    active_task = new_active;
    active_task->state = TASK_ACTIVE;
}

/**
 * This function should ONLY be called by internal routines.
 * Enables the system tick interrupt.
 */
void enable_systick() {
    uint32_t reload_val;
    /**
     * The stm32l433 defaults to sourcing the systick clock as HCLK divided
     * by 8. We must set the reload value (24 bits) to achieve the desired
     * systick rate of 5ms
     */
    reload_val = (hclk_freq() >> 3) / SYSTICK_FREQ;
    if (reload_val > SysTick_LOAD_RELOAD_Msk) {
        LOG_E(TAG, "Oversized systick reload value");
        exit(ERR_BADPARAM);
    }
    // Set the reload value (interrupt fires when counting from 1 to 0)
    SysTick->LOAD = reload_val - 1;
    // Enable the systick interrupt
    SETBITS(SysTick->CTRL, SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk);
}

/**
 * Initializes a task stack for use with the scheduler
 * @param stack_ptr: pointer to start of stack to initialize
 * @param return_pc: address to set program counter to when task starts (usually
 * task entry point)
 * @param arg0: assigned to r0, argument to pass to task entry function
 * @return new stack pointer after initialization
 */
static uint32_t *initialize_task_stack(uint32_t *stack_ptr, void *return_pc,
                                       void *arg0) {
    /**
     * Memory access to stack pointer must be WORD aligned. If pointer is not
     * word aligned, just loose a few stack bytes until it is.
     */
    while ((((uint32_t)stack_ptr) % 4) != 0) {
        // Move stack pointer by a byte
        stack_ptr = (uint32_t *)(((uint8_t *)stack_ptr) - 1);
    }
    /**
     * Process stacks are saved with registers in the following order
     * (from largest to smallest address):
     * xPSR, ReturnAddress, LR (saved by exception), R12, R3, R2, R1 R0,
     * LR (saved by context switch), R11, R10, R9, R8, R6, R5, R4
     */
    *stack_ptr-- = INITIAL_xPSR; // (xPSR) inital PSR value
    // Exception will return to task entry
    *stack_ptr-- = (uint32_t)return_pc; // (ReturnAddress)
    // Return to task_exithandler if task exits
    *stack_ptr-- = (uint32_t)task_exithandler; // LR (exception)
    // Set general purpose registers to dummy values (idea from FreeRTOS)
    *stack_ptr-- = 0x12121212UL;        // R12
    *stack_ptr-- = 0x03030303UL;        // R3
    *stack_ptr-- = 0x02020202UL;        // R2
    *stack_ptr-- = 0x01010101UL;        // R1
    *stack_ptr-- = (uint32_t)arg0;      // (R0) Argument for task
    *stack_ptr-- = INITIAL_EXEC_RETURN; // (LR) EXEC_RETURN value
    *stack_ptr-- = 0x11111111UL;        // R11
    *stack_ptr-- = 0x10101010UL;        // R10
    *stack_ptr-- = 0x09090909UL;        // R9
    *stack_ptr-- = 0x08080808UL;        // R8
    *stack_ptr-- = 0x07070707UL;        // R7
    *stack_ptr-- = 0x06060606UL;        // R6
    *stack_ptr-- = 0x05050505UL;        // R5
    *stack_ptr = 0x04040404UL;          // R4 (do not decrement)
    return stack_ptr;
}

/**
 * Handles exit of task
 */
static void task_exithandler() {
    LOG_I(TAG, "Task named '%s' exited", active_task->name);
    task_destroy((task_handle_t)active_task);
}

/**
 * Idle loop. This task runs when no other tasks can.
 * @param arg: unused.
 */
static void idle_entry(void *arg) {
    task_status_t *task;
    while (1) {
        LOG_MIN(SYSLOGLEVEL_DEBUG, TAG, "Idle loop");
        /**
         * Reap resources of exited tasks
         */
        while (exited_tasks != NULL) {
            task = list_get_head(exited_tasks);
            exited_tasks = list_remove(exited_tasks, &(task->list_state));
            LOG_MIN(SYSLOGLEVEL_DEBUG, TAG, "Reaping task");
            // Free task and task stack
            if (task->stack_allocated) {
                free(task->stack_end);
            }
            free(task);
        }
        // Yield to another task
        task_yield();
    }
}

/**
 * Triggers a context switch via setting pendsv (will trigger pendsv
 * interrupt)
 */
static inline void set_pendsv() { SETBITS(SCB->ICSR, SCB_ICSR_PENDSVSET_Msk); }

/**
 * Triggers a task setup (essentially populating a task control block) via
 * an SVCall exception
 */
static inline void trigger_svcall() { asm volatile("svc 0"); }