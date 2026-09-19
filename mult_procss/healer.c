#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>

#define NUM_MANAGED 4

typedef struct {
    const char *name;
    const char *path;
    pid_t       pid;
    int         restart_count;
    time_t      last_restart;
} ManagedProcess;

static ManagedProcess g_procs[NUM_MANAGED] = {
    { "server",  "./server",  -1, 0, 0 },
    { "timer",   "./timer",   -1, 0, 0 },
    { "display", "./display", -1, 0, 0 },
    { "rfid",    "./rfid",    -1, 0, 0 }
};

volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void spawn(int i) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("[HEALER] fork");
        return;
    }
    if (pid == 0) {
        /* Child: redirect output so only display process draws to terminal */
        if (i != 2) { // 2 is display
            int fd = open("/dev/null", O_WRONLY);
            if (fd != -1) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
        }
        /* Child: exec the process */
        execl(g_procs[i].path, g_procs[i].name, NULL);
        perror("[HEALER] execl");
        _exit(1);
    }
    g_procs[i].pid           = pid;
    g_procs[i].last_restart  = time(NULL);
    g_procs[i].restart_count++;
    printf("[HEALER] %-10s PID=%-6d (restart #%d)\n",
           g_procs[i].name, pid, g_procs[i].restart_count);
}

int main(void) {
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  QNX Healer — Multi-Process Supervisor          ║\n");
    printf("║  Spawning: server → timer → display → rfid      ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Staggered startup to respect dependency order */
    spawn(0); sleep(1);
    spawn(1); usleep(200000);
    spawn(2); usleep(200000);
    spawn(3);

    printf("[HEALER] All processes launched. Monitoring...\n");

    while (g_running) {
        int   status;
        pid_t dead = waitpid(-1, &status, WNOHANG);

        if (dead > 0) {
            for (int i = 0; i < NUM_MANAGED; i++) {
                if (g_procs[i].pid != dead) continue;

                if (WIFEXITED(status))
                    printf("[HEALER] %-10s (PID %d) exited with code %d\n",
                           g_procs[i].name, dead, WEXITSTATUS(status));
                else if (WIFSIGNALED(status))
                    printf("[HEALER] %-10s (PID %d) killed by signal %d — HEALING\n",
                           g_procs[i].name, dead, WTERMSIG(status));

                g_procs[i].pid = -1;

                /* Back-off: if it keeps crashing, wait longer before restart */
                time_t since_last = time(NULL) - g_procs[i].last_restart;
                int backoff = (since_last < 5) ? 3 : 1;
                printf("[HEALER] Restarting %-10s in %d s...\n",
                       g_procs[i].name, backoff);
                sleep(backoff);

                if (i == 0) { // server died
                    spawn(0); sleep(1);
                    /* Restart dependents so they can reconnect */
                    for (int j = 1; j < NUM_MANAGED; j++) {
                        pid_t old_pid = g_procs[j].pid;
                        if (old_pid > 0) kill(old_pid, SIGTERM);
                        g_procs[j].pid = -1;
                        if (old_pid > 0) waitpid(old_pid, NULL, 0);
                    }
                    usleep(200000); spawn(1);
                    usleep(200000); spawn(2);
                    usleep(200000); spawn(3);
                } else {
                    spawn(i);
                }
                break;
            }
        } else if (dead == -1 && errno == ECHILD) {
            printf("[HEALER] No children left. Exiting.\n");
            break;
        }

        usleep(300000);
    }

    printf("\n[HEALER] Shutdown initiated — terminating children...\n");
    for (int i = NUM_MANAGED - 1; i >= 0; i--) {
        if (g_procs[i].pid > 0) {
            kill(g_procs[i].pid, SIGTERM);
            printf("[HEALER] Sent SIGTERM to %-10s (PID %d)\n",
                   g_procs[i].name, g_procs[i].pid);
        }
    }
    for (int i = 0; i < NUM_MANAGED; i++) {
        if (g_procs[i].pid > 0) waitpid(g_procs[i].pid, NULL, 0);
    }
    printf("[HEALER] All processes terminated. Goodbye.\n");
    return 0;
}
