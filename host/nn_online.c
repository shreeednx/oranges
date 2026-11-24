/* The online and wireless services, answered as a console that is offline: no internet connection, no local
   wireless, no add-on content, an empty delivery cache, an empty friend list, no parental controls.

   Each answer is the console's own for that state where switchbrew's "Error codes" lists one, and every output the
   game passes is written. How the game takes each answer was checked at its call sites (main+...):
     - nifm: IsNetworkAvailable false after SubmitNetworkRequestAndWait, and HandleNetworkRequestErrorResult false
       (not resolved), the bool the game keeps (0x9c7f7dc, 0xbafa798).
     - bcat: MountDeliveryCacheStorage succeeds (a failed one stopped the frame loop) and a directory that does not
       open ends the game's read (0xbb0be3c); a failed RequestSyncDeliveryCache skips its progress object
       (0xbb0b648). The delivery cache's classes are HLE too: lifted, they would run over the service object the
       SDK's Mount makes.
     - friends: a failed EnsureFriendListAvailable skips its AsyncContext (0xbaf82d4).
     - ldn: a failed Initialize goes to the game's error display (0xb78283c).
   Results: nifm 2110-1100 "network not available", bcat ResultNotFound / NotOpened / InternetRequestNotAccepted,
   friends ResultInternetRequestNotAccepted, ldn 2203-0037 "state is not Initialized" (switchbrew); ldn's Initialize
   answers 2203-0022, yuzu's ResultDisabled (local communication off), which switchbrew does not list. */
#include "hos.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void once(const char *what) {   /* (a note of each service's first use) */
    static const char *seen[64];
    static int n;
    for (int i = 0; i < n; i++)
        if (seen[i] == what) return;
    if (n < 64) seen[n++] = what;
    hos_log("%s", what);
}

/* ---- nn::nifm: not connected ---------------------------------------------------------------------------------- */
#define NIFM_NOT_AVAILABLE HOS_RESULT(110, 1100)   /* 2110-1100: network not available */

HLE(_ZN2nn4nifm10InitializeEv) { RET(0); }
HLE(_ZN2nn4nifm20SubmitNetworkRequestEv) { once("nifm: the game asks for the network: there is none (offline)"); }
HLE(_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv) {
    once("nifm: the game asks for the network: there is none (offline)");
}
HLE(_ZN2nn4nifm20CancelNetworkRequestEv) { (void)c; }
HLE(_ZN2nn4nifm22IsNetworkRequestOnHoldEv) { RET(0); }
HLE(_ZN2nn4nifm18IsNetworkAvailableEv) { RET(0); }
HLE(_ZN2nn4nifm31HandleNetworkRequestErrorResultEv) { RET(0); }   /* (the error applet: not resolved) */
HLE(_ZN2nn4nifm19SetLocalNetworkModeEb) { (void)c; }
/* GetCurrentPrimaryIpAddress(in_addr*) */
HLE(_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr) {
    if (ARG(0)) G32(ARG(0)) = 0;
    RET(NIFM_NOT_AVAILABLE);
}
/* GetCurrentIpConfigInfo(in_addr* address, in_addr* mask, in_addr* gateway, in_addr* dns1, in_addr* dns2) */
HLE(_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_) {
    for (int i = 0; i < 5; i++)
        if (ARG(i)) G32(ARG(i)) = 0;
    RET(NIFM_NOT_AVAILABLE);
}
HLE(_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE) { RET(NIFM_NOT_AVAILABLE); }

/* ---- nn::bcat: an empty delivery cache. The storage mounts (it is the application's, there even offline: a
   failed mount leaves the game waiting, its frames stopped), and holds no directory and no file. A sync needs the
   internet. The classes' objects are the game's (8 bytes for a directory, main+0xbb0be2c) and are not touched. -- */
#define BCAT_NOT_FOUND HOS_RESULT(122, 2)                  /* ResultNotFound */
#define BCAT_NOT_OPENED HOS_RESULT(122, 7)                 /* ResultNotOpened */
#define BCAT_INTERNET_NOT_ACCEPTED HOS_RESULT(122, 8)      /* ResultInternetRequestNotAccepted */

HLE(_ZN2nn4bcat10InitializeEv) { (void)c; }
HLE(_ZN2nn4bcat25MountDeliveryCacheStorageEv) {
    once("bcat: the delivery cache is empty (offline)");
    RET(0);
}
HLE(_ZN2nn4bcat27UnmountDeliveryCacheStorageEv) { (void)c; }
HLE(_ZN2nn4bcat22DeliveryCacheDirectoryC1Ev) { (void)c; }
HLE(_ZN2nn4bcat22DeliveryCacheDirectoryD1Ev) { (void)c; }
HLE(_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE) { RET(BCAT_NOT_FOUND); }
/* Read(int* count, DeliveryCacheDirectoryEntry*, int max) */
HLE(_ZN2nn4bcat22DeliveryCacheDirectory4ReadEPiPNS0_27DeliveryCacheDirectoryEntryEi) {
    if (ARG(1)) G32(ARG(1)) = 0;
    RET(BCAT_NOT_OPENED);
}
HLE(_ZN2nn4bcat17DeliveryCacheFileC1Ev) { (void)c; }
HLE(_ZN2nn4bcat17DeliveryCacheFileD1Ev) { (void)c; }
HLE(_ZN2nn4bcat17DeliveryCacheFile4OpenERKNS0_13DirectoryNameERKNS0_8FileNameE) { RET(BCAT_NOT_FOUND); }
/* Read(size_t* read, int64_t offset, void* buffer, size_t size) */
HLE(_ZN2nn4bcat17DeliveryCacheFile4ReadEPmlPvm) {
    if (ARG(1)) G64(ARG(1)) = 0;
    RET(BCAT_NOT_OPENED);
}
HLE(_ZN2nn4bcat17DeliveryCacheFile9GetDigestEv) { c->x[0] = c->x[1] = 0; }   /* Digest (16 bytes, in x0/x1) */
HLE(_ZN2nn4bcat21DeliveryCacheProgressC1Ev) { (void)c; }
HLE(_ZN2nn4bcat21DeliveryCacheProgressD1Ev) { (void)c; }
HLE(_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE) { RET(BCAT_INTERNET_NOT_ACCEPTED); }
HLE(_ZN2nn4bcat21DeliveryCacheProgress6UpdateEv) { (void)c; }
HLE(_ZNK2nn4bcat21DeliveryCacheProgress9GetStatusEv) { RET(9); }   /* Done (the status the game waits for) */
HLE(_ZNK2nn4bcat21DeliveryCacheProgress9GetResultEv) { RET(BCAT_INTERNET_NOT_ACCEPTED); }
HLE(_ZNK2nn4bcat21DeliveryCacheProgress18GetWholeDownloadedEv) { RET(0); }

/* ---- nn::friends: no friends, nothing online ------------------------------------------------------------------ */
#define FRIENDS_INTERNET_NOT_ACCEPTED HOS_RESULT(121, 6)   /* ResultInternetRequestNotAccepted */

HLE(_ZN2nn7friends10InitializeEv) { (void)c; }
HLE(_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE) { (void)c; }
/* GetBlockedUserList(int* count, NetworkServiceAccountId*, Uid const&, int offset, int max) */
HLE(_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii) {
    if (ARG(0)) G32(ARG(0)) = 0;
    RET(0);
}
/* GetFriendList(int* count, Friend*, Uid const&, int offset, int max, FriendFilter const&) */
HLE(_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE) {
    if (ARG(0)) G32(ARG(0)) = 0;
    RET(0);
}
HLE(_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi) { RET(0); }
HLE(_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE) {
    RET(FRIENDS_INTERNET_NOT_ACCEPTED);
}
HLE(_ZN2nn7friends28DeclareOpenOnlinePlaySessionERKNS_7account3UidE) { RET(0); }
HLE(_ZN2nn7friends29DeclareCloseOnlinePlaySessionERKNS_7account3UidE) { RET(0); }
/* GetPlayHistoryRegistrationKey(PlayHistoryRegistrationKey* (0x40 bytes), Uid const&, bool localPlay): a blank key */
HLE(_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb) {
    if (ARG(0)) memset(GP(ARG(0)), 0, 0x40);
    RET(0);
}
HLE(_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_) {
    RET(0);
}
HLE(_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_) {
    hos_log("friends: the game shows a user's profile (not shown: offline)");
    RET(0);
}
HLE(_ZN2nn7friends25StartSendingFriendRequestERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_) {
    hos_log("friends: the game sends a friend request (not sent: offline)");
    RET(0);
}

/* ---- nn::pctl: no parental controls --------------------------------------------------------------------------- */
HLE(_ZN2nn4pctl32CheckFreeCommunicationPermissionEb) { RET(0); }
HLE(_ZN2nn4pctl28IsFreeCommunicationAvailableEv) { RET(1); }

/* ---- nn::prepo: play reports are dropped ---------------------------------------------------------------------- */
HLE(_ZN2nn5prepo28RequestImmediateTransmissionEv) { RET(0); }
HLE(_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_) { RET(0); }

/* ---- nn::aoc: no add-on content ------------------------------------------------------------------------------- */
HLE(_ZN2nn3aoc17CountAddOnContentEv) { RET(0); }
HLE(_ZN2nn3aoc16ListAddOnContentEPiii) { RET(0); }   /* (int* out, int offset, int max) -> the number listed */

/* ---- nn::album: screenshots are not kept ---------------------------------------------------------------------- */
HLE(_ZN2nn5album10InitializeEv) { (void)c; }
HLE(_ZN2nn5album8FinalizeEv) { (void)c; }
HLE(_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionE) {
    hos_log("album: the game saves a screenshot (%llu bytes, not kept)", (unsigned long long)ARG(1));
    RET(0);
}
HLE(_ZN2nn5album22SaveAndShareScreenshotEPNS0_7ShareIdEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionERKNS_7account3UidEPKc) {
    hos_log("album: the game shares a screenshot (not kept, not posted)");
    RET(0);
}
/* GetSharePostedLastUrl(char* out, size_t size, ShareId const&): none */
HLE(_ZN2nn5album21GetSharePostedLastUrlEPcmRKNS0_7ShareIdE) {
    if (ARG(0) && ARG(1)) *(char *)GP(ARG(0)) = 0;
    RET(0);
}

/* ---- nn::ec, nn::err: applets the game would show ------------------------------------------------------------- */
HLE(_ZN2nn2ec30ShowShopApplicationInformationEv) { hos_log("ec: the game opens its eShop page (not shown)"); }
HLE(_ZN2nn3err9ShowErrorENS_6ResultE) {
    const uint32_t r = (uint32_t)ARG(0);
    hos_log("err: the game shows error %04u-%04u (result %#x)", 2000 + (r & 0x1FF), (r >> 9) & 0x1FFF, r);
}
HLE(_ZN2nn3err9ShowErrorENS0_9ErrorCodeE) {   /* ErrorCode {u32 category, u32 number}, by value */
    hos_log("err: the game shows error %04u-%04u", (uint32_t)ARG(0), (uint32_t)(ARG(0) >> 32));
}
HLE(_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE) {
    const uint32_t *v = ARG(0) ? GP(ARG(0)) : NULL;
    if (v) hos_log("err: the game shows an error (variant %08x %08x %08x)", v[0], v[1], v[2]);
}

/* ---- nn::ldn: local wireless is off --------------------------------------------------------------------------- */
#define LDN_DISABLED HOS_RESULT(203, 22)          /* (yuzu's ResultDisabled) */
#define LDN_NOT_INITIALIZED HOS_RESULT(203, 37)   /* 2203-0037: state is not Initialized */

HLE(_ZN2nn3ldn10InitializeEv) {
    once("ldn: the game opens local wireless: it is off");
    RET(LDN_DISABLED);
}
HLE(_ZN2nn3ldn8FinalizeEv) { (void)c; }
HLE(_ZN2nn3ldn8GetStateEv) { RET(0); }   /* State_None */
HLE(_ZN2nn3ldn19GetDisconnectReasonEv) { RET(0); }   /* DisconnectReason_None */
HLE(_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE) {
    if (ARG(0)) hos_system_event_init(c, ARG(0), 1);
}
/* MakeBroadcastAddress(Ipv4Address, SubnetMask) -> Ipv4Address */
HLE(_ZN2nn3ldn20MakeBroadcastAddressENS0_11Ipv4AddressENS0_10SubnetMaskE) {
    RET((uint32_t)ARG(0) | ~(uint32_t)ARG(1));
}
/* Scan / ScanPrivate(NetworkInfo*, int* count, int max, ScanFilter const&, int channel) */
HLE(_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi) {
    if (ARG(1)) G32(ARG(1)) = 0;
    RET(LDN_NOT_INITIALIZED);
}
HLE(_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi) {
    if (ARG(1)) G32(ARG(1)) = 0;
    RET(LDN_NOT_INITIALIZED);
}
/* GetIpv4Address(Ipv4Address*, SubnetMask*) */
HLE(_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE) {
    if (ARG(0)) G32(ARG(0)) = 0;
    if (ARG(1)) G32(ARG(1)) = 0;
    RET(LDN_NOT_INITIALIZED);
}
HLE(_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE) { RET(LDN_NOT_INITIALIZED); }
HLE(_ZN2nn3ldn15OpenAccessPointEv) { RET(LDN_NOT_INITIALIZED); }
HLE(_ZN2nn3ldn16CloseAccessPointEv) { RET(LDN_NOT_INITIALIZED); }
HLE(_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE) { RET(LDN_NOT_INITIALIZED); }
HLE(_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE) { RET(LDN_NOT_INITIALIZED); }
HLE(_ZN2nn3ldn14DestroyNetworkEv) { RET(LDN_NOT_INITIALIZED); }
HLE(_ZN2nn3ldn16SetAdvertiseDataEPKvm) { RET(LDN_NOT_INITIALIZED); }
HLE(_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE) { RET(LDN_NOT_INITIALIZED); }
HLE(_ZN2nn3ldn6RejectENS0_11Ipv4AddressE) { RET(LDN_NOT_INITIALIZED); }
HLE(_ZN2nn3ldn11OpenStationEv) { RET(LDN_NOT_INITIALIZED); }
HLE(_ZN2nn3ldn12CloseStationEv) { RET(LDN_NOT_INITIALIZED); }
HLE(_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE) { RET(LDN_NOT_INITIALIZED); }
HLE(_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE) { RET(LDN_NOT_INITIALIZED); }
HLE(_ZN2nn3ldn10DisconnectEv) { RET(LDN_NOT_INITIALIZED); }
HLE(_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE) { RET(LDN_NOT_INITIALIZED); }
HLE(_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE) { RET(LDN_NOT_INITIALIZED); }

/* ---- nn::socket: no network. A socket is never made (ENETDOWN), so every call on a descriptor is EBADF; name
   lookups fail as a resolver without a connection does (EAI_AGAIN). The address helpers are plain functions.
   Errno and AiErrno numbers are FreeBSD's, as the console's socket layer uses. -------------------------------- */
enum { S_EBADF = 9, S_EAFNOSUPPORT = 47, S_ENETDOWN = 50, S_EAI_AGAIN = 2, S_AF_INET = 2 };
static __thread int sock_errno;

static void sock_fail(Ctx *c, int err) {
    sock_errno = err;
    RET((uint64_t)(int64_t)-1);
}

HLE(_ZN2nn6socket10InitializeEPvmmi) { RET(0); }   /* Initialize(void* pool, size_t, size_t allocPoolSize, int) */
HLE(_ZN2nn6socket8FinalizeEv) { RET(0); }
HLE(_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE) {
    once("socket: the game opens a socket: the network is down (offline)");
    sock_fail(c, S_ENETDOWN);
}
HLE(_ZN2nn6socket6SocketEiii) {
    once("socket: the game opens a socket: the network is down (offline)");
    sock_fail(c, S_ENETDOWN);
}
HLE(_ZN2nn6socket4RecvEiPvmi) { sock_fail(c, S_EBADF); }
HLE(_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj) { sock_fail(c, S_EBADF); }
HLE(_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj) { sock_fail(c, S_EBADF); }
HLE(_ZN2nn6socket4SendEiPKvmi) { sock_fail(c, S_EBADF); }
HLE(_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj) { sock_fail(c, S_EBADF); }
HLE(_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj) { sock_fail(c, S_EBADF); }
HLE(_ZN2nn6socket6AcceptEiP8sockaddrPj) { sock_fail(c, S_EBADF); }
HLE(_ZN2nn6socket4BindEiPKNS0_8SockAddrEj) { sock_fail(c, S_EBADF); }
HLE(_ZN2nn6socket4BindEiPK8sockaddrj) { sock_fail(c, S_EBADF); }
HLE(_ZN2nn6socket7ConnectEiPK8sockaddrj) { sock_fail(c, S_EBADF); }
HLE(_ZN2nn6socket11GetSockNameEiP8sockaddrPj) { sock_fail(c, S_EBADF); }
HLE(_ZN2nn6socket10GetSockOptEiiiPvPj) { sock_fail(c, S_EBADF); }
HLE(_ZN2nn6socket6ListenEii) { sock_fail(c, S_EBADF); }
HLE(_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj) { sock_fail(c, S_EBADF); }
HLE(_ZN2nn6socket10SetSockOptEiiiPKvj) { sock_fail(c, S_EBADF); }
HLE(_ZN2nn6socket8ShutdownEii) { sock_fail(c, S_EBADF); }
HLE(_ZN2nn6socket5CloseEi) { sock_fail(c, S_EBADF); }
HLE(_ZN2nn6socket5FcntlEiiz) { sock_fail(c, S_EBADF); }
/* Poll(pollfd* fds, nfds_t n, int timeout_ms): nothing is ever ready. Waits out the timeout (in 50 ms steps at most,
   which a poll loop takes as a spurious wake) and reports a timeout, as Longshot's nn::socket::detail::Poll. */
HLE(_ZN2nn6socket4PollEP6pollfdmi) {
    const uint64_t fds = ARG(0), n = ARG(1) < 4096 ? ARG(1) : 4096;
    const int timeout = (int)ARG(2);
    if (fds && hos_guest_range(fds, 8 * n))
        for (uint64_t i = 0; i < n; i++) *(uint16_t *)GP(fds + 8 * i + 6) = 0;   /* revents */
    if (timeout) {
        const int64_t ms = timeout < 0 || timeout > 50 ? 50 : timeout;
        hos_sleep_ns(ms * 1000000);
    }
    RET(0);
}
HLE(_ZN2nn6socket12GetLastErrorEv) { RET(sock_errno); }
HLE(_ZN2nn6socket12GetLastErrnoEv) { RET(sock_errno); }
/* GetAddrInfo(char const* node, char const* service, AddrInfo const* hints, AddrInfo** out) -> AiErrno */
HLE(_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_) {
    if (ARG(3)) G64(ARG(3)) = 0;
    RET(S_EAI_AGAIN);
}
HLE(_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_) {
    if (ARG(3)) G64(ARG(3)) = 0;
    RET(S_EAI_AGAIN);
}
HLE(_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE) { (void)c; }
HLE(_ZN2nn6socket12FreeAddrInfoEP8addrinfo) { (void)c; }
static uint64_t guest_string(const char *s) {   /* a constant string in guest memory, made once */
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static struct { const char *s; uint64_t at; } made[16];
    uint64_t at = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 16 && !at; i++) {
        if (!made[i].s) {
            made[i].at = hos_sys_alloc(strlen(s) + 1, 16);
            memcpy(GP(made[i].at), s, strlen(s) + 1);
            made[i].s = s;
        }
        if (made[i].s == s) at = made[i].at;
    }
    pthread_mutex_unlock(&lock);
    return at;
}
HLE(_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE) {
    RET(guest_string((int)ARG(0) == S_EAI_AGAIN ? "Temporary failure in name resolution" : "Name lookup failed"));
}

static uint16_t bswap16(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
HLE(_ZN2nn6socket9InetHtonsEt) { RET(bswap16((uint16_t)ARG(0))); }
HLE(_ZN2nn6socket9InetNtohsEt) { RET(bswap16((uint16_t)ARG(0))); }
HLE(_ZN2nn6socket9InetHtonlEj) { RET(__builtin_bswap32((uint32_t)ARG(0))); }
HLE(_ZN2nn6socket9InetNtohlEj) { RET(__builtin_bswap32((uint32_t)ARG(0))); }

/* a dotted quad (a.b.c.d, each 0-255) to an address in network order: 1, or 0 when it is not one */
static int parse_ipv4(const char *s, uint32_t *out) {
    uint8_t b[4];
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return 0;
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 4) v = v * 10 + (unsigned)(*s++ - '0'), digits++;
        if (v > 255 || digits > 3) return 0;
        b[i] = (uint8_t)v;
        if (i < 3 && *s++ != '.') return 0;
    }
    if (*s) return 0;
    memcpy(out, b, 4);
    return 1;
}
static void format_ipv4(uint32_t a, char out[16]) {
    const uint8_t *b = (const uint8_t *)&a;
    snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}
/* InetPton(int family, char const* src, void* dst) -> 1, 0 (not an address) or -1 (family) */
HLE(_ZN2nn6socket8InetPtonEiPKcPv) {
    uint32_t a;
    if ((int)ARG(0) != S_AF_INET) { sock_fail(c, S_EAFNOSUPPORT); return; }
    if (!parse_ipv4(GP(ARG(1)), &a)) { RET(0); return; }
    memcpy(GP(ARG(2)), &a, 4);
    RET(1);
}
/* InetNtop(int family, void const* src, char* dst, socklen_t size) -> dst, or NULL */
HLE(_ZN2nn6socket8InetNtopEiPKvPcj) {
    char s[16];
    if ((int)ARG(0) != S_AF_INET) { sock_fail(c, S_EAFNOSUPPORT); RET(0); return; }
    format_ipv4(G32(ARG(1)), s);
    if (strlen(s) + 1 > (uint32_t)ARG(3)) { RET(0); return; }
    memcpy(GP(ARG(2)), s, strlen(s) + 1);
    RET(ARG(2));
}
/* InetAton(char const* cp, InAddr* out) -> 1 or 0 */
HLE(_ZN2nn6socket8InetAtonEPKcPNS0_6InAddrE) {
    uint32_t a;
    const int ok = parse_ipv4(GP(ARG(0)), &a);
    if (ok && ARG(1)) G32(ARG(1)) = a;
    RET(ok);
}
/* InetNtoa(in_addr) -> char*: a buffer of the caller's thread, as the C library's */
HLE(_ZN2nn6socket8InetNtoaE7in_addr) {
    static __thread uint64_t buf;
    if (!buf) buf = hos_sys_alloc(16, 16);
    format_ipv4((uint32_t)ARG(0), GP(buf));
    RET(buf);
}
