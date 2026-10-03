#pragma once

#ifndef _WIN32

#include <SDL3/SDL.h>

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#ifdef __cplusplus
extern "C" {
#endif

// Installs the small in-game iOS ESC touch button in the top-left corner.
// The button is part of the game binary, not the native launcher.
void GeneralsXInstallIOSEscOverlay(SDL_Window *window);

// Removes the overlay during game shutdown.
void GeneralsXRemoveIOSEscOverlay(void);

#ifdef __cplusplus
}
#endif
#endif

#endif // !_WIN32
