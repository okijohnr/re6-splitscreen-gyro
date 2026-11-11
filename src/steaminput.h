// Minimal hand-rolled access to ISteamClient / ISteamInput006 via vtable index.
// We call through explicit __thiscall function pointers rather than C++ virtuals
// so the MSVC ABI (incl. hidden struct-return pointer) is matched exactly.
// Indices taken from Steamworks SDK 1.58 isteaminput.h / isteamclient.h.
#pragma once
#include <stdint.h>

typedef uint64_t InputHandle_t;
typedef int32_t HSteamUser;
typedef int32_t HSteamPipe;

#define STEAM_INPUT_MAX_COUNT 16

typedef struct {
    float rotQuatX, rotQuatY, rotQuatZ, rotQuatW;
    float posAccelX, posAccelY, posAccelZ;
    float rotVelX, rotVelY, rotVelZ;
} InputMotionData_t;

enum {
    SC_GetISteamGenericInterface = 12,

    SI_Init = 0,
    SI_Shutdown = 1,
    SI_RunFrame = 3,
    SI_GetConnectedControllers = 6,
    SI_GetMotionData = 29,
    SI_GetInputTypeForHandle = 37,
    SI_GetControllerForGamepadIndex = 38,
    SI_GetGamepadIndexForController = 39,
};

#define VT(obj, idx) ((*(void ***)(obj))[idx])

typedef void *(__thiscall *pfnGetGenericInterface)(void *self, HSteamUser, HSteamPipe, const char *);
typedef uint8_t (__thiscall *pfnSI_Init)(void *self, uint8_t bExplicitlyCallRunFrame);
typedef void (__thiscall *pfnSI_RunFrame)(void *self, uint8_t bReserved);
typedef int (__thiscall *pfnSI_GetConnected)(void *self, InputHandle_t *out);
// struct return: MSVC thiscall passes hidden result pointer as first stack arg, returns it in EAX
typedef InputMotionData_t *(__thiscall *pfnSI_GetMotion)(void *self, InputMotionData_t *ret, InputHandle_t h);
typedef int (__thiscall *pfnSI_GetInputType)(void *self, InputHandle_t h);
typedef int (__thiscall *pfnSI_GetGamepadIndex)(void *self, InputHandle_t h);

static inline const char *steam_input_type_name(int t)
{
    switch (t) {
    case 0: return "Unknown";
    case 1: return "SteamController";
    case 2: return "XBox360";
    case 3: return "XBoxOne";
    case 4: return "GenericGamepad";
    case 5: return "PS4";
    case 6: return "AppleMFi";
    case 7: return "AndroidController";
    case 8: return "SwitchJoyConPair";
    case 9: return "SwitchJoyConSingle";
    case 10: return "SwitchProController";
    case 11: return "MobileTouch";
    case 12: return "PS3";
    case 13: return "PS5";
    case 14: return "SteamDeck";
    default: return "?";
    }
}
