#pragma once
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#ifdef __cplusplus
extern "C" {
#endif
bool GeneralsXStartVPN(const char *relayURL, const char *lobbyID, const char *playerToken, const char *virtualIP);
void GeneralsXStopVPN(void);
bool GeneralsXVPNIsConnected(void);
#ifdef __cplusplus
}
#endif
#endif
