#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

char *train_uids[NUM_TRAINS] = {
    "62:89:36:07", "50:43:D2:55", "43:63:9C:C9", "09:2E:FE:06"
};

ServerState *state = NULL;

void get_time_str(time_t t, char *buf, size_t size) {
    struct tm tm_val;
    localtime_r(&t, &tm_val);
    strftime(buf, size, "%H:%M:%S", &tm_val);
}

void init_state() {
    int fd = shm_open("/irpcm_state", O_RDWR | O_CREAT, 0666);
    if (fd == -1) { perror("shm_open"); exit(1); }
    
    struct stat st;
    fstat(fd, &st);
    int is_new = (st.st_size == 0);
    
    if (is_new) ftruncate(fd, sizeof(ServerState));
    
    state = mmap(NULL, sizeof(ServerState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (state == MAP_FAILED) { perror("mmap"); exit(1); }
    
    if (is_new) {
        memset(state, 0, sizeof(ServerState));
        for (int i = 0; i < NUM_TRAINS; i++) {
            state->trains[i].number   = i + 1;
            state->trains[i].priority = 1;
            strcpy(state->trains[i].uid, train_uids[i]);
        }
        for (int i = 0; i < NUM_PLATFORMS; i++) {
            state->platforms[i].number      = i + 1;
            state->platforms[i].owner_type  = OWNER_FREE;
            state->platforms[i].train_index = -1;
        }
    }
}

int find_train_by_uid(char *uid) {
    for (int i = 0; i < NUM_TRAINS; i++)
        if (strcmp(state->trains[i].uid, uid) == 0) return i;
    return -1;
}

int find_free_unknown_platform(void) {
    for (int i = FIRST_UNKNOWN_PLATFORM - 1; i <= LAST_UNKNOWN_PLATFORM - 1; i++)
        if (!state->platforms[i].occupied) return i;
    return -1;
}

int find_unknown_inside(char *uid) {
    for (int i = FIRST_UNKNOWN_PLATFORM - 1; i <= LAST_UNKNOWN_PLATFORM - 1; i++)
        if (state->platforms[i].owner_type == OWNER_UNKNOWN && strcmp(state->platforms[i].unknown_uid, uid) == 0)
            return i;
    return -1;
}

int is_unknown_waiting(char *uid) {
    for (int i = 0; i < state->wait_cnt; i++)
        if (strcmp(state->wait_uid[i], uid) == 0) return 1;
    return 0;
}

time_t update_seen(char *uid, time_t now) {
    int old = 0; time_t prev;
    for (int i = 0; i < MAX_SEEN_UIDS; i++) {
        if (state->seen_uid[i][0] != '\0' && strcmp(state->seen_uid[i], uid) == 0) {
            prev = state->seen_time[i];
            state->seen_time[i] = now;
            return prev;
        }
    }
    for (int i = 0; i < MAX_SEEN_UIDS; i++) {
        if (state->seen_uid[i][0] == '\0') { old = i; break; }
        if (state->seen_time[i] < state->seen_time[old]) old = i;
    }
    strncpy(state->seen_uid[old], uid, 31);
    state->seen_uid[old][31] = '\0';
    state->seen_time[old] = now;
    return 0;
}

int assign_platform(int p, int owner, int t, char *uid) {
    time_t now = time(NULL);
    char now_s[16], leave[16];

    state->platforms[p].occupied       = 1;
    state->platforms[p].owner_type     = owner;
    state->platforms[p].arrival_time   = now;
    state->platforms[p].departure_time = now + DWELL_TIME_SECONDS;

    if (owner == OWNER_KNOWN) {
        state->platforms[p].train_index  = t;
        state->platforms[p].train_number = state->trains[t].number;
        state->platforms[p].unknown_uid[0] = '\0';

        state->trains[t].platform       = state->platforms[p].number;
        state->trains[t].active         = 1;
        state->trains[t].arrived        = 1;
        state->trains[t].arrival_time   = now;
        state->trains[t].departure_time = state->platforms[p].departure_time;
    } else {
        state->platforms[p].train_index  = -1;
        state->platforms[p].train_number = 0;
        strncpy(state->platforms[p].unknown_uid, uid, 31);
        state->platforms[p].unknown_uid[31] = '\0';
    }

    get_time_str(now, now_s, sizeof(now_s));
    get_time_str(state->platforms[p].departure_time, leave, sizeof(leave));

    printf("[SERVER] ALLOCATED PLATFORM : %d | TIME: %s | LEAVE BY: %s\n", state->platforms[p].number, now_s, leave);
    return state->platforms[p].number;
}

int process_known(int t, Message *msg, Reply *reply) {
    time_t now = msg->event_time;
    int p = state->trains[t].number - 1;

    if (state->trains[t].active) {
        if (now - state->trains[t].last_msg_time >= DUPLICATE_MSG_GAP) {
            printf("[SERVER] TRAIN %d ALREADY INSIDE (Platform %d)\n", state->trains[t].number, state->trains[t].platform);
            state->trains[t].last_msg_time = now;
        }
        state->trains[t].last_scan_time = now;
        reply->status   = RPL_DUPLICATE;
        reply->platform = state->trains[t].platform;
        return 0;
    }

    if (state->trains[t].last_scan_time != 0 && now - state->trains[t].last_scan_time < REARRIVAL_GAP) {
        if (now - state->trains[t].last_msg_time >= DUPLICATE_MSG_GAP) {
            printf("[SERVER] Train %d card not removed.\n", state->trains[t].number);
            state->trains[t].last_msg_time = now;
        }
        state->trains[t].last_scan_time = now;
        reply->status = RPL_IGNORED;
        return 0;
    }

    state->trains[t].last_scan_time = now;
    state->trains[t].last_msg_time  = now;

    if (state->platforms[p].occupied) {
        printf("[SERVER] PLATFORM %d IS OCCUPIED\n", state->platforms[p].number);
        reply->status = RPL_NOTHING;
        return 0;
    }

    reply->platform = assign_platform(p, OWNER_KNOWN, t, NULL);
    reply->status   = RPL_ALLOCATED;
    return reply->platform;
}

int process_unknown(Message *msg, Reply *reply) {
    time_t now = msg->event_time;
    time_t prev = update_seen(msg->uid, now);
    int p = find_unknown_inside(msg->uid);

    if (p >= 0 || is_unknown_waiting(msg->uid)) {
        reply->status = RPL_DUPLICATE;
        return 0;
    }

    if (prev != 0 && now - prev < REARRIVAL_GAP) {
        reply->status = RPL_IGNORED;
        return 0;
    }

    p = find_free_unknown_platform();

    if (p < 0) {
        if (state->wait_cnt < MAX_UNKNOWN_WAITING) {
            strncpy(state->wait_uid[state->wait_cnt], msg->uid, 31);
            state->wait_uid[state->wait_cnt][31] = '\0';
            state->wait_cnt++;
        }
        reply->status = RPL_WAITING;
        reply->waiting_trains = state->wait_cnt;
        printf("[SERVER] UNKNOWN TRAIN WAITING: %s\n", msg->uid);
        return 0;
    }

    reply->platform = assign_platform(p, OWNER_UNKNOWN, -1, msg->uid);
    reply->status   = RPL_ALLOCATED;
    return reply->platform;
}

int process_scan(Message *msg, Reply *reply) {
    int t = find_train_by_uid(msg->uid);
    if (t >= 0) return process_known(t, msg, reply);
    return process_unknown(msg, reply);
}

int release_platform(Message *msg, Reply *reply) {
    time_t now = time(NULL);
    int p = msg->platform - 1;

    if (p < 0 || p >= NUM_PLATFORMS || !state->platforms[p].occupied || now < state->platforms[p].departure_time) {
        reply->status = RPL_NOTHING;
        return 0;
    }

    if (state->platforms[p].owner_type == OWNER_KNOWN) {
        int t = state->platforms[p].train_index;
        if (t >= 0 && t < NUM_TRAINS) {
            state->trains[t].active = 0;
            state->trains[t].arrived = 0;
            state->trains[t].platform = 0;
            state->trains[t].departure_time = now;
        }
    }

    printf("[SERVER] PLATFORM %d FREED\n", state->platforms[p].number);
    state->platforms[p].occupied = 0;
    state->platforms[p].owner_type = OWNER_FREE;
    state->platforms[p].train_index = -1;
    
    reply->status = RPL_RELEASED;
    reply->platform = msg->platform;
    reply->waiting_trains = state->wait_cnt;
    return msg->platform;
}

int assign_waiting(Reply *reply) {
    int p = find_free_unknown_platform();
    if (state->wait_cnt == 0 || p < 0) {
        reply->status = RPL_NOTHING;
        return 0;
    }

    char uid[32];
    strcpy(uid, state->wait_uid[0]);
    for (int i = 1; i < state->wait_cnt; i++) strcpy(state->wait_uid[i - 1], state->wait_uid[i]);
    state->wait_cnt--;

    reply->platform = assign_platform(p, OWNER_UNKNOWN, -1, uid);
    reply->status   = RPL_ALLOCATED;
    return reply->platform;
}

void handle_status(Reply *reply) {
    reply->status = RPL_OK;
    reply->waiting_trains = state->wait_cnt;
}

int main(void) {
    init_state();
    
    name_attach_t *attach = name_attach(NULL, SERVER_NAME, 0);
    if (attach == NULL) { perror("name_attach"); exit(1); }

    printf("[SERVER] Process Ready. Backed by shared memory.\n");
    
    RecvBuf buf;
    Reply reply;

    while (1) {
        int rcvid = MsgReceive(attach->chid, &buf, sizeof(buf), NULL);
        if (rcvid == -1) { if (errno == EINTR) continue; break; }

        if (rcvid == 0) {
            switch (buf.pulse.code) {
                case _PULSE_CODE_DISCONNECT: ConnectDetach(buf.pulse.scoid); break;
            }
            continue;
        }

        if (buf.type == _IO_CONNECT) { MsgReply(rcvid, EOK, NULL, 0); continue; }
        if (buf.type > _IO_BASE && buf.type <= _IO_MAX) { MsgError(rcvid, ENOSYS); continue; }

        memset(&reply, 0, sizeof(reply));
        
        switch (buf.msg.type) {
            case MSG_RFID_DETECTED: process_scan(&buf.msg, &reply); break;
            case MSG_PLATFORM_REQUEST: assign_waiting(&reply); break;
            case MSG_PLATFORM_RELEASE: release_platform(&buf.msg, &reply); break;
            case MSG_SYSTEM_STATUS: handle_status(&reply); break;
            case MSG_SHUTDOWN: exit(0); break;
            default: reply.status = RPL_NOTHING; break;
        }
        MsgReply(rcvid, EOK, &reply, sizeof(reply));
    }
    return 0;
}
