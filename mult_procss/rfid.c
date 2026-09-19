#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

typedef struct { int at_second; const char *uid; } SimEvent;

// Richer test cases: Mix of known trains, unknown trains filling up platforms,
// causing a queue, and duplicate re-scans.
static SimEvent SIM_EVENTS[] = {
    {  2,  "62:89:36:07" },   /* Known Train 1 arrives (Track 1) */
    {  4,  "50:43:D2:55" },   /* Known Train 2 arrives (Track 2) */
    {  6,  "43:63:9C:C9" },   /* Known Train 3 arrives (Track 3) */
    {  8,  "09:2E:FE:06" },   /* Known Train 4 arrives (Track 4) */
    
    { 12,  "UNK_FREIGHT_A" }, /* Unknown train -> Track 5 */
    { 14,  "UNK_FREIGHT_B" }, /* Unknown train -> Track 6 */
    { 16,  "UNK_FREIGHT_C" }, /* Unknown train -> Track 7 */
    { 18,  "UNK_FREIGHT_D" }, /* Unknown train -> Track 8 */
    
    { 22,  "UNK_FREIGHT_E" }, /* ALL TRACKS FULL -> Goes to WAITING QUEUE */
    { 24,  "UNK_FREIGHT_F" }, /* ALL TRACKS FULL -> Goes to WAITING QUEUE */
    { 26,  "UNK_FREIGHT_G" }, /* ALL TRACKS FULL -> Goes to WAITING QUEUE */
    
    { 30,  "62:89:36:07" },   /* Duplicate scan of Train 1 (should be ignored) */
    { 35,  "UNK_FREIGHT_A" }, /* Duplicate scan of Unknown A (should be ignored) */
    
    { 70,  "LATE_TRAIN_01" }, /* Arrives after some dwell times have expired */
    { 72,  "LATE_TRAIN_02" }
};
#define NUM_SIM_EVENTS  (int)(sizeof(SIM_EVENTS)/sizeof(SIM_EVENTS[0]))

int main(void) {
    int coid = -1;
    while (coid == -1) {
        coid = name_open(SERVER_NAME, 0);
        if (coid == -1) sleep(1);
    }
    
    time_t start = time(NULL);
    int next_ev = 0;
    Message msg;
    Reply reply;

    while (1) {
        usleep(100000); // 100ms
        time_t now = time(NULL);
        int elapsed = (int)(now - start);
        
        if (next_ev < NUM_SIM_EVENTS && elapsed >= SIM_EVENTS[next_ev].at_second) {
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_RFID_DETECTED;
            msg.event_time = now;
            strncpy(msg.uid, SIM_EVENTS[next_ev].uid, 31);
            msg.uid[31] = '\0';
            
            MsgSend(coid, &msg, sizeof(msg), &reply, sizeof(reply));
            next_ev++;
        }
    }

    name_close(coid);
    return 0;
}
