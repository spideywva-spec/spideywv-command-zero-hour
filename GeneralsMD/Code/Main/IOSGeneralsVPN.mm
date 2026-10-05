#include "IOSGeneralsVPN.h"

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE

#import <Foundation/Foundation.h>
#import <NetworkExtension/NetworkExtension.h>
#include <atomic>
#include <cstdio>

namespace {
std::atomic<bool> gConnected(false);
NETunnelProviderManager *gManager = nil;

NSString *GXString(const char *value) {
    return value ? [NSString stringWithUTF8String:value] : nil;
}

void ConfigureAndStart(NSString *relayURL, NSString *lobbyID,
                       NSString *playerToken, NSString *virtualIP) {
    [NETunnelProviderManager loadAllFromPreferencesWithCompletionHandler:
        ^(NSArray<NETunnelProviderManager *> *managers, NSError *error) {
        if (error) {
            fprintf(stderr, "ERROR: Generals VPN preferences: %s\\n", error.localizedDescription.UTF8String);
            gConnected.store(false);
            return;
        }

        NETunnelProviderManager *manager = managers.firstObject;
        if (!manager) manager = [[NETunnelProviderManager alloc] init];

        NSString *bundleID = NSBundle.mainBundle.bundleIdentifier;
        NSString *providerID = [bundleID stringByAppendingString:@".GeneralsVPN"];

        NETunnelProviderProtocol *protocol = [[NETunnelProviderProtocol alloc] init];
        protocol.providerBundleIdentifier = providerID;
        protocol.serverAddress = @"GeneralsXZH Virtual LAN";
        protocol.providerConfiguration = @{
            @"relayURL": relayURL,
            @"lobbyID": lobbyID,
            @"playerToken": playerToken,
            @"virtualIP": virtualIP
        };

        manager.protocolConfiguration = protocol;
        manager.localizedDescription = @"GeneralsXZH Virtual LAN";
        manager.enabled = YES;

        [manager saveToPreferencesWithCompletionHandler:^(NSError *saveError) {
            if (saveError) {
                fprintf(stderr, "ERROR: Generals VPN save: %s\\n", saveError.localizedDescription.UTF8String);
                gConnected.store(false);
                return;
            }

            [manager loadFromPreferencesWithCompletionHandler:^(NSError *reloadError) {
                if (reloadError) {
                    fprintf(stderr, "ERROR: Generals VPN reload: %s\\n", reloadError.localizedDescription.UTF8String);
                    gConnected.store(false);
                    return;
                }

                gManager = manager;
                NSError *startError = nil;
                [manager.connection startVPNTunnelAndReturnError:&startError];
                if (startError) {
                    fprintf(stderr, "ERROR: Generals VPN start: %s\\n", startError.localizedDescription.UTF8String);
                    gConnected.store(false);
                    return;
                }

                gConnected.store(true);
                fprintf(stderr, "INFO: Generals embedded VPN started at %s\\n", virtualIP.UTF8String);
            }];
        }];
    }];
}
}

bool GeneralsXStartVPN(const char *relayURL, const char *lobbyID,
                       const char *playerToken, const char *virtualIP) {
    NSString *relay = GXString(relayURL);
    NSString *lobby = GXString(lobbyID);
    NSString *token = GXString(playerToken);
    NSString *ip = GXString(virtualIP);
    if (!relay.length || !lobby.length || !token.length || !ip.length) return false;
    ConfigureAndStart(relay, lobby, token, ip);
    return true;
}

void GeneralsXStopVPN(void) {
    gConnected.store(false);
    [gManager.connection stopVPNTunnel];
    gManager = nil;
}

bool GeneralsXVPNIsConnected(void) {
    return gConnected.load();
}

#endif
