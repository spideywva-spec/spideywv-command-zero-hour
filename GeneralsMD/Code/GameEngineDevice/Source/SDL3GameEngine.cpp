/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
** SDL3GameEngine.cpp
**
** Linux implementation of GameEngine using SDL3 for windowing/input.
**
** TheSuperHackers @feature CnC_Generals_Linux 07/02/2026
** Provides SDL3-based input and window management for Linux builds.
** Based on fighter19 reference implementation.
*/

#ifndef _WIN32

#include "SDL3GameEngine.h"
#include "OpenALAudioManager.h"
#include "SDL3Device/GameClient/SDL3Mouse.h"
#include "SDL3Device/GameClient/SDL3Keyboard.h"
#include "GameClient/Mouse.h"
#include "GameClient/Keyboard.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Gadget.h"
#include "W3DDevice/GameLogic/W3DGameLogic.h"
#include "W3DDevice/GameClient/W3DGameClient.h"
#include "W3DDevice/Common/W3DModuleFactory.h"
#include "W3DDevice/Common/W3DThingFactory.h"
#include "W3DDevice/Common/W3DFunctionLexicon.h"
#include "W3DDevice/Common/W3DRadar.h"
#include "W3DDevice/GameClient/W3DParticleSys.h"
#include "W3DDevice/GameClient/W3DWebBrowser.h"
#include "StdDevice/Common/StdLocalFileSystem.h"
#include "StdDevice/Common/StdBIGFileSystem.h"
#include "Common/GlobalData.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// Extern globals for input devices (set by GameClient)
extern Mouse *TheMouse;
extern Keyboard *TheKeyboard;
extern GameWindowManager *TheWindowManager;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#include <atomic>

// ---------------------------------------------------------------------------
// iOS app lifecycle
//
// iOS suspends the process when the app leaves the foreground. Any GPU work
// submitted around suspension stalls on drawable acquisition (MoltenVK waits
// out a timeout per present), which surfaces as multi-second input hangs right
// after resuming. SDL warns that lifecycle events can arrive outside the
// normal poll cycle, so they are captured in an event watcher that fires
// immediately on the delivering thread; the engine update loop checks the
// flag and skips simulation + rendering while backgrounded.
// ---------------------------------------------------------------------------
// Two independent reasons to halt the render/sim loop on iOS:
//  - BACKGROUNDED (home / switched away): the process is about to be suspended.
//  - INACTIVE (multitasking switcher open, Control Center, a notification
//    banner): iOS snapshots the window and owns the CAMetalLayer drawable during
//    this window — and crucially, opening the app switcher fires resign-active
//    WITHOUT a full background transition.
// Acquiring a Metal drawable during EITHER state fights iOS for the layer; across
// repeated suspend/switcher cycles MoltenVK is driven into an unrecoverable
// surface state and the app crashes (the reported "crashes after backgrounding /
// multitasking a few times"). Pause whenever either is set.
static std::atomic<bool> s_appBackgrounded{false};
static std::atomic<bool> s_appInactive{false};

static inline bool iosShouldPauseRendering()
{
	return s_appBackgrounded.load() || s_appInactive.load();
}

static bool SDLCALL iosLifecycleWatcher(void *userdata, SDL_Event *event)
{
	switch (event->type) {
		case SDL_EVENT_WILL_ENTER_BACKGROUND:
		case SDL_EVENT_DID_ENTER_BACKGROUND:
			s_appBackgrounded.store(true);
			break;
		case SDL_EVENT_DID_ENTER_FOREGROUND:
			s_appBackgrounded.store(false);
			break;
		// Resign/become active. On iOS, SDL maps applicationWillResignActive ->
		// window focus lost and applicationDidBecomeActive -> window focus gained.
		// Stay paused until fully active again (focus regained), which arrives
		// after DID_ENTER_FOREGROUND.
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			s_appInactive.store(true);
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			s_appInactive.store(false);
			break;
		default:
			break;
	}
	return true;
}

// ---------------------------------------------------------------------------
// iOS touch -> mouse gesture translation
//
// One-finger normal:
//   tap                  -> LMB click
//   drag before 250 ms   -> RMB drag camera pan
//   hold 250 ms + drag   -> LMB selection rectangle
//
// Building placement:
//   movement             -> move preview only; camera locked
//   short tap            -> LMB build
//   stationary hold 200 ms -> MMB building rotation
//   movement after hold  -> rotate building with the same finger
//   release after rotate -> MMB up + LMB build
//
// Two fingers:
//   pinch                -> wheel zoom
//   twist                -> MMB camera rotation
//   quick stationary tap -> RMB cancel
//
// Building rotation is strictly single-finger.
namespace {

struct TouchState {
	enum Phase {
		IDLE,
		ONE_PENDING,
		CAMERA_PAN,
		SELECTION,
		BUILD_PREVIEW,
		TWO_FINGER
	};

	Phase phase = IDLE;
	SDL_FingerID finger1 = 0;
	SDL_FingerID finger2 = 0;

	float downX = 0.0f, downY = 0.0f;
	float lastX = 0.0f, lastY = 0.0f;
	float f1x = 0.0f, f1y = 0.0f;
	float f2x = 0.0f, f2y = 0.0f;

	Uint64 downTicks = 0;

	// Building state.
	bool buildMoved = false;
	bool buildRotationActive = false;

	// Two-finger state.
	Uint64 twoFingerDownTicks = 0;
	float twoFingerStart1X = 0.0f, twoFingerStart1Y = 0.0f;
	float twoFingerStart2X = 0.0f, twoFingerStart2Y = 0.0f;
	float lastDistance = 0.0f;
	float lastAngle = 0.0f;
	float rotationAccum = 0.0f;
	bool twoFingerMoved = false;
	bool twoFingerRotationActive = false;
	bool twoFingerBuildCancel = false;
};

TouchState s_touch;

constexpr Uint64 SELECTION_HOLD_MS = 250;
constexpr Uint64 BUILD_ROTATION_HOLD_MS = 200;
constexpr Uint64 TWO_FINGER_TAP_MS = 300;
constexpr Uint64 DOUBLE_TAP_MS = 300;

constexpr float TOUCH_MOVE_EPSILON_PX = 3.0f;
constexpr float DOUBLE_TAP_SLOP_PX = 24.0f;
constexpr float TWO_FINGER_SLOP_PX = 10.0f;
constexpr float TWO_FINGER_ROTATION_DEGREES = 40.0f;
constexpr float PINCH_WHEEL_SCALE = 0.035f;
constexpr float TWO_FINGER_ROTATION_SPEED = 1.75f;

// Camera momentum is intentionally kept entirely inside this .cpp.
// Velocity is measured from real touch motion and decays exponentially
// after release, while the synthetic RMB remains held until the glide ends.
bool s_cameraMomentumActive = false;
float s_cameraVelocityX = 0.0f;
float s_cameraVelocityY = 0.0f;
Uint64 s_cameraMomentumLastTicks = 0;
Uint64 s_cameraLastMotionTicks = 0;

Uint64 s_lastTapTicks = 0;
float s_lastTapX = 0.0f;
float s_lastTapY = 0.0f;
bool s_haveLastTap = false;

float s_lastSyntheticX = 0.0f;
float s_lastSyntheticY = 0.0f;
bool s_haveSyntheticPosition = false;

static bool isBuildingPlacementMode(SDL3Mouse *mouse)
{
	if (!mouse) return false;

	const Mouse::MouseCursor cursor = mouse->getMouseCursor();
	return cursor == Mouse::BUILD_PLACEMENT ||
	       cursor == Mouse::INVALID_BUILD_PLACEMENT;
}

static float touchDistancePx(float x1, float y1, float x2, float y2,
                             int width, int height)
{
	const float dx = (x1 - x2) * static_cast<float>(width);
	const float dy = (y1 - y2) * static_cast<float>(height);
	return SDL_sqrtf(dx * dx + dy * dy);
}

static float touchAngle(float x1, float y1, float x2, float y2)
{
	return SDL_atan2f(y2 - y1, x2 - x1);
}

static float normalizedAngleDelta(float current, float previous)
{
	constexpr float PI = 3.14159265358979323846f;
	float delta = current - previous;
	while (delta > PI) delta -= 2.0f * PI;
	while (delta < -PI) delta += 2.0f * PI;
	return delta;
}

static void stopCameraMomentum(SDL3Mouse *mouse, SDL_Window *window)
{
	if (s_cameraMomentumActive) {
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
		                   s_lastSyntheticX, s_lastSyntheticY,
		                   SDL_BUTTON_RIGHT);
	}

	s_cameraMomentumActive = false;
	s_cameraVelocityX = 0.0f;
	s_cameraVelocityY = 0.0f;
	s_cameraMomentumLastTicks = 0;
}

static void resetTouchState()
{
	s_touch = TouchState{};
	s_haveSyntheticPosition = false;
}

static void sendSyntheticMouse(SDL3Mouse *mouse, SDL_Window *window,
                               Uint32 type, float x, float y,
                               Uint8 button = 0, float wheelY = 0.0f,
                               Uint8 clicks = 1,
                               bool suppressMotionDelta = false)
{
	if (!mouse || !window) return;

	SDL_Event event;
	SDL_zero(event);
	event.type = type;

	const SDL_WindowID windowID = SDL_GetWindowID(window);

	switch (type) {
	case SDL_EVENT_MOUSE_MOTION:
		event.motion.windowID = windowID;
		event.motion.which = 0;
		event.motion.x = x;
		event.motion.y = y;
		if (s_haveSyntheticPosition && !suppressMotionDelta) {
			event.motion.xrel = x - s_lastSyntheticX;
			event.motion.yrel = y - s_lastSyntheticY;
		} else {
			event.motion.xrel = 0.0f;
			event.motion.yrel = 0.0f;
		}
		s_lastSyntheticX = x;
		s_lastSyntheticY = y;
		s_haveSyntheticPosition = true;
		break;

	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
		event.button.windowID = windowID;
		event.button.which = 0;
		event.button.button = button;
		event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
		event.button.clicks = clicks;
		event.button.x = x;
		event.button.y = y;
		break;

	case SDL_EVENT_MOUSE_WHEEL:
		event.wheel.windowID = windowID;
		event.wheel.which = 0;
		event.wheel.x = 0.0f;
		event.wheel.y = wheelY;
		event.wheel.mouse_x = x;
		event.wheel.mouse_y = y;
		break;

	default:
		return;
	}

	mouse->addSDLEvent(&event);
}

static void beginTwoFingerGesture(SDL3Mouse *mouse, SDL_Window *window,
                                  int width, int height, bool buildCancelOnly)
{
	s_touch.phase = TouchState::TWO_FINGER;
	s_touch.twoFingerDownTicks = SDL_GetTicks();
	s_touch.twoFingerMoved = false;
	s_touch.twoFingerRotationActive = false;
	s_touch.twoFingerBuildCancel = buildCancelOnly;
	s_touch.rotationAccum = 0.0f;

	s_touch.twoFingerStart1X = s_touch.f1x;
	s_touch.twoFingerStart1Y = s_touch.f1y;
	s_touch.twoFingerStart2X = s_touch.f2x;
	s_touch.twoFingerStart2Y = s_touch.f2y;

	s_touch.lastDistance =
		touchDistancePx(s_touch.f1x, s_touch.f1y,
		                s_touch.f2x, s_touch.f2y, width, height);
	s_touch.lastAngle =
		touchAngle(s_touch.f1x, s_touch.f1y,
		           s_touch.f2x, s_touch.f2y);

	const float centerX =
		(s_touch.f1x + s_touch.f2x) * 0.5f * static_cast<float>(width);
	const float centerY =
		(s_touch.f1y + s_touch.f2y) * 0.5f * static_cast<float>(height);

	sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION,
	                   centerX, centerY, 0, 0.0f, 1, true);
}

static void finishBuilding(SDL3Mouse *mouse, SDL_Window *window,
                           float x, float y)
{
	if (s_touch.buildRotationActive) {
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
		                   x, y, SDL_BUTTON_MIDDLE);

		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
		                   x, y, SDL_BUTTON_LEFT);
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
		                   x, y, SDL_BUTTON_LEFT);
		resetTouchState();
		return;
	}

	if (!s_touch.buildMoved) {
		// A stationary touch on an already-fixed preview is the fast
		// build confirmation path.
		const Uint64 now = SDL_GetTicks();
		const bool doubleTap =
			s_haveLastTap &&
			now - s_lastTapTicks <= DOUBLE_TAP_MS &&
			SDL_sqrtf((x - s_lastTapX) * (x - s_lastTapX) +
			         (y - s_lastTapY) * (y - s_lastTapY)) <= DOUBLE_TAP_SLOP_PX;

		(void)doubleTap;
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
		                   x, y, SDL_BUTTON_LEFT);
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
		                   x, y, SDL_BUTTON_LEFT);
		resetTouchState();
		return;
	}

	// Placement drag ends by fixing the preview at its current position.
	// The camera remains locked and the next touch chooses build or rotation.
	s_touch.phase = TouchState::BUILD_PREVIEW;
	s_touch.finger1 = 0;
	s_touch.finger2 = 0;
	s_touch.buildMoved = false;
	s_touch.buildRotationActive = false;
	s_touch.downTicks = 0;
	s_touch.lastX = x;
	s_touch.lastY = y;
	s_touch.f1x = 0.0f;
	s_touch.f1y = 0.0f;
	s_haveSyntheticPosition = false;
}

static void rememberTap(float x, float y)
{
	s_lastTapTicks = SDL_GetTicks();
	s_lastTapX = x;
	s_lastTapY = y;
	s_haveLastTap = true;
}

static void sendTap(SDL3Mouse *mouse, SDL_Window *window,
                    float x, float y)
{
	const Uint64 now = SDL_GetTicks();
	const bool doubleTap =
		s_haveLastTap &&
		now - s_lastTapTicks <= DOUBLE_TAP_MS &&
		SDL_sqrtf((x - s_lastTapX) * (x - s_lastTapX) +
		         (y - s_lastTapY) * (y - s_lastTapY)) <= DOUBLE_TAP_SLOP_PX;

	const Uint8 clicks = doubleTap ? 2 : 1;
	sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
	                   x, y, SDL_BUTTON_LEFT, 0.0f, clicks);
	sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
	                   x, y, SDL_BUTTON_LEFT, 0.0f, clicks);

	if (doubleTap) {
		s_haveLastTap = false;
	} else {
		rememberTap(x, y);
	}
}

static void startCameraPan(SDL3Mouse *mouse, SDL_Window *window,
                           float x, float y)
{
	stopCameraMomentum(mouse, window);

	s_cameraLastMotionTicks = SDL_GetTicks();
	s_cameraVelocityX = 0.0f;
	s_cameraVelocityY = 0.0f;

	sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
	                   x, y, SDL_BUTTON_RIGHT);
	sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, x, y,
	                   0, 0.0f, 1, true);
}

static void updateCameraVelocity(float dx, float dy, Uint64 now)
{
	Uint64 dtMs = now - s_cameraLastMotionTicks;
	if (dtMs == 0) dtMs = 1;
	if (dtMs > 100) dtMs = 100;

	const float dt = static_cast<float>(dtMs) * 0.001f;
	const float instantX = dx / dt;
	const float instantY = dy / dt;

	// Low-pass the physical finger velocity. This removes event jitter while
	// preserving the user's actual speed and direction.
	constexpr float VELOCITY_RESPONSE = 0.55f;
	s_cameraVelocityX += (instantX - s_cameraVelocityX) * VELOCITY_RESPONSE;
	s_cameraVelocityY += (instantY - s_cameraVelocityY) * VELOCITY_RESPONSE;
	s_cameraLastMotionTicks = now;
}

static void startBuildingRotation(SDL3Mouse *mouse, SDL_Window *window)
{
	if (s_touch.buildMoved || s_touch.buildRotationActive ||
	    s_touch.finger1 == 0) {
		return;
	}

	s_touch.buildRotationActive = true;

	// Establish a zero-delta origin. The next physical finger motion becomes
	// pure relative MMB rotation, with no initial jump.
	sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION,
	                   s_touch.lastX, s_touch.lastY,
	                   0, 0.0f, 1, true);
	sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
	                   s_touch.lastX, s_touch.lastY,
	                   SDL_BUTTON_MIDDLE);
}

void handleTouchEvent(SDL3Mouse *mouse, SDL_Window *window,
                      const SDL_Event &event)
{
	if (!mouse || !window) return;

	int width = 0, height = 0;
	SDL_GetWindowSize(window, &width, &height);

	const float x = event.tfinger.x * static_cast<float>(width);
	const float y = event.tfinger.y * static_cast<float>(height);

	switch (event.type) {
	case SDL_EVENT_FINGER_DOWN:
	{
		const SDL_FingerID id = event.tfinger.fingerID;

		if (s_touch.phase == TouchState::IDLE) {
			s_touch.phase = isBuildingPlacementMode(mouse)
				? TouchState::BUILD_PREVIEW
				: TouchState::ONE_PENDING;

			s_touch.finger1 = id;
			s_touch.downX = s_touch.lastX = x;
			s_touch.downY = s_touch.lastY = y;
			s_touch.f1x = event.tfinger.x;
			s_touch.f1y = event.tfinger.y;
			s_touch.downTicks = SDL_GetTicks();
			s_touch.buildMoved = false;
			s_touch.buildRotationActive = false;

			// Any new touch interrupts an old camera glide.
			stopCameraMomentum(mouse, window);

			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION,
			                   x, y, 0, 0.0f, 1, true);
			return;
		}

		// After a placement drag has been released, the preview remains
		// fixed. A new single finger is the build/rotate decision touch.
		if (s_touch.phase == TouchState::BUILD_PREVIEW &&
		    s_touch.finger1 == 0) {
			s_touch.finger1 = id;
			s_touch.downX = s_touch.lastX = x;
			s_touch.downY = s_touch.lastY = y;
			s_touch.f1x = event.tfinger.x;
			s_touch.f1y = event.tfinger.y;
			s_touch.downTicks = SDL_GetTicks();
			s_touch.buildMoved = false;
			s_touch.buildRotationActive = false;

			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION,
			                   x, y, 0, 0.0f, 1, true);
			return;
		}

		if (s_touch.finger1 != 0 && id != s_touch.finger1) {
			// During building this becomes a cancel-only two-finger gesture.
			// It can never feed pinch/twist data into the camera.
			if (s_touch.phase == TouchState::BUILD_PREVIEW) {
				beginTwoFingerGesture(mouse, window, width, height, true);
				return;
			}

			// Outside building, terminate the one-finger gesture cleanly.
			if (s_touch.phase == TouchState::CAMERA_PAN) {
				sendSyntheticMouse(mouse, window,
				                   SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.lastX, s_touch.lastY,
				                   SDL_BUTTON_RIGHT);
			} else if (s_touch.phase == TouchState::SELECTION) {
				sendSyntheticMouse(mouse, window,
				                   SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.lastX, s_touch.lastY,
				                   SDL_BUTTON_LEFT);
			}

			s_touch.finger2 = id;
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
			beginTwoFingerGesture(mouse, window, width, height, false);
			return;
		}
		return;
	}

	case SDL_EVENT_FINGER_MOTION:
	{
		const SDL_FingerID id = event.tfinger.fingerID;

		if (id == s_touch.finger1) {
			s_touch.f1x = event.tfinger.x;
			s_touch.f1y = event.tfinger.y;
		} else if (id == s_touch.finger2) {
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
		} else {
			return;
		}

		if (s_touch.phase == TouchState::BUILD_PREVIEW &&
		    id == s_touch.finger1) {
			const float dx = x - s_touch.downX;
			const float dy = y - s_touch.downY;
			const bool moved =
				SDL_sqrtf(dx * dx + dy * dy) >= TOUCH_MOVE_EPSILON_PX;

			if (s_touch.buildRotationActive) {
				sendSyntheticMouse(mouse, window,
				                   SDL_EVENT_MOUSE_MOTION, x, y);
			} else {
				if (moved) {
					s_touch.buildMoved = true;
				}

				// Absolute preview tracking: no xrel/yrel reaches the
				// camera, so the camera is hard-locked throughout placement.
				sendSyntheticMouse(mouse, window,
				                   SDL_EVENT_MOUSE_MOTION,
				                   x, y, 0, 0.0f, 1, true);
			}

			s_touch.lastX = x;
			s_touch.lastY = y;
			return;
		}

		if (s_touch.phase == TouchState::ONE_PENDING &&
		    id == s_touch.finger1) {
			const float dx = x - s_touch.downX;
			const float dy = y - s_touch.downY;
			if (SDL_sqrtf(dx * dx + dy * dy) < TOUCH_MOVE_EPSILON_PX) {
				return;
			}

			const Uint64 now = SDL_GetTicks();
			const bool selectionHeld =
				(now - s_touch.downTicks) >= SELECTION_HOLD_MS;

			s_touch.lastX = x;
			s_touch.lastY = y;

			if (selectionHeld) {
				s_touch.phase = TouchState::SELECTION;
				sendSyntheticMouse(mouse, window,
				                   SDL_EVENT_MOUSE_BUTTON_DOWN,
				                   s_touch.downX, s_touch.downY,
				                   SDL_BUTTON_LEFT);
				sendSyntheticMouse(mouse, window,
				                   SDL_EVENT_MOUSE_MOTION, x, y);
			} else {
				s_touch.phase = TouchState::CAMERA_PAN;
				startCameraPan(mouse, window, s_touch.downX, s_touch.downY);
				updateCameraVelocity(dx, dy, now);
				sendSyntheticMouse(mouse, window,
				                   SDL_EVENT_MOUSE_MOTION, x, y);
			}
			return;
		}

		if (s_touch.phase == TouchState::CAMERA_PAN &&
		    id == s_touch.finger1) {
			const float dx = x - s_touch.lastX;
			const float dy = y - s_touch.lastY;
			const Uint64 now = SDL_GetTicks();

			updateCameraVelocity(dx, dy, now);
			sendSyntheticMouse(mouse, window,
			                   SDL_EVENT_MOUSE_MOTION, x, y);
			s_touch.lastX = x;
			s_touch.lastY = y;
			return;
		}

		if (s_touch.phase == TouchState::SELECTION &&
		    id == s_touch.finger1) {
			sendSyntheticMouse(mouse, window,
			                   SDL_EVENT_MOUSE_MOTION, x, y);
			s_touch.lastX = x;
			s_touch.lastY = y;
			return;
		}

		if (s_touch.phase == TouchState::TWO_FINGER) {
			if (s_touch.twoFingerBuildCancel) {
				// Building remains completely camera-locked even if the
				// cancel gesture fingers move around.
				const float move1 =
					touchDistancePx(s_touch.f1x, s_touch.f1y,
					                s_touch.twoFingerStart1X,
					                s_touch.twoFingerStart1Y,
					                width, height);
				const float move2 =
					touchDistancePx(s_touch.f2x, s_touch.f2y,
					                s_touch.twoFingerStart2X,
					                s_touch.twoFingerStart2Y,
					                width, height);
				if (move1 > TWO_FINGER_SLOP_PX ||
				    move2 > TWO_FINGER_SLOP_PX) {
					s_touch.twoFingerMoved = true;
				}
				return;
			}

			const float move1 =
				touchDistancePx(s_touch.f1x, s_touch.f1y,
				                s_touch.twoFingerStart1X,
				                s_touch.twoFingerStart1Y,
				                width, height);
			const float move2 =
				touchDistancePx(s_touch.f2x, s_touch.f2y,
				                s_touch.twoFingerStart2X,
				                s_touch.twoFingerStart2Y,
				                width, height);

			if (move1 > TWO_FINGER_SLOP_PX ||
			    move2 > TWO_FINGER_SLOP_PX) {
				s_touch.twoFingerMoved = true;
			}

			const float centerX =
				(s_touch.f1x + s_touch.f2x) * 0.5f *
				static_cast<float>(width);
			const float centerY =
				(s_touch.f1y + s_touch.f2y) * 0.5f *
				static_cast<float>(height);

			const float distance =
				touchDistancePx(s_touch.f1x, s_touch.f1y,
				                s_touch.f2x, s_touch.f2y,
				                width, height);
			const float angle =
				touchAngle(s_touch.f1x, s_touch.f1y,
				           s_touch.f2x, s_touch.f2y);

			const float distanceDelta =
				distance - s_touch.lastDistance;

			if (SDL_fabsf(distanceDelta) > TWO_FINGER_SLOP_PX) {
				s_touch.twoFingerMoved = true;
			}

			// Continuous zoom: every physical distance delta is passed
			// through directly, so faster finger separation gives faster zoom.
			const float wheel = distanceDelta * PINCH_WHEEL_SCALE;
			if (SDL_fabsf(wheel) > 0.001f) {
				sendSyntheticMouse(mouse, window,
				                   SDL_EVENT_MOUSE_WHEEL,
				                   centerX, centerY, 0, wheel);
			}
			s_touch.lastDistance = distance;

			const float angleDelta =
				normalizedAngleDelta(angle, s_touch.lastAngle);
			s_touch.rotationAccum += angleDelta;
			s_touch.lastAngle = angle;

			const float degrees =
				SDL_fabsf(s_touch.rotationAccum) *
				(180.0f / 3.14159265358979323846f);

			if (!s_touch.twoFingerRotationActive &&
			    degrees >= TWO_FINGER_ROTATION_DEGREES) {
				s_touch.twoFingerRotationActive = true;
				sendSyntheticMouse(mouse, window,
				                   SDL_EVENT_MOUSE_BUTTON_DOWN,
				                   centerX, centerY,
				                   SDL_BUTTON_MIDDLE);
			}

			if (s_touch.twoFingerRotationActive) {
				const float rotationPixels =
					angleDelta *
					(180.0f / 3.14159265358979323846f) *
					TWO_FINGER_ROTATION_SPEED;

				sendSyntheticMouse(mouse, window,
				                   SDL_EVENT_MOUSE_MOTION,
				                   s_lastSyntheticX + rotationPixels,
				                   s_lastSyntheticY);
				s_touch.twoFingerMoved = true;
			}

			return;
		}
		return;
	}

	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED:
	{
		const SDL_FingerID id = event.tfinger.fingerID;
		const bool canceled = event.type == SDL_EVENT_FINGER_CANCELED;

		if (s_touch.phase == TouchState::BUILD_PREVIEW &&
		    id == s_touch.finger1) {
			if (canceled) {
				if (s_touch.buildRotationActive) {
					sendSyntheticMouse(mouse, window,
					                   SDL_EVENT_MOUSE_BUTTON_UP,
					                   x, y, SDL_BUTTON_MIDDLE);
				}
				resetTouchState();
			} else {
				finishBuilding(mouse, window, x, y);
			}
			return;
		}

		if (s_touch.phase == TouchState::ONE_PENDING &&
		    id == s_touch.finger1) {
			if (!canceled) {
				sendTap(mouse, window, x, y);
			}
			resetTouchState();
			return;
		}

		if (s_touch.phase == TouchState::CAMERA_PAN &&
		    id == s_touch.finger1) {
			if (canceled) {
				sendSyntheticMouse(mouse, window,
				                   SDL_EVENT_MOUSE_BUTTON_UP,
				                   x, y, SDL_BUTTON_RIGHT);
				stopCameraMomentum(mouse, window);
				resetTouchState();
				return;
			}

			// Release the finger but keep the synthetic RMB held while
			// the measured velocity glides the camera to a stop.
			const Uint64 now = SDL_GetTicks();
			if (now - s_cameraLastMotionTicks <= 120) {
				s_cameraMomentumActive =
					SDL_fabsf(s_cameraVelocityX) > 20.0f ||
					SDL_fabsf(s_cameraVelocityY) > 20.0f;
			} else {
				s_cameraMomentumActive = false;
			}

			if (s_cameraMomentumActive) {
				s_cameraMomentumLastTicks = now;
			} else {
				sendSyntheticMouse(mouse, window,
				                   SDL_EVENT_MOUSE_BUTTON_UP,
				                   x, y, SDL_BUTTON_RIGHT);
			}
			resetTouchState();
			return;
		}

		if (s_touch.phase == TouchState::SELECTION &&
		    id == s_touch.finger1) {
			sendSyntheticMouse(mouse, window,
			                   SDL_EVENT_MOUSE_BUTTON_UP,
			                   x, y, SDL_BUTTON_LEFT);
			resetTouchState();
			return;
		}

		if (s_touch.phase == TouchState::TWO_FINGER) {
			if (id == s_touch.finger1) {
				s_touch.finger1 = 0;
			} else if (id == s_touch.finger2) {
				s_touch.finger2 = 0;
			} else {
				return;
			}

			if (s_touch.finger1 != 0 || s_touch.finger2 != 0) {
				return;
			}

			if (s_touch.twoFingerRotationActive) {
				sendSyntheticMouse(mouse, window,
				                   SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_lastSyntheticX,
				                   s_lastSyntheticY,
				                   SDL_BUTTON_MIDDLE);
			}

			const Uint64 held = SDL_GetTicks() - s_touch.twoFingerDownTicks;
			const bool quickStationaryTap =
				!canceled &&
				held <= TWO_FINGER_TAP_MS &&
				!s_touch.twoFingerMoved;

			if (quickStationaryTap) {
				// Two-finger tap is the universal right-click/cancel.
				sendSyntheticMouse(mouse, window,
				                   SDL_EVENT_MOUSE_BUTTON_DOWN,
				                   s_lastSyntheticX, s_lastSyntheticY,
				                   SDL_BUTTON_RIGHT);
				sendSyntheticMouse(mouse, window,
				                   SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_lastSyntheticX, s_lastSyntheticY,
				                   SDL_BUTTON_RIGHT);
			}

			resetTouchState();
			return;
		}

		resetTouchState();
		return;
	}

	default:
		return;
	}
}

void updateTouchLongPress(SDL3Mouse *mouse, SDL_Window *window)
{
	if (!mouse || !window) return;

	// Camera momentum is updated once per engine frame, not tied to touch
	// event frequency. This gives a stable, smooth smartphone-like glide.
	if (s_cameraMomentumActive) {
		const Uint64 now = SDL_GetTicks();
		Uint64 dtMs = now - s_cameraMomentumLastTicks;
		if (dtMs == 0) return;
		if (dtMs > 50) dtMs = 50;

		const float dt = static_cast<float>(dtMs) * 0.001f;
		const float dx = s_cameraVelocityX * dt;
		const float dy = s_cameraVelocityY * dt;

		sendSyntheticMouse(mouse, window,
		                   SDL_EVENT_MOUSE_MOTION,
		                   s_lastSyntheticX + dx,
		                   s_lastSyntheticY + dy);

		s_cameraVelocityX *= SDL_powf(0.0008f, dt);
		s_cameraVelocityY *= SDL_powf(0.0008f, dt);
		s_cameraMomentumLastTicks = now;

		if (SDL_sqrtf(s_cameraVelocityX * s_cameraVelocityX +
		              s_cameraVelocityY * s_cameraVelocityY) < 20.0f) {
			stopCameraMomentum(mouse, window);
		}
	}

	if (s_touch.phase != TouchState::BUILD_PREVIEW) return;
	if (s_touch.finger1 == 0) return;
	if (s_touch.buildMoved) return;
	if (s_touch.buildRotationActive) return;

	if ((SDL_GetTicks() - s_touch.downTicks) >= BUILD_ROTATION_HOLD_MS) {
		startBuildingRotation(mouse, window);
	}
}

} // anonymous namespace

#endif // TARGET_OS_IPHONE

namespace {

// Enter on an iOS soft keyboard must dismiss the IME and must not be
// immediately restarted by updateTextInputState() while the same entry field
// still owns focus. This flag is intentionally kept in this .cpp only.
static bool s_textInputDismissedForCurrentFocus = false;

Bool DecodeNextUtf8Codepoint(const char* text, size_t length, size_t& offset, UnsignedInt& outCodepoint)
{
	outCodepoint = 0;
	if (!text || offset >= length) {
		return false;
	}

	const unsigned char first = static_cast<unsigned char>(text[offset]);
	if (first == 0) {
		return false;
	}

	if (first < 0x80) {
		outCodepoint = first;
		offset += 1;
		return true;
	}

	if ((first & 0xE0) == 0xC0 && offset + 1 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		if ((second & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x1F) << 6) | (second & 0x3F);
			offset += 2;
			return true;
		}
	}

	if ((first & 0xF0) == 0xE0 && offset + 2 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
		if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F);
			offset += 3;
			return true;
		}
	}

	if ((first & 0xF8) == 0xF0 && offset + 3 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
		const unsigned char fourth = static_cast<unsigned char>(text[offset + 3]);
		if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80 && (fourth & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x07) << 18) | ((second & 0x3F) << 12) | ((third & 0x3F) << 6) | (fourth & 0x3F);
			offset += 4;
			return true;
		}
	}

	// Invalid UTF-8 sequence: skip one byte and keep processing.
	offset += 1;
	return false;
}

}

/**
 * Constructor: Initialize SDL3 game engine state
 */
SDL3GameEngine::SDL3GameEngine()
	: GameEngine(),
	  m_SDLWindow(nullptr),
	  m_IsInitialized(false),
	  m_IsActive(false),
	  m_IsTextInputActive(false),
	  m_TextInputFocusWindow(nullptr)
{
	fprintf(stderr, "DEBUG: SDL3GameEngine::SDL3GameEngine() created\n");
}

/**
 * Destructor: Cleanup SDL3 resources
 */
SDL3GameEngine::~SDL3GameEngine()
{
	if (m_SDLWindow && m_IsTextInputActive) {
		SDL_StopTextInput(m_SDLWindow);
		m_IsTextInputActive = false;
		m_TextInputFocusWindow = nullptr;
	}

	if (m_IsInitialized) {
		// Window cleanup is done in reset/shutdown
	}
	fprintf(stderr, "DEBUG: SDL3GameEngine::~SDL3GameEngine() destroyed\n");
}

/**
 * From GameEngine: init() - initialize subsystems
 * 
 * GeneralsX @bugfix felipebraz 16/02/2026
 * Simplified to follow fighter19 pattern - SDL3/Vulkan initialized in SDL3Main.cpp
 * before GameEngine is created. This init() only delegates to parent GameEngine::init().
 * ApplicationHWnd and TheSDL3Window are already set by main() before this is called.
 */
void SDL3GameEngine::init(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::init() starting\n");

	if (TheGlobalData && TheGlobalData->m_headless) {
		// GeneralsX @bugfix Copilot 17/05/2026 Allow headless replay path to initialize engine subsystems without an SDL window.
		fprintf(stderr, "INFO: SDL3GameEngine::init() headless mode - skipping SDL window binding\n");
		m_SDLWindow = nullptr;
		m_IsInitialized = true;
		m_IsActive = true;
		GameEngine::init();
		return;
	}

	// Verify window was created by SDL3Main.cpp
	extern SDL_Window* TheSDL3Window;
	extern HWND ApplicationHWnd;
	
	if (!TheSDL3Window || !ApplicationHWnd) {
		fprintf(stderr, "FATAL: SDL3 window not initialized before GameEngine::init()\n");
		fprintf(stderr, "FATAL: TheSDL3Window=%p, ApplicationHWnd=%p\n", TheSDL3Window, ApplicationHWnd);
		return;
	}

	// Store window reference locally
	m_SDLWindow = TheSDL3Window;
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// SDL3 documents this hint specifically for iOS/Android: Return hides the
	// soft keyboard instead of leaving the IME permanently visible.
	SDL_SetHint(SDL_HINT_RETURN_KEY_HIDES_IME, "1");
#endif
	m_IsInitialized = true;
	m_IsActive = true;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Lifecycle events can fire outside the poll cycle on iOS; catch them
	// immediately so rendering halts before the process is suspended.
	SDL_AddEventWatch(iosLifecycleWatcher, nullptr);
#endif

	fprintf(stderr, "INFO: SDL3GameEngine using pre-initialized window\n");

	// Call parent init to initialize game subsystems
	GameEngine::init();
}

/**
 * From GameEngine: reset() - reset system to starting state
 */
void SDL3GameEngine::reset(void)
{
	s_textInputDismissedForCurrentFocus = false;
	fprintf(stderr, "DEBUG: SDL3GameEngine::reset()\n");
	if (m_SDLWindow && m_IsTextInputActive) {
		SDL_StopTextInput(m_SDLWindow);
		m_IsTextInputActive = false;
		m_TextInputFocusWindow = nullptr;
	}
	GameEngine::reset();
}

/**
 * From GameEngine: update() - per-frame update
 */
void SDL3GameEngine::update(void)
{
	pollSDL3Events();
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Pause sim + render while backgrounded OR inactive (see iosLifecycleWatcher).
	// Acquiring a Metal drawable in these windows fights iOS for the layer and,
	// across repeated suspend/switcher cycles, crashes MoltenVK. Keep polling so
	// we still catch the resume events; just don't touch the GPU.
	if (iosShouldPauseRendering()) {
		SDL_Delay(50);
		return;
	}
#endif
	GameEngine::update();
}

/**
 * From GameEngine: execute() - main game loop
 */
void SDL3GameEngine::execute(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::execute() - entering main loop\n");
	GameEngine::execute();
	fprintf(stderr, "INFO: SDL3GameEngine::execute() - exited main loop\n");
}

/**
 * From GameEngine: serviceWindowsOS() - native OS service
 * On Linux, process SDL3 events
 */
void SDL3GameEngine::serviceWindowsOS(void)
{
	pollSDL3Events();
}

/**
 * Check if game has OS focus
 */
Bool SDL3GameEngine::isActive(void)
{
	return m_IsActive;
}

/**
 * Set OS focus status
 */
void SDL3GameEngine::setIsActive(Bool isActive)
{
	m_IsActive = isActive;
}

/**
 * Poll and process SDL3 events
 * Handles keyboard, mouse, window, and quit events
 */
void SDL3GameEngine::pollSDL3Events(void)
{
	if (!m_SDLWindow) {
		return;
	}

	updateTextInputState();

	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
			case SDL_EVENT_QUIT:
				m_quitting = true;
				break;

			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				m_quitting = true;
				break;

			case SDL_EVENT_WINDOW_FOCUS_GAINED:
				m_IsActive = true;
				if (TheMouse) {
					TheMouse->regainFocus();
					TheMouse->refreshCursorCapture();
				}
				break;

			case SDL_EVENT_WINDOW_FOCUS_LOST:
				m_IsActive = false;
				if (m_IsTextInputActive) {
					SDL_StopTextInput(m_SDLWindow);
					m_IsTextInputActive = false;
					m_TextInputFocusWindow = nullptr;
				}
				if (TheMouse) {
					TheMouse->loseFocus();
				}
				break;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
			// App suspension/resume: mirror the desktop focus handling so audio
			// and mouse state pause cleanly (the render gate lives in update()).
			case SDL_EVENT_DID_ENTER_BACKGROUND:
				m_IsActive = false;
				if (TheMouse) {
					TheMouse->loseFocus();
				}
				break;

			case SDL_EVENT_DID_ENTER_FOREGROUND:
				m_IsActive = true;
				if (TheMouse) {
					TheMouse->regainFocus();
					TheMouse->refreshCursorCapture();
				}
				break;
#endif

			case SDL_EVENT_WINDOW_MOUSE_ENTER:
				if (TheMouse) {
					TheMouse->onCursorMovedInside();
				}
				break;

			case SDL_EVENT_WINDOW_MOUSE_LEAVE:
				if (TheMouse) {
					TheMouse->onCursorMovedOutside();
				}
				break;

			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_KEY_UP:
				// Forward normal keyboard events through the existing SDL3Keyboard.
				if (TheKeyboard) {
					SDL3Keyboard* keyboard = dynamic_cast<SDL3Keyboard*>(TheKeyboard);
					if (keyboard) {
						keyboard->addSDLEvent(&event);
					}
				}

				// iOS Return/Enter: stop text input immediately. SDL3's documented
				// RETURN_KEY_HIDES_IME hint handles the native keyboard as well, while
				// this explicit stop prevents it from remaining on-screen forever.
				if (event.type == SDL_EVENT_KEY_DOWN &&
				    (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) &&
				    m_IsTextInputActive) {
					SDL_ClearComposition(m_SDLWindow);
					SDL_StopTextInput(m_SDLWindow);
					m_IsTextInputActive = false;
					s_textInputDismissedForCurrentFocus = true;
				}
				break;

			case SDL_EVENT_TEXT_INPUT:
				forwardTextInputEvent(event.text.text);
				break;

			case SDL_EVENT_MOUSE_MOTION:
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP:
			case SDL_EVENT_MOUSE_WHEEL:
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
				// Belt-and-braces: drop SDL's own touch-synthesized mouse events.
				// The gesture translator owns all touch->mouse conversion; double
				// delivery would produce phantom second clicks.
				if (event.motion.which == SDL_TOUCH_MOUSEID) {
					break;
				}
#endif
				// Fighter19 pattern: direct addSDLEvent() call with raw SDL_Event
				// GeneralsX @refactor felipebraz 16/02/2026 Simplified event routing
				if (TheMouse) {
					SDL3Mouse* mouse = dynamic_cast<SDL3Mouse*>(TheMouse);
					if (mouse) {
						mouse->addSDLEvent(&event);
					}
				}
				break;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
			case SDL_EVENT_FINGER_DOWN:
			case SDL_EVENT_FINGER_MOTION:
			case SDL_EVENT_FINGER_UP:
			case SDL_EVENT_FINGER_CANCELED:
				if (TheMouse && m_SDLWindow) {
					SDL3Mouse* mouse = dynamic_cast<SDL3Mouse*>(TheMouse);
					if (mouse) {
						handleTouchEvent(mouse, m_SDLWindow, event);
					}
				}
				break;
#endif

			case SDL_EVENT_WINDOW_RESIZED:
				handleWindowEvent(event.window);
				break;

			default:
				// Ignore other events for now
				break;
		}

		updateTextInputState();
	}

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Keep the touch state alive between frames. The exact 250 ms selection rule is
	// evaluated against SDL_GetTicks() when the next motion arrives.
	if (TheMouse && m_SDLWindow) {
		SDL3Mouse* touchMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (touchMouse) {
			updateTouchLongPress(touchMouse, m_SDLWindow);
		}
	}
#endif
}

// GeneralsX @bugfix felipebraz 01/04/2026 Enable SDL text input only while an entry gadget owns focus.
void SDL3GameEngine::updateTextInputState(void)
{
	if (!m_SDLWindow || !TheWindowManager) {
		return;
	}

	GameWindow* focusedWindow = TheWindowManager->winGetFocus();
	const Bool wantsTextInput =
		focusedWindow != nullptr && BitIsSet(focusedWindow->winGetStyle(), GWS_ENTRY_FIELD);

	if (wantsTextInput) {
		const bool sameDismissedFocus =
			s_textInputDismissedForCurrentFocus &&
			m_TextInputFocusWindow == focusedWindow;

		if (!m_IsTextInputActive && !sameDismissedFocus) {
			if (SDL_StartTextInput(m_SDLWindow)) {
				m_IsTextInputActive = true;
			}
		}
		if (m_TextInputFocusWindow != focusedWindow) {
			s_textInputDismissedForCurrentFocus = false;
		}
		m_TextInputFocusWindow = focusedWindow;
	} else {
		if (m_IsTextInputActive) {
			SDL_ClearComposition(m_SDLWindow);
			SDL_StopTextInput(m_SDLWindow);
			m_IsTextInputActive = false;
		}
		m_TextInputFocusWindow = nullptr;
		s_textInputDismissedForCurrentFocus = false;
	}
}

// GeneralsX @bugfix felipebraz 01/04/2026 Forward SDL UTF-8 text input through existing GWM_IME_CHAR path.
void SDL3GameEngine::forwardTextInputEvent(const char* utf8Text)
{
	if (!utf8Text || !TheWindowManager) {
		return;
	}

	// GeneralsX @bugfix felipebraz 01/04/2026 Use tracked text-input focus window to keep SDL text delivery stable.
	GameWindow* targetWindow = m_TextInputFocusWindow;
	if (!targetWindow || !BitIsSet(targetWindow->winGetStyle(), GWS_ENTRY_FIELD)) {
		return;
	}

	const size_t textLength = strlen(utf8Text);
	size_t offset = 0;
	while (offset < textLength) {
		UnsignedInt codepoint = 0;
		if (!DecodeNextUtf8Codepoint(utf8Text, textLength, offset, codepoint)) {
			continue;
		}

		// GeneralsX @bugfix felipebraz 01/04/2026 Clamp IME char forwarding to BMP and reject UTF-16 surrogate range.
		if (codepoint == 0 || codepoint > 0x10FFFFU) {
			continue;
		}

		if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
			continue;
		}

		if (codepoint > 0xFFFFU) {
			continue;
		}

		const WideChar wideCharacter = static_cast<WideChar>(codepoint);
		TheWindowManager->winSendInputMsg(targetWindow, GWM_IME_CHAR, static_cast<WindowMsgData>(wideCharacter), 0);
	}
}

/**
 * Handle keyboard event -dispatch to Keyboard manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleKeyboardEvent(const SDL_KeyboardEvent& event)
{
	// Dispatch to SDL3Keyboard if available
	if (TheKeyboard) {
		SDL3Keyboard* sdlKeyboard = dynamic_cast<SDL3Keyboard*>(TheKeyboard);
		if (sdlKeyboard) {
			sdlKeyboard->addSDL3KeyEvent(event);
		}
	}
}

/**
 * Handle mouse motion event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseMotionEvent(const SDL_MouseMotionEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseMotionEvent(event);
		}
	}
}

/**
 * Handle mouse button event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseButtonEvent(const SDL_MouseButtonEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseButtonEvent(event);
		}
	}
}

/**
 * Handle mouse wheel event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseWheelEvent(const SDL_MouseWheelEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseWheelEvent(event);
		}
	}
}

/**
 * Handle window event (resize, etc.)
 */
void SDL3GameEngine::handleWindowEvent(const SDL_WindowEvent& event)
{
	// TODO: Phase 2 - Handle window resize, notify graphics subsystem
	// fprintf(stderr, "DEBUG: Window event (type=%d)\n", event.type);
}

/**
 * Factory Methods for GameEngine subsystems
 * TheSuperHackers @build felipebraz 13/02/2026
 * Implementations in .cpp to provide complete type definitions and avoid circular includes
 */

LocalFileSystem *SDL3GameEngine::createLocalFileSystem(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createLocalFileSystem() -> StdLocalFileSystem\n");
	return NEW StdLocalFileSystem;
}

ArchiveFileSystem *SDL3GameEngine::createArchiveFileSystem(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createArchiveFileSystem() -> StdBIGFileSystem\n");
	return NEW StdBIGFileSystem;
}

GameLogic *SDL3GameEngine::createGameLogic(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createGameLogic() -> W3DGameLogic\n");
	return NEW W3DGameLogic;
}

GameClient *SDL3GameEngine::createGameClient(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createGameClient() -> W3DGameClient\n");
	return NEW W3DGameClient;
}

ModuleFactory *SDL3GameEngine::createModuleFactory(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createModuleFactory() -> W3DModuleFactory\n");
	return NEW W3DModuleFactory;
}

ThingFactory *SDL3GameEngine::createThingFactory(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createThingFactory() -> W3DThingFactory\n");
	return NEW W3DThingFactory;
}

FunctionLexicon *SDL3GameEngine::createFunctionLexicon(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createFunctionLexicon() -> W3DFunctionLexicon\n");
	return NEW W3DFunctionLexicon;
}

// GeneralsX @bugfix Copilot 15/04/2026 Match upstream GameEngine pure-virtual signature after sync.
Radar *SDL3GameEngine::createRadar(Bool dummy)
{
	// GeneralsX @bugfix fbraz 04/05/2026 Respect headless mode and create dummy radar.
	// Upstream reference: Win32GameEngine headless factory behavior, TheSuperHackers/GeneralsGameCode
	// https://github.com/TheSuperHackers/GeneralsGameCode
	if (dummy) {
		fprintf(stderr, "INFO: SDL3GameEngine::createRadar() -> RadarDummy (headless)\n");
		return NEW RadarDummy;
	}
	fprintf(stderr, "INFO: SDL3GameEngine::createRadar() -> W3DRadar\n");
	return NEW W3DRadar;
}

// GeneralsX @bugfix Copilot 24/03/2026 Match upstream GameEngine pure-virtual signature after sync.
ParticleSystemManager* SDL3GameEngine::createParticleSystemManager(Bool dummy)
{
	// GeneralsX @bugfix fbraz 04/05/2026 Respect headless mode and create dummy particle manager.
	if (dummy) {
		fprintf(stderr, "INFO: SDL3GameEngine::createParticleSystemManager() -> ParticleSystemManagerDummy (headless)\n");
		return NEW ParticleSystemManagerDummy;
	}
	fprintf(stderr, "INFO: SDL3GameEngine::createParticleSystemManager() -> W3DParticleSystemManager\n");
	return NEW W3DParticleSystemManager;
}

WebBrowser *SDL3GameEngine::createWebBrowser(void)
{
	// WebBrowser uses Windows COM (CComObject<W3DWebBrowser>)
	// Not available on Linux - return nullptr
	fprintf(stderr, "WARNING: WebBrowser not available on Linux platform\n");
	return nullptr;
}

/**
 * Factory method: AudioManager
 * Select audio backend based on compile flags
 * GeneralsX @bugfix Copilot 15/04/2026 Match upstream GameEngine pure-virtual signature after sync.
 */
AudioManager *SDL3GameEngine::createAudioManager(Bool dummy)
{
	(void)dummy;
	fprintf(stderr, "INFO: SDL3GameEngine::createAudioManager()\n");

#ifdef SAGE_USE_OPENAL
	fprintf(stderr, "INFO: Creating OpenAL audio backend\n");
	return new OpenALAudioManager();
#else
	fprintf(stderr, "INFO: Audio backend not available (SAGE_USE_OPENAL not defined)\n");
	fprintf(stderr, "WARNING: Falls back to parent implementation or silent mode\n");
	return GameEngine::createAudioManager();  // Call parent (may return stub)
#endif
}

#endif // !_WIN32
