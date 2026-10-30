/*
 * scheduler.c - Scheduler round-robin de miniOS
 */

#include "scheduler.h"
#include "timer.h"
#include "monitor.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <libgen.h>

// Estado global del scheduler
static volatile int current_running = -1;   // indice en process_table del proceso RUNNING, -1 si ninguno
static volatile int scheduler_active = 0;   // 1 si el scheduler esta corriendo

// ============================================================
// Helpers ya implementados
// ============================================================

double timespec_diff_ms(struct timespec end, struct timespec start) {
    double sec = (double)(end.tv_sec - start.tv_sec);
    double nsec = (double)(end.tv_nsec - start.tv_nsec);
    return sec * 1000.0 + nsec / 1000000.0;
}

void scheduler_init(void) {
    process_count = 0;
    current_running = -1;
    scheduler_active = 0;
    rq_init();
}

int scheduler_get_running(void) {
    return current_running;
}

int scheduler_is_running(void) {
    return scheduler_active;
}

void scheduler_install_sigchld(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = scheduler_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
}

void scheduler_stop(void) {
    timer_stop();
    scheduler_active = 0;

    for (int i = 0; i < process_count; i++) {
        if (process_table[i].state != PROC_TERMINATED) {
            kill(process_table[i].pid, SIGKILL);
            int status;
            waitpid(process_table[i].pid, &status, 0);
            process_table[i].state = PROC_TERMINATED;
        }
    }
    current_running = -1;
}

// ============================================================
// 1/4 scheduler_create_process
// ============================================================
int scheduler_create_process(const char *path, const char *arg) {
    int status;

    // Paso 1: validar espacio
    if (process_count >= MAX_PROCESSES) {
        fprintf(stderr, "Error: tabla de procesos llena (max %d).\n", MAX_PROCESSES);
        return -1;
    }

    // Paso 2: fork
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }

    // Paso 3: HIJO
    if (pid == 0) {
        if (platform_uses_ptrace()) {
            platform_trace_child();
        }
        if (arg != NULL) {
            execl(path, path, arg, (char *)NULL);
        } else {
            execl(path, path, (char *)NULL);
        }
        perror("execl");
        _exit(1);
    }

    // Paso 4: PADRE espera el SIGTRAP post-exec
    if (platform_uses_ptrace()) {
        if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
    }

    // Paso 5: crear PCB
    int idx = process_count;
    char *path_copy = strdup(path);
    char *short_name = basename(path_copy);
    pcb_init(&process_table[idx], pid, short_name);
    free(path_copy);

    // Paso 6: registros iniciales y detach
    if (platform_uses_ptrace()) {
        if (platform_get_registers(pid, &process_table[idx].registers) == 0) {
            process_table[idx].regs_valid = 1;
        }
        platform_detach(pid);
    }

    // Paso 7: detener
    if (platform_stop_process(pid) != 0) {
        perror("platform_stop_process");
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return -1;
    }

    // Paso 8: confirmar que se detuvo
    if (waitpid(pid, &status, WUNTRACED) < 0) {
        perror("waitpid");
        kill(pid, SIGKILL);
        return -1;
    }

    // Paso 9: marcar READY y encolar
    process_table[idx].state = PROC_READY;
    process_count++;
    rq_enqueue(idx);
    monitor_emit_created(pid, process_table[idx].name);
    if (process_table[idx].regs_valid) {
        monitor_emit_registers(pid,
                               process_table[idx].registers.program_counter,
                               process_table[idx].registers.stack_pointer);
    }
    return idx;
}

// ============================================================
// 2/4 scheduler_start
// ============================================================
void scheduler_start(int slice_ms) {
    if (rq_is_empty()) {
        printf("No hay procesos en la ready queue.\n");
        return;
    }

    int idx = rq_dequeue();

    process_table[idx].state = PROC_RUNNING;
    clock_gettime(CLOCK_MONOTONIC, &process_table[idx].last_started);
    current_running = idx;

    platform_resume_process(process_table[idx].pid);

    scheduler_active = 1;
    timer_init(slice_ms, scheduler_tick);
    timer_start();
}

// ============================================================
// 3/4 scheduler_tick (handler de SIGALRM)
// ============================================================
void scheduler_tick(int signum) {
    (void)signum;

    if (current_running < 0 || !scheduler_active) {
        return;
    }

    pcb_t *current = &process_table[current_running];

    platform_stop_process(current->pid);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = timespec_diff_ms(now, current->last_started);

    // Los procesos que esperaban en la cola acumulan tiempo de espera
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].state == PROC_READY) {
            process_table[i].wait_time_ms += elapsed;
        }
    }

    current->cpu_time_ms += elapsed;
    current->state = PROC_READY;
    current->context_switches++;

    rq_enqueue(current_running);

    if (rq_is_empty()) {
        current_running = -1;
        timer_stop();
        return;
    }

    int next_idx = rq_dequeue();
    pcb_t *next = &process_table[next_idx];
    next->state = PROC_RUNNING;
    clock_gettime(CLOCK_MONOTONIC, &next->last_started);
    platform_resume_process(next->pid);
    monitor_emit_switch(current->pid, next->pid, timer_get_slice());
    current_running = next_idx;
}

// ============================================================
// 4/4 scheduler_sigchld (handler de SIGCHLD)
// ============================================================
void scheduler_sigchld(int signum) {
    (void)signum;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        // Ignorar paradas: solo nos interesan las terminaciones
        if (!WIFEXITED(status) && !WIFSIGNALED(status)) {
            continue;
        }

        // Buscar el PID en la tabla
        int i;
        for (i = 0; i < process_count; i++) {
            if (process_table[i].pid == pid) break;
        }
        if (i == process_count || process_table[i].state == PROC_TERMINATED) {
            continue;
        }

        int was_running = (i == current_running);

        // Si era el que corria, sumar su ultimo tramo de CPU
        if (was_running) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = timespec_diff_ms(now, process_table[i].last_started);
            process_table[i].cpu_time_ms += elapsed;

            for (int k = 0; k < process_count; k++) {
                if (process_table[k].state == PROC_READY) {
                    process_table[k].wait_time_ms += elapsed;
                }
            }
        }

        process_table[i].state = PROC_TERMINATED;
        monitor_emit_terminated(pid, process_table[i].cpu_time_ms,
                                process_table[i].context_switches);

        if (was_running) {
            current_running = -1;
            if (!rq_is_empty()) {
                int next = rq_dequeue();
                process_table[next].state = PROC_RUNNING;
                clock_gettime(CLOCK_MONOTONIC, &process_table[next].last_started);
                platform_resume_process(process_table[next].pid);
                current_running = next;
            } else {
                timer_stop();
                scheduler_active = 0;
            }
        } else {
            rq_remove(i);
        }
    }
}