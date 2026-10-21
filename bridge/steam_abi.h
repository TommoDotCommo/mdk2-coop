/* Minimal Windows x86 Steam ABI declarations, not an SDK dependency.
 * Interface versions are requested explicitly; only the named slots below are used.
 * Sources: Valve's SDK declarations and Windows ABI wrappers in Proton:
 * https://github.com/ValveSoftware/Proton/tree/proton_10.0/lsteamclient
 * https://github.com/ValveSoftware/GameNetworkingSockets/tree/master/include/steam
 * In particular, CSteamID returns use an explicit hidden result pointer on Win32.
 */
#ifndef STEAM_ABI_H
#define STEAM_ABI_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SMETHOD(obj, slot, ret, ...) \
    ((ret (__thiscall *)(void *, ##__VA_ARGS__))(*(void ***)(obj))[slot])

enum {
    SC_USER = 5, SC_FRIENDS = 8, SC_UTILS = 9, SC_MATCHMAKING = 10, SC_GENERIC = 12,
    SU_LOGGED_ON = 1, SU_ID = 2,
    SF_INVITE = 33, SF_PRESENCE = 43,
    MM_CREATE = 13, MM_JOIN = 14, MM_LEAVE = 15, MM_MEMBERS = 17, MM_MEMBER = 18,
    MM_DATA = 19, MM_SET_DATA = 20, MM_JOINABLE = 34, MM_OWNER = 35,
    UT_APP_ID = 9, UT_COMPLETED = 11, UT_RESULT = 13,
    NM_SEND = 0, NM_RECEIVE = 1, NM_ACCEPT = 2, NM_CLOSE = 3,
    CB_JOIN = 333, CB_PRESENCE_JOIN = 337, CB_ENTER = 504, CB_CHAT_UPDATE = 506,
    CB_CREATED = 513, CB_SESSION_REQUEST = 1251
};

/* The SDK packs identities to one byte. Some Steam client versions align the
 * identity in a message to 8 bytes, others put it immediately after m_conn. */
#pragma pack(push, 1)
struct steam_identity {
    int type, size;
    union { uint64_t id; unsigned char reserved[128]; } data;
};
#pragma pack(pop)

#pragma pack(push, 8)
struct steam_message {
    void *data;
    int size;
    uint32_t connection;
    uint32_t identity_padding;
    struct steam_identity identity;
    int64_t user_data, received, number;
    void (__cdecl *free_data)(struct steam_message *);
    void (__cdecl *release)(struct steam_message *);
};
struct steam_created { int result; uint64_t lobby; };
struct steam_enter { uint64_t lobby; uint32_t permissions; bool locked; uint32_t response; };
struct steam_join_requested { uint64_t lobby, friend_id; };
struct steam_presence_join { uint64_t friend_id; char connect[256]; };
struct steam_chat_update { uint64_t lobby, changed, actor; uint32_t state; };
#pragma pack(pop)

/* CCallbackBase has no virtual destructor. Never use manual dispatch on the
 * game's pipe: it would consume callbacks belonging to the original game. */
struct steam_callback;
struct steam_callback_vtable {
    void (__thiscall *run)(struct steam_callback *, void *);
    void (__thiscall *run_result)(struct steam_callback *, void *, bool, uint64_t);
    int (__thiscall *size)(struct steam_callback *);
};
struct steam_callback {
    const struct steam_callback_vtable *vtable;
    unsigned char flags;
    int id;
    int bytes;
};

_Static_assert(sizeof(void *) == 4, "MDK2 HD requires the Win32 Steam ABI");
_Static_assert(sizeof(struct steam_identity) == 136, "Steam identity ABI");
_Static_assert(offsetof(struct steam_message, identity) == 16, "Steam message ABI");
_Static_assert(offsetof(struct steam_message, release) == 180, "Steam release ABI");
_Static_assert(offsetof(struct steam_created, lobby) == 8, "Steam callback packing");
_Static_assert(sizeof(struct steam_enter) == 24, "Steam lobby enter ABI");
_Static_assert(offsetof(struct steam_callback, id) == 8, "Steam callback ABI");
#endif
