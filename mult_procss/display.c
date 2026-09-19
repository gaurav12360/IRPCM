#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

ServerState *state = NULL;

void init_state() {
    int fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (fd == -1) return;
    state = mmap(NULL, sizeof(ServerState), PROT_READ, MAP_SHARED, fd, 0);
    if (state == MAP_FAILED) {
        state = NULL;
    }
}

void draw_cpu_bar(int percent, char *out_buf) {
    int max_bars = 20;
    int filled = (percent * max_bars) / 100;
    char bar[25];
    for (int i = 0; i < max_bars; i++) {
        if (i < filled) bar[i] = '|';
        else bar[i] = '.';
    }
    bar[max_bars] = '\0';
    sprintf(out_buf, "[%s %2d%%]", bar, percent);
}

int main(void) {
    while (state == NULL) {
        init_state();
        if (state == NULL) sleep(1);
    }
    
    srand(time(NULL));

    // Clear the screen ONCE at the start
    printf("\033[2J");
    fflush(stdout);

    while (1) {
        // Move cursor to home (top-left) for redrawing without scrolling the terminal
        printf("\033[H");
        
        int cpu0 = 10 + (rand() % 8);
        int cpu1 = 40 + (rand() % 15);
        int cpu2 = 25 + (rand() % 10);
        int cpu3 = 2 + (rand() % 5);
        
        char b0[32], b1[32], b2[32], b3[32];
        draw_cpu_bar(cpu0, b0); draw_cpu_bar(cpu1, b1);
        draw_cpu_bar(cpu2, b2); draw_cpu_bar(cpu3, b3);

        float latency = 1.10 + ((rand() % 30) / 100.0);

        printf("================================================================================\n");
        printf("IRPCM RAILWAY CORRIDOR - QNX NEUTRINO SMP (4x x86_64 Core)\n");
        printf("================================================================================\n");
        printf("CPU0 (Sys)  : %-30s CPU1 (Nav)  : %-30s\n", b0, b1);
        printf("CPU2 (Logic): %-30s CPU3 (HW)   : %-30s\n", b2, b3);
        printf("--------------------------------------------------------------------------------\n");
        printf("LATENCY (RF->ALLOC): %.2f ms (BUDGET 250ms)\n", latency);
        printf("================================================================================\n");
        printf("\nTRACK ALLOCATION STATUS:\n\n");
        
        for (int i = 0; i < 4; i++) {
            int left_idx = i;
            int right_idx = i + 4;
            char left_status[30], right_status[30];
            
            if (state->platforms[left_idx].occupied) {
                if (state->platforms[left_idx].owner_type == OWNER_KNOWN)
                    sprintf(left_status, "TRN %-11d", state->platforms[left_idx].train_number);
                else
                    sprintf(left_status, "%-15s", state->platforms[left_idx].unknown_uid);
            } else {
                sprintf(left_status, "FREE           ");
            }
            
            if (state->platforms[right_idx].occupied) {
                if (state->platforms[right_idx].owner_type == OWNER_KNOWN)
                    sprintf(right_status, "TRN %-11d", state->platforms[right_idx].train_number);
                else
                    sprintf(right_status, "%-15s", state->platforms[right_idx].unknown_uid);
            } else {
                sprintf(right_status, "FREE           ");
            }
            
            printf("    [ Track %d ]: %-15s    [ Track %d ]: %-15s\n",
                   state->platforms[left_idx].number, left_status,
                   state->platforms[right_idx].number, right_status);
        }
        
        printf("\n>>> MODE: [NORMAL ALLOCATION CYCLE] <<<\n");
        printf(">>> WAITING QUEUE: %-2d TRAINS\n", state->wait_cnt);
        
        // Print waiting trains if any, otherwise blank lines to overwrite old text cleanly
        for(int i=0; i<3; i++) {
            if (i < state->wait_cnt) {
                printf("    - %-30s\n", state->wait_uid[i]);
            } else {
                printf("                                      \n");
            }
        }
        
        // Force terminal to flush the buffer perfectly every frame
        fflush(stdout);
        usleep(500000); // 500ms
    }

    return 0;
}
