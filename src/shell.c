/*
 * shell.c - Shell interactivo de miniOS
 */

#include "shell.h"
#include "scheduler.h"
#include "timer.h"
#include "monitor.h"
#include "pcb.h"
#include "ready_queue.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_LINE 256

// ============================================================
// Helpers y comandos ya implementados
// ============================================================

// Block SIGALRM while modifying shared state
static sigset_t alarm_mask;

static void block_alarm(void) {
    sigprocmask(SIG_BLOCK, &alarm_mask, NULL);
}

static void unblock_alarm(void) {
    sigprocmask(SIG_UNBLOCK, &alarm_mask, NULL);
}

static void cmd_help(void) {
    printf("\nComandos disponibles:\n");
    printf("  run <binario>       Lanzar un proceso nuevo\n");
    printf("  ps                  Mostrar tabla de procesos\n");
    printf("  kill <pid>          Terminar un proceso\n");
    printf("  slice <ms>          Cambiar time slice (50-5000 ms)\n");
    printf("  inspect <pid>       Ver registros de un proceso\n");
    printf("  runpair <nombre>    Lanzar par de procesos comunicantes\n");
    printf("  stats               Mostrar metricas del scheduler\n");
    printf("  log on|off          Activar/desactivar emision de eventos JSON\n");
    printf("  help                Mostrar esta ayuda\n");
    printf("  exit                Terminar todos los procesos y salir\n");
    printf("\n");
}

static void cmd_slice(const char *arg) {
    if (!arg || strlen(arg) == 0) {
        printf("Time slice actual: %d ms\n", timer_get_slice());
        return;
    }

    int ms = atoi(arg);
    if (ms < 50 || ms > 5000) {
        printf("Error: time slice debe estar entre 50 y 5000 ms\n");
        return;
    }

    int old_ms = timer_get_slice();
    timer_set_slice(ms);
    if (scheduler_is_running()) {
        timer_start();
    }
    monitor_emit_slice_changed(old_ms, ms);
    printf("Time slice cambiado a %d ms\n", ms);
}

static void cmd_inspect(const char *arg) {
    if (!arg || strlen(arg) == 0) {
        printf("Uso: inspect <pid>\n");
        return;
    }

    int target_pid = atoi(arg);
    block_alarm();

    int found = 0;
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].pid == target_pid) {
            found = 1;
            printf("\n=== PCB de PID %d (%s) ===\n", target_pid, process_table[i].name);
            printf("  Estado:           %s\n", pcb_state_name(process_table[i].state));
            printf("  CPU Time:         %.1f ms\n", process_table[i].cpu_time_ms);
            printf("  Waiting Time:     %.1f ms\n", process_table[i].wait_time_ms);
            printf("  Context Switches: %d\n", process_table[i].context_switches);

            if (platform_registers_available() && process_table[i].regs_valid) {
                printf("\n  === Registros ===\n");
                printf("  Program Counter: 0x%016llx\n", (unsigned long long)process_table[i].registers.program_counter);
                printf("  Stack Pointer:   0x%016llx\n", (unsigned long long)process_table[i].registers.stack_pointer);
                for (int r = 0; r < NUM_GENERAL_REGS; r++) {
                    printf("  Reg[%2d]:         0x%016llx\n", r,
                           (unsigned long long)process_table[i].registers.general_regs[r]);
                }
            } else if (!platform_registers_available()) {
                printf("\n  [Registros no disponibles -- SIP habilitado en macOS]\n");
                printf("  Los registros estaran disponibles en WSL2/Linux.\n");
            } else {
                printf("\n  [Registros aun no capturados]\n");
            }
            printf("\n");
            break;
        }
    }

    if (!found) {
        printf("Proceso PID %d no encontrado.\n", target_pid);
    }

    unblock_alarm();
}

static void cmd_runpair(const char *name) {
    if (!name || strlen(name) == 0) {
        printf("Uso: runpair <nombre>\n");
        printf("  Pares disponibles: ping_pong, productor_consumidor\n");
        return;
    }

    char server_path[128], client_path[128], sock_path[128];

    if (strcmp(name, "ping_pong") == 0) {
        snprintf(server_path, sizeof(server_path), "programs/bin/ping_pong_server");
        snprintf(client_path, sizeof(client_path), "programs/bin/ping_pong_client");
        snprintf(sock_path, sizeof(sock_path), "/tmp/minios_pingpong.sock");
    } else if (strcmp(name, "productor_consumidor") == 0) {
        snprintf(server_path, sizeof(server_path), "programs/bin/productor");
        snprintf(client_path, sizeof(client_path), "programs/bin/consumidor");
        snprintf(sock_path, sizeof(sock_path), "/tmp/minios_prodcons.sock");
    } else {
        printf("Par desconocido: '%s'\n", name);
        printf("  Pares disponibles: ping_pong, productor_consumidor\n");
        return;
    }

    if (access(server_path, X_OK) != 0) {
        printf("Error: '%s' no encontrado. Ejecuta 'make programs' primero.\n", server_path);
        return;
    }
    if (access(client_path, X_OK) != 0) {
        printf("Error: '%s' no encontrado. Ejecuta 'make programs' primero.\n", client_path);
        return;
    }

    unlink(sock_path);
    printf("Lanzando par '%s' con socket %s\n", name, sock_path);

    int idx1 = scheduler_create_process(server_path, sock_path);
    if (idx1 < 0) return;

    int idx2 = scheduler_create_process(client_path, sock_path);
    if (idx2 < 0) return;

    if (!scheduler_is_running()) {
        scheduler_start(timer_get_slice());
    }
}

// ============================================================
// 1/4 cmd_run
// ============================================================
static void cmd_run(const char *path, const char *arg) {
    if (path == NULL || strlen(path) == 0) {
        printf("Uso: run <binario> [argumento]\n");
        return;
    }

    if (access(path, X_OK) != 0) {
        printf("Error: '%s' no existe o no es ejecutable.\n", path);
        return;
    }

    int idx = scheduler_create_process(path, arg);
    if (idx < 0) {
        return;
    }
    printf("Proceso '%s' creado (PID %d).\n", process_table[idx].name, process_table[idx].pid);

    if (!scheduler_is_running() && !rq_is_empty()) {
        scheduler_start(timer_get_slice());
    }
}

// ============================================================
// 2/4 cmd_ps
// ============================================================
static void cmd_ps(void) {
    block_alarm();

    if (process_count == 0) {
        printf("No hay procesos.\n");
        unblock_alarm();
        return;
    }

    printf("\n");
    pcb_print_table();
    printf("\n");
    rq_print();

    unblock_alarm();
}

// ============================================================
// 3/4 cmd_kill_proc
// ============================================================
static void cmd_kill_proc(const char *arg) {
    if (arg == NULL || strlen(arg) == 0) {
        printf("Uso: kill <pid>\n");
        return;
    }

    int target_pid = atoi(arg);
    if (target_pid <= 0) {
        printf("PID invalido: '%s'\n", arg);
        return;
    }

    block_alarm();

    int found = 0;
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].pid == target_pid &&
            process_table[i].state != PROC_TERMINATED) {
            found = 1;

            kill(target_pid, SIGKILL);

            // El handler de SIGCHLD marca TERMINATED, saca de la cola y
            // despacha al siguiente. Esperamos (max ~500 ms) a que lo haga.
            volatile pcb_t *p = &process_table[i];
            for (int t = 0; t < 500 && p->state != PROC_TERMINATED; t++) {
                usleep(1000);
            }

            // Respaldo por si el handler no llego a correr
            if (p->state != PROC_TERMINATED) {
                int status;
                waitpid(target_pid, &status, WNOHANG);
                process_table[i].state = PROC_TERMINATED;
                rq_remove(i);
            }

            printf("Proceso PID %d terminado.\n", target_pid);
            break;
        }
    }

    if (!found) {
        printf("Error: no existe un proceso activo con PID %d.\n", target_pid);
    }

    unblock_alarm();
}

// ============================================================
// 4/4 cmd_stats
// ============================================================
static void cmd_stats(void) {
    block_alarm();

    int active = 0, terminated = 0;
    double total_cpu = 0, total_wait = 0;
    int total_switches = 0;

    for (int i = 0; i < process_count; i++) {
        if (process_table[i].state == PROC_TERMINATED) {
            terminated++;
        } else {
            active++;
        }
        total_cpu += process_table[i].cpu_time_ms;
        total_wait += process_table[i].wait_time_ms;
        total_switches += process_table[i].context_switches;
    }

    printf("\n=== Estadisticas del Scheduler ===\n");
    printf("  Procesos activos:      %d\n", active);
    printf("  Procesos terminados:   %d\n", terminated);
    printf("  Time slice actual:     %d ms\n", timer_get_slice());
    printf("  CPU total acumulado:   %.1f ms\n", total_cpu);
    printf("  Context switches:      %d\n", total_switches);
    if (process_count > 0) {
        printf("  Avg CPU por proceso:   %.1f ms\n", total_cpu / process_count);
        printf("  Avg espera:            %.1f ms\n", total_wait / process_count);
    }
    printf("\n");

    unblock_alarm();
}

// ============================================================
// Main loop del shell
// ============================================================
void shell_run(void) {
    char line[MAX_LINE];

    // Setup alarm mask for sigprocmask
    sigemptyset(&alarm_mask);
    sigaddset(&alarm_mask, SIGALRM);

    printf("+----------------------------------+\n");
    printf("|       miniOS v1.0                |\n");
    printf("|   Simulador de Context Switching |\n");
    printf("|   Escribe 'help' para ayuda      |\n");
    printf("+----------------------------------+\n\n");

    while (1) {
        printf("miniOS> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\n");
            break;
        }

        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        if (strlen(line) == 0) continue;

        char *cmd = strtok(line, " \t");
        char *arg = strtok(NULL, " \t");
        char *arg2 = strtok(NULL, " \t");

        if (!cmd) continue;

        if (strcmp(cmd, "help") == 0) {
            cmd_help();
        } else if (strcmp(cmd, "run") == 0) {
            block_alarm();
            cmd_run(arg, arg2);
            unblock_alarm();
        } else if (strcmp(cmd, "ps") == 0) {
            cmd_ps();
        } else if (strcmp(cmd, "kill") == 0) {
            cmd_kill_proc(arg);
        } else if (strcmp(cmd, "slice") == 0) {
            cmd_slice(arg);
        } else if (strcmp(cmd, "inspect") == 0) {
            cmd_inspect(arg);
        } else if (strcmp(cmd, "runpair") == 0) {
            block_alarm();
            cmd_runpair(arg);
            unblock_alarm();
        } else if (strcmp(cmd, "stats") == 0) {
            cmd_stats();
        } else if (strcmp(cmd, "log") == 0) {
            if (!arg || strlen(arg) == 0) {
                printf("Log esta %s. Uso: log on|off\n",
                       monitor_is_enabled() ? "activado" : "desactivado");
            } else if (strcmp(arg, "on") == 0) {
                monitor_init(MONITOR_SOCKET_PATH);
                monitor_set_enabled(1);
            } else if (strcmp(arg, "off") == 0) {
                monitor_set_enabled(0);
                printf("Log desactivado.\n");
            } else {
                printf("Uso: log on|off\n");
            }
        } else if (strcmp(cmd, "exit") == 0) {
            block_alarm();
            scheduler_stop();
            monitor_close();
            unblock_alarm();
            printf("Hasta luego.\n");
            break;
        } else {
            printf("Comando desconocido: '%s'. Escribe 'help' para ver comandos.\n", cmd);
        }
    }
}