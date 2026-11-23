/* nn::nfp (amiibo): a reader that is there and never sees a tag. The game's amiibo screen waits for one
   (StartDetection, then GetDeviceState stays Searching) until the player backs out (StopDetection).

   The device states, results and the reader's DeviceHandle (8 bytes, the npad id first) are as in Longshot's
   host/nn_nfp.c, which reads tags from the player's own dumps; that library is the way to real tags here too.
   The game checks GetState before it initializes (main+0x9d23a78: 0 None -> Initialize, 1 Initialized). */
#include "hos.h"

#include <pthread.h>
#include <string.h>

#define NFP_RESULT(desc) HOS_RESULT(115, desc)
#define NFP_DEVICE_NOT_FOUND NFP_RESULT(64)
#define NFP_WRONG_STATE NFP_RESULT(73)   /* WrongDeviceState */
enum { ST_INIT = 0, ST_SEARCHING = 1, ST_FOUND = 2, ST_REMOVED = 3, ST_MOUNTED = 4, ST_UNAVAILABLE = 5 };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int ready, dev_state;

HLE(_ZN2nn3nfp10InitializeEv) {
    pthread_mutex_lock(&lock);
    ready = 1, dev_state = ST_INIT;
    pthread_mutex_unlock(&lock);
    hos_log("nfp: the game uses amiibo: the reader never sees one");
}
HLE(_ZN2nn3nfp8FinalizeEv) {
    pthread_mutex_lock(&lock);
    ready = 0, dev_state = ST_INIT;
    pthread_mutex_unlock(&lock);
}
HLE(_ZN2nn3nfp8GetStateEv) { RET(ready); }   /* State_None / State_Initialized */
/* (the reader coming and going: it is always there. Made and signalled once, so a game that waits for it goes on) */
HLE(_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE) {
    if (ARG(0)) hos_kernel_event_signal(hos_system_event_init(c, ARG(0), 1));
    RET(0);
}
/* ListDevices(DeviceHandle* out, int* count, int max): the one reader, the first controller's */
HLE(_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii) {
    if ((int)ARG(2) < 1 || !ARG(0) || !ready) {
        if (ARG(1)) G32(ARG(1)) = 0;
        RET(NFP_DEVICE_NOT_FOUND);
        return;
    }
    memset(GP(ARG(0)), 0, 8);
    if (ARG(1)) G32(ARG(1)) = 1;
    RET(0);
}
HLE(_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE) {
    if (ARG(0)) G32(ARG(0)) = 0;   /* No.1 */
    RET(0);
}
HLE(_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE) {
    pthread_mutex_lock(&lock);
    const int s = ready ? dev_state : ST_UNAVAILABLE;
    pthread_mutex_unlock(&lock);
    RET(s);
}
HLE(_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE) {
    pthread_mutex_lock(&lock);
    const int ok = ready;
    if (ok) dev_state = ST_SEARCHING;
    pthread_mutex_unlock(&lock);
    if (ok) hos_log("nfp: the game waits for an amiibo (none comes)");
    RET(ok ? 0 : NFP_WRONG_STATE);
}
HLE(_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE) {
    pthread_mutex_lock(&lock);
    if (ready) dev_state = ST_INIT;
    pthread_mutex_unlock(&lock);
    RET(0);
}
/* no tag is ever found, so there is none to mount or read */
HLE(_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE) { RET(NFP_WRONG_STATE); }
HLE(_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE) { RET(NFP_WRONG_STATE); }
HLE(_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE) { RET(NFP_WRONG_STATE); }
