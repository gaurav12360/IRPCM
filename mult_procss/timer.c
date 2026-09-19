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
    if (fd == -1) {
        perror("shm_open (timer waiting for server)");
        return;
    }
    state = mmap(NULL, sizeof(ServerState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (state == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
}

int main(void) {
    printf("[TIMER] Starting up...\n");

    while (state == NULL) {
        init_state();
        if (state == NULL) sleep(1);
    }

    int coid = -1;
    while (coid == -1) {
        coid = name_open(SERVER_NAME, 0);
        if (coid == -1) {
            printf("[TIMER] Waiting for server '%s'...\n", SERVER_NAME);
            sleep(1);
        }
    }
    
    printf("[TIMER] Connected to server.\n");

    Message msg;
    Reply reply;

    int tick = 0;
    while (1) {
        sleep(1);
        time_t now = time(NULL);
        
        // 1. Check for platform expirations
        for (int i = 0; i < NUM_PLATFORMS; i++) {
            if (state->platforms[i].occupied && now >= state->platforms[i].departure_time) {
                memset(&msg, 0, sizeof(msg));
                msg.type = MSG_PLATFORM_RELEASE;
                msg.platform = state->platforms[i].number;
                
                if (MsgSend(coid, &msg, sizeof(msg), &reply, sizeof(reply)) == -1) {
                    perror("MsgSend (release)");
                }
            }
        }
        
        // 2. Check if we need to request platform for waiting trains
        if (state->wait_cnt > 0) {
            // See if there's a free unknown platform
            int free_found = 0;
            for (int i = FIRST_UNKNOWN_PLATFORM - 1; i <= LAST_UNKNOWN_PLATFORM - 1; i++) {
                if (!state->platforms[i].occupied) { free_found = 1; break; }
            }
            if (free_found) {
                memset(&msg, 0, sizeof(msg));
                msg.type = MSG_PLATFORM_REQUEST;
                if (MsgSend(coid, &msg, sizeof(msg), &reply, sizeof(reply)) == -1) {
                    perror("MsgSend (request)");
                }
            }
        }
        
        // 3. Periodic status print or pulse (optional, since display handles UI)
        tick++;
        if (tick % 5 == 0) {
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_SYSTEM_STATUS;
            if (MsgSend(coid, &msg, sizeof(msg), &reply, sizeof(reply)) == -1) {
                perror("MsgSend (status)");
            }
        }
    }

    name_close(coid);
    return 0;
}
