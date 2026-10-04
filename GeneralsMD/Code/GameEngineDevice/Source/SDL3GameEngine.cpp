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
#include "GameClient/InGameUI.h"
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
//
// Two independent reasons to halt the render/sim loop on iOS:
//  - BACKGROUNDED (home / switched away): the process is about to be suspended.
//  - INACTIVE (multitasking switcher open, Control Center, a notification
//    banner): iOS snapshots the window and owns the CAMetalLayer drawable during
//    this window — and crucially, opening the app switcher fires resign-active
//    WITHOUT a full background transition.
// ---------------------------------------------------------------------------
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
// Строго один файл. Только SDL3Mouse::addSDLEvent().
// Никакой инерции. Отпустил палец — движение остановилось мгновенно.
//
// Один палец:
//   тап без движения                -> LMB click (двойной тап = clicks=2)
//   движение до 250 мс              -> RMB drag, карта едет за пальцем
//   250 мс покоя, потом движение    -> LMB drag, рамка выделения
//   250 мс покоя, потом отпускание  -> LMB click в точке удержания
//
// Режим стройки (TheInGameUI->getPendingPlaceType() != null):
//   движение сразу                  -> превью за пальцем, xrel=yrel=0
//   отпустил после движения         -> превью фиксируется, стройки нет
//   тап < 200 мс                    -> LMB click, стройка начинается
//   200 мс покоя                    -> MMB down, вращение здания 360°
//   отпустил после вращения         -> MMB up + LMB click, авто-стройка
//
// Два пальца:
//   pinch                           -> wheel (зум), плавно, по скорости
//   twist после 25°                 -> MMB drag, поворот камеры
//   короткий синхронный шлепок      -> RMB click (отмена/сброс)
// ---------------------------------------------------------------------------
namespace {

struct TouchState {
	enum Phase {
		Idle,
		OnePending,      // палец 1, жест ещё не классифицирован
		CameraPan,       // RMB drag, карта едет за пальцем
		Selection,       // LMB down, рамка выделения
		BuildPending,    // превью зафиксировано / палец на нём
		BuildMoving,     // превью едет за пальцем
		BuildRotate,     // MMB, вращение 360°
		TwoFinger
	};

	Phase phase = Idle;
	SDL_FingerID finger1 = 0;
	SDL_FingerID finger2 = 0;

	float downX = 0.0f, downY = 0.0f;
	float lastX = 0.0f, lastY = 0.0f;
	float f1x = 0.0f, f1y = 0.0f;
	float f2x = 0.0f, f2y = 0.0f;

	Uint64 downTicks = 0;

	Uint64 lastTapTicks = 0;
	float  lastTapX = 0.0f, lastTapY = 0.0f;
	bool   haveLastTap = false;

	bool firstFingerMoved = false;

	float pinchCurrentDistance = 0.0f;
	float pinchCurrentAngle = 0.0f;
	bool  pinchMoved = false;
	bool  rotationArmed = false;
	float rotationAccum = 0.0f;
	Uint64 twoFingerDownTicks = 0;
};

TouchState s_touch;

float s_synthX = 0.0f;
float s_synthY = 0.0f;
bool  s_haveSynth = false;

// Виртуальный курсор панорамы. Двигаем в противоположную сторону от
// пальца, чтобы игра развернула RMB drag и карта поехала за пальцем.
float s_camX = 0.0f;
float s_camY = 0.0f;

constexpr Uint64 kSelectionHoldMs   = 250;
constexpr Uint64 kBuildRotateHoldMs = 200;
constexpr Uint64 kDoubleTapMs       = 300;
constexpr Uint64 kTwoFingerTapMs    = 300;

constexpr float kMoveDeadzonePx     = 5.0f;
constexpr float kDoubleTapDistPx    = 40.0f;
constexpr float kTwoFingerTapMaxPx  = 20.0f;
constexpr float kPinchPixelsPerTick = 40.0f;   // 40 px пинча = 1 тик колеса
constexpr float kRotateThresholdDeg = 25.0f;   // порог включения поворота
constexpr float kRotatePixelsPerRad = 250.0f;  // 1 рад twist = 250 px MMB
constexpr float kPi = 3.14159265358979323846f;

// Точная проверка режима стройки. getPendingPlaceType() возвращает
// nullptr, когда ничего не строится. Это надёжнее getMouseCursor(),
// который игра обновляет не мгновенно.
static bool isBuildingPlacementMode()
{
	if (TheInGameUI && TheInGameUI->getPendingPlaceType() != nullptr) return true;
	if (TheMouse) {
		const Mouse::MouseCursor c = TheMouse->getMouseCursor();
		if (c == Mouse::BUILD_PLACEMENT || c == Mouse::INVALID_BUILD_PLACEMENT) return true;
	}
	return false;
}

static float touchDistance(float x1, float y1, float x2, float y2)
{
	const float dx = x1 - x2, dy = y1 - y2;
	return SDL_sqrtf(dx*dx + dy*dy);
}

static float touchAngle(float x1, float y1, float x2, float y2)
{
	return SDL_atan2f(y2 - y1, x2 - x1);
}

static float normalizedAngleDelta(float current, float previous)
{
	float d = current - previous;
	while (d >  kPi) d -= 2.0f * kPi;
	while (d < -kPi) d += 2.0f * kPi;
	return d;
}

// Явная отправка события с полным контролем над дельтой.
static void sendMouseExplicit(SDL3Mouse *mouse, SDL_Window *window,
                              Uint32 type, float x, float y,
                              float xrel, float yrel,
                              Uint8 button = 0, float wheelY = 0.0f, Uint8 clicks = 1)
{
	if (!mouse || !window) return;
	SDL_Event event; SDL_zero(event); event.type = type;
	const SDL_WindowID wid = SDL_GetWindowID(window);
	switch (type) {
	case SDL_EVENT_MOUSE_MOTION:
		event.motion.windowID = wid;
		event.motion.which    = 0;
		event.motion.x        = x;
		event.motion.y        = y;
		event.motion.xrel     = xrel;
		event.motion.yrel     = yrel;
		break;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
		event.button.windowID = wid;
		event.button.which    = 0;
		event.button.button   = button;
		event.button.down     = (type == SDL_EVENT_MOUSE_BUTTON_DOWN);
		event.button.clicks   = clicks;
		event.button.x        = x;
		event.button.y        = y;
		break;
	case SDL_EVENT_MOUSE_WHEEL:
		event.wheel.windowID = wid;
		event.wheel.which    = 0;
		event.wheel.x        = 0.0f;
		event.wheel.y        = wheelY;
		event.wheel.mouse_x  = x;
		event.wheel.mouse_y  = y;
		break;
	default: return;
	}
	mouse->addSDLEvent(&event);
}

// Авто-дельта от прошлой отправленной позиции.
static void sendMotion(SDL3Mouse *mouse, SDL_Window *window, float x, float y)
{
	const float dx = s_haveSynth ? (x - s_synthX) : 0.0f;
	const float dy = s_haveSynth ? (y - s_synthY) : 0.0f;
	s_synthX = x; s_synthY = y; s_haveSynth = true;
	sendMouseExplicit(mouse, window, SDL_EVENT_MOUSE_MOTION, x, y, dx, dy);
}

// Позиционирование курсора без движения камеры.
static void sendMotionNoDelta(SDL3Mouse *mouse, SDL_Window *window, float x, float y)
{
	s_synthX = x; s_synthY = y; s_haveSynth = true;
	sendMouseExplicit(mouse, window, SDL_EVENT_MOUSE_MOTION, x, y, 0.0f, 0.0f);
}

static void sendBtnDown(SDL3Mouse *mouse, SDL_Window *window, float x, float y, Uint8 btn, Uint8 clicks = 1)
{
	sendMouseExplicit(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, 0.0f, 0.0f, btn, 0.0f, clicks);
}

static void sendBtnUp(SDL3Mouse *mouse, SDL_Window *window, float x, float y, Uint8 btn, Uint8 clicks = 1)
{
	sendMouseExplicit(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, x, y, 0.0f, 0.0f, btn, 0.0f, clicks);
}

static void sendWheel(SDL3Mouse *mouse, SDL_Window *window, float x, float y, float wheelY)
{
	sendMouseExplicit(mouse, window, SDL_EVENT_MOUSE_WHEEL, x, y, 0.0f, 0.0f, 0, wheelY);
}

static void resetTouchState()
{
	const Uint64 ltt = s_touch.lastTapTicks;
	const float  ltx = s_touch.lastTapX, lty = s_touch.lastTapY;
	const bool   hlt = s_touch.haveLastTap;
	s_touch = TouchState{};
	s_touch.lastTapTicks = ltt;
	s_touch.lastTapX = ltx; s_touch.lastTapY = lty;
	s_touch.haveLastTap = hlt;
	s_haveSynth = false;
}

static void emitTap(SDL3Mouse *mouse, SDL_Window *window, float x, float y)
{
	const Uint64 now = SDL_GetTicks();
	const float dx = x - s_touch.lastTapX, dy = y - s_touch.lastTapY;
	const bool isDouble = s_touch.haveLastTap &&
	                      (now - s_touch.lastTapTicks) <= kDoubleTapMs &&
	                      SDL_sqrtf(dx*dx + dy*dy) <= kDoubleTapDistPx;
	const Uint8 clicks = isDouble ? 2 : 1;

	sendMotionNoDelta(mouse, window, x, y);
	sendBtnDown(mouse, window, x, y, SDL_BUTTON_LEFT, clicks);
	sendBtnUp  (mouse, window, x, y, SDL_BUTTON_LEFT, clicks);

	if (isDouble) { s_touch.haveLastTap = false; }
	else {
		s_touch.lastTapTicks = now;
		s_touch.lastTapX = x; s_touch.lastTapY = y;
		s_touch.haveLastTap = true;
	}
}

static void startCameraPan(SDL3Mouse *mouse, SDL_Window *window, float x, float y)
{
	s_touch.phase = TouchState::CameraPan;
	s_touch.lastX = x; s_touch.lastY = y;
	s_camX = x; s_camY = y;
	sendMotionNoDelta(mouse, window, x, y);
	sendBtnDown(mouse, window, x, y, SDL_BUTTON_RIGHT);
}

static void startSelection(SDL3Mouse *mouse, SDL_Window *window, float x, float y)
{
	s_touch.phase = TouchState::Selection;
	s_touch.lastX = x; s_touch.lastY = y;
	sendMotionNoDelta(mouse, window, x, y);
	sendBtnDown(mouse, window, x, y, SDL_BUTTON_LEFT);
}

static void releaseAllButtons(SDL3Mouse *mouse, SDL_Window *window)
{
	switch (s_touch.phase) {
	case TouchState::CameraPan:
		sendBtnUp(mouse, window, s_camX, s_camY, SDL_BUTTON_RIGHT);
		break;
	case TouchState::Selection:
		sendBtnUp(mouse, window, s_touch.lastX, s_touch.lastY, SDL_BUTTON_LEFT);
		break;
	case TouchState::BuildRotate:
		sendBtnUp(mouse, window, s_touch.lastX, s_touch.lastY, SDL_BUTTON_MIDDLE);
		break;
	default: break;
	}
}

static void startBuildRotation(SDL3Mouse *mouse, SDL_Window *window)
{
	if (s_touch.phase != TouchState::BuildPending) return;
	if (s_touch.firstFingerMoved) return;

	s_touch.phase = TouchState::BuildRotate;
	s_touch.lastX = s_touch.downX;
	s_touch.lastY = s_touch.downY;
	sendMotionNoDelta(mouse, window, s_touch.lastX, s_touch.lastY);
	sendBtnDown(mouse, window, s_touch.lastX, s_touch.lastY, SDL_BUTTON_MIDDLE);
}

// Пофреймовая проверка удержания 200 мс для вращения здания.
// Палец, стоящий на месте, не генерирует SDL-события — без этого
// вызова вращение никогда не включится.
static void updateTouchHold(SDL3Mouse *mouse, SDL_Window *window)
{
	if (!mouse || !window) return;
	if (s_touch.phase != TouchState::BuildPending) return;
	if (s_touch.finger1 == 0) return;
	if (s_touch.firstFingerMoved) return;
	if ((SDL_GetTicks() - s_touch.downTicks) < kBuildRotateHoldMs) return;
	startBuildRotation(mouse, window);
}

static void handleTouchEvent(SDL3Mouse *mouse, SDL_Window *window, const SDL_Event &event)
{
	if (!mouse || !window) return;

	int width = 0, height = 0;
	SDL_GetWindowSize(window, &width, &height);
	if (width <= 0 || height <= 0) return;

	const float x = event.tfinger.x * static_cast<float>(width);
	const float y = event.tfinger.y * static_cast<float>(height);

	// Отмена: отпускаем кнопки, никаких кликов, фиксированное превью не теряем.
	if (event.type == SDL_EVENT_FINGER_CANCELED) {
		const bool keepBuildPending = (s_touch.phase == TouchState::BuildPending ||
		                               s_touch.phase == TouchState::BuildMoving);
		releaseAllButtons(mouse, window);
		resetTouchState();
		if (keepBuildPending) {
			s_touch.phase = TouchState::BuildPending;
		}
		return;
	}

	switch (event.type) {

	// ================= КАСАНИЕ =================
	case SDL_EVENT_FINGER_DOWN:
	{
		const SDL_FingerID id = event.tfinger.fingerID;

		// Первое касание — определяем режим.
		if (s_touch.phase == TouchState::Idle) {
			s_touch.finger1 = id;
			s_touch.downX = s_touch.lastX = x;
			s_touch.downY = s_touch.lastY = y;
			s_touch.f1x = event.tfinger.x;
			s_touch.f1y = event.tfinger.y;
			s_touch.downTicks = SDL_GetTicks();
			s_touch.firstFingerMoved = false;
			s_touch.phase = isBuildingPlacementMode()
				? TouchState::BuildPending
				: TouchState::OnePending;
			sendMotionNoDelta(mouse, window, x, y);
			return;
		}

		// Превью уже зафиксировано, палец отпущен. Следующее касание —
		// по превью: тап → стройка, удержание 200 мс → вращение.
		if (s_touch.phase == TouchState::BuildPending && s_touch.finger1 == 0) {
			s_touch.finger1 = id;
			s_touch.downX = s_touch.lastX = x;
			s_touch.downY = s_touch.lastY = y;
			s_touch.f1x = event.tfinger.x;
			s_touch.f1y = event.tfinger.y;
			s_touch.downTicks = SDL_GetTicks();
			s_touch.firstFingerMoved = false;
			sendMotionNoDelta(mouse, window, x, y);
			return;
		}

		// Второй палец — двухпальцевый жест.
		if (s_touch.finger1 != 0 && id != s_touch.finger1 && s_touch.finger2 == 0) {
			releaseAllButtons(mouse, window);

			s_touch.finger2 = id;
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
			s_touch.twoFingerDownTicks = SDL_GetTicks();
			s_touch.pinchCurrentDistance =
				touchDistance(s_touch.f1x, s_touch.f1y, s_touch.f2x, s_touch.f2y);
			s_touch.pinchCurrentAngle =
				touchAngle(s_touch.f1x, s_touch.f1y, s_touch.f2x, s_touch.f2y);
			s_touch.pinchMoved = false;
			s_touch.rotationArmed = false;
			s_touch.rotationAccum = 0.0f;
			s_touch.phase = TouchState::TwoFinger;
			return;
		}
		return;
	}

	// ================= ДВИЖЕНИЕ =================
	case SDL_EVENT_FINGER_MOTION:
	{
		const SDL_FingerID id = event.tfinger.fingerID;

		if (id == s_touch.finger1) { s_touch.f1x = event.tfinger.x; s_touch.f1y = event.tfinger.y; }
		else if (id == s_touch.finger2) { s_touch.f2x = event.tfinger.x; s_touch.f2y = event.tfinger.y; }
		else return;

		// --- Два пальца: зум и поворот ---
		if (s_touch.phase == TouchState::TwoFinger) {
			const float dist  = touchDistance(s_touch.f1x, s_touch.f1y, s_touch.f2x, s_touch.f2y);
			const float angle = touchAngle(s_touch.f1x, s_touch.f1y, s_touch.f2x, s_touch.f2y);
			const float distDelta  = dist - s_touch.pinchCurrentDistance;
			const float angleDelta = normalizedAngleDelta(angle, s_touch.pinchCurrentAngle);
			s_touch.pinchCurrentDistance = dist;
			s_touch.pinchCurrentAngle    = angle;

			const float cx = (s_touch.f1x + s_touch.f2x) * 0.5f * (float)width;
			const float cy = (s_touch.f1y + s_touch.f2y) * 0.5f * (float)height;

			// Зум: пиксели / пиксели-на-тик. Плавно, по скорости пальцев.
			if (SDL_fabsf(distDelta) > 0.5f) {
				s_touch.pinchMoved = true;
				const float wheelY = distDelta / kPinchPixelsPerTick;
				sendWheel(mouse, window, cx, cy, wheelY);
			}

			// Поворот включается только после порога.
			if (!s_touch.rotationArmed) {
				s_touch.rotationAccum += angleDelta;
				const float deg = SDL_fabsf(s_touch.rotationAccum) * (180.0f / kPi);
				if (deg >= kRotateThresholdDeg) {
					s_touch.rotationArmed = true;
					s_touch.pinchMoved = true;
					sendMotionNoDelta(mouse, window, cx, cy);
					sendBtnDown(mouse, window, cx, cy, SDL_BUTTON_MIDDLE);
				}
			} else if (SDL_fabsf(angleDelta) > 0.0001f) {
				// Скорость поворота камеры линейна от скорости кручения.
				const float shift = angleDelta * kRotatePixelsPerRad;
				const float newX = s_synthX + shift;
				sendMotion(mouse, window, newX, s_synthY);
			}
			return;
		}

		if (id != s_touch.finger1) return;

		// Отсев дубликатов motion.
		if (SDL_fabsf(x - s_touch.lastX) < 0.5f &&
		    SDL_fabsf(y - s_touch.lastY) < 0.5f) return;

		// --- Режим стройки: превью едет за пальцем ---
		if (s_touch.phase == TouchState::BuildPending) {
			const float dx = x - s_touch.downX, dy = y - s_touch.downY;
			if (SDL_sqrtf(dx*dx + dy*dy) < kMoveDeadzonePx) return;
			s_touch.firstFingerMoved = true;
			s_touch.phase = TouchState::BuildMoving;
			sendMotionNoDelta(mouse, window, x, y);
			s_touch.lastX = x; s_touch.lastY = y;
			return;
		}
		if (s_touch.phase == TouchState::BuildMoving) {
			sendMotionNoDelta(mouse, window, x, y);
			s_touch.lastX = x; s_touch.lastY = y;
			return;
		}
		if (s_touch.phase == TouchState::BuildRotate) {
			// MMB drag: реальная дельта считается из s_synthX/Y.
			sendMotion(mouse, window, x, y);
			s_touch.lastX = x; s_touch.lastY = y;
			return;
		}

		// --- Один палец: классификация ---
		if (s_touch.phase == TouchState::OnePending) {
			const float dx = x - s_touch.downX, dy = y - s_touch.downY;
			if (SDL_sqrtf(dx*dx + dy*dy) < kMoveDeadzonePx) return;

			// Перепроверка: игра могла войти в режим стройки прямо сейчас.
			if (isBuildingPlacementMode()) {
				s_touch.firstFingerMoved = true;
				s_touch.phase = TouchState::BuildMoving;
				sendMotionNoDelta(mouse, window, x, y);
				s_touch.lastX = x; s_touch.lastY = y;
				return;
			}

			const Uint64 held = SDL_GetTicks() - s_touch.downTicks;
			s_touch.firstFingerMoved = true;
			if (held >= kSelectionHoldMs) {
				startSelection(mouse, window, s_touch.downX, s_touch.downY);
			} else {
				startCameraPan(mouse, window, s_touch.downX, s_touch.downY);
			}
			s_touch.lastX = x; s_touch.lastY = y;
			sendMotion(mouse, window, x, y);
			return;
		}

		// --- Панорама камеры. Палец вправо → курсор влево → карта за пальцем. ---
		if (s_touch.phase == TouchState::CameraPan) {
			const float dx = x - s_touch.lastX;
			const float dy = y - s_touch.lastY;
			s_camX -= dx;
			s_camY -= dy;
			s_synthX = s_camX; s_synthY = s_camY; s_haveSynth = true;
			sendMouseExplicit(mouse, window, SDL_EVENT_MOUSE_MOTION,
			                  s_camX, s_camY, -dx, -dy);
			s_touch.lastX = x; s_touch.lastY = y;
			return;
		}

		// --- Рамка выделения ---
		if (s_touch.phase == TouchState::Selection) {
			sendMotion(mouse, window, x, y);
			s_touch.lastX = x; s_touch.lastY = y;
			return;
		}
		return;
	}

	// ================= ОТПУСКАНИЕ =================
	case SDL_EVENT_FINGER_UP:
	{
		const SDL_FingerID id = event.tfinger.fingerID;

		// --- Два пальца ---
		if (s_touch.phase == TouchState::TwoFinger) {
			if (id == s_touch.finger1) s_touch.finger1 = 0;
			else if (id == s_touch.finger2) s_touch.finger2 = 0;
			else return;
			if (s_touch.finger1 != 0 || s_touch.finger2 != 0) return;

			if (s_touch.rotationArmed) {
				sendBtnUp(mouse, window, s_synthX, s_synthY, SDL_BUTTON_MIDDLE);
			}

			const Uint64 held = SDL_GetTicks() - s_touch.twoFingerDownTicks;
			const bool isTap = (held <= kTwoFingerTapMs) &&
			                   !s_touch.pinchMoved &&
			                   !s_touch.rotationArmed &&
			                   !s_touch.firstFingerMoved;
			if (isTap) {
				sendMotionNoDelta(mouse, window, s_synthX, s_synthY);
				sendBtnDown(mouse, window, s_synthX, s_synthY, SDL_BUTTON_RIGHT);
				sendBtnUp  (mouse, window, s_synthX, s_synthY, SDL_BUTTON_RIGHT);
			}
			resetTouchState();
			return;
		}

		if (id != s_touch.finger1) return;

		// --- Режим стройки: конец вращения = авто-стройка ---
		if (s_touch.phase == TouchState::BuildRotate) {
			sendBtnUp(mouse, window, x, y, SDL_BUTTON_MIDDLE);
			sendMotionNoDelta(mouse, window, x, y);
			sendBtnDown(mouse, window, x, y, SDL_BUTTON_LEFT);
			sendBtnUp  (mouse, window, x, y, SDL_BUTTON_LEFT);
			resetTouchState();
			return;
		}

		// --- Режим стройки: превью ехало, отпустили — фиксация ---
		if (s_touch.phase == TouchState::BuildMoving) {
			s_touch.finger1 = 0;
			s_touch.firstFingerMoved = false;
			s_touch.phase = TouchState::BuildPending;
			return;
		}

		// --- Режим стройки: тап по превью = стройка ---
		if (s_touch.phase == TouchState::BuildPending) {
			const Uint64 held = SDL_GetTicks() - s_touch.downTicks;
			if (held < kBuildRotateHoldMs && !s_touch.firstFingerMoved) {
				sendMotionNoDelta(mouse, window, x, y);
				sendBtnDown(mouse, window, x, y, SDL_BUTTON_LEFT);
				sendBtnUp  (mouse, window, x, y, SDL_BUTTON_LEFT);
				resetTouchState();
			} else {
				// Палец ушёл раньше 200 мс, но уже без движения — превью
				// остаётся на месте, готово к следующему касанию.
				s_touch.finger1 = 0;
				s_touch.firstFingerMoved = false;
			}
			return;
		}

		// --- Панорама камеры ---
		if (s_touch.phase == TouchState::CameraPan) {
			sendBtnUp(mouse, window, s_camX, s_camY, SDL_BUTTON_RIGHT);
			resetTouchState();
			return;
		}

		// --- Рамка выделения ---
		if (s_touch.phase == TouchState::Selection) {
			sendBtnUp(mouse, window, x, y, SDL_BUTTON_LEFT);
			resetTouchState();
			return;
		}

		// --- Один палец, тап без движения ---
		if (s_touch.phase == TouchState::OnePending) {
			if (!s_touch.firstFingerMoved) {
				emitTap(mouse, window, s_touch.downX, s_touch.downY);
			}
			resetTouchState();
			return;
		}
		return;
	}

	default: return;
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
 */
void SDL3GameEngine::init(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::init() starting\n");

	if (TheGlobalData && TheGlobalData->m_headless) {
		fprintf(stderr, "INFO: SDL3GameEngine::init() headless mode - skipping SDL window binding\n");
		m_SDLWindow = nullptr;
		m_IsInitialized = true;
		m_IsActive = true;
		GameEngine::init();
		return;
	}

	extern SDL_Window* TheSDL3Window;
	extern HWND ApplicationHWnd;

	if (!TheSDL3Window || !ApplicationHWnd) {
		fprintf(stderr, "FATAL: SDL3 window not initialized before GameEngine::init()\n");
		fprintf(stderr, "FATAL: TheSDL3Window=%p, ApplicationHWnd=%p\n", TheSDL3Window, ApplicationHWnd);
		return;
	}

	m_SDLWindow = TheSDL3Window;
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	SDL_SetHint(SDL_HINT_RETURN_KEY_HIDES_IME, "1");
#endif
	m_IsInitialized = true;
	m_IsActive = true;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	SDL_AddEventWatch(iosLifecycleWatcher, nullptr);
#endif

	fprintf(stderr, "INFO: SDL3GameEngine using pre-initialized window\n");

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
				if (TheKeyboard) {
					SDL3Keyboard* keyboard = dynamic_cast<SDL3Keyboard*>(TheKeyboard);
					if (keyboard) {
						keyboard->addSDLEvent(&event);
					}
				}

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
				if (event.motion.which == SDL_TOUCH_MOUSEID) {
					break;
				}
#endif
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
				break;
		}

		updateTextInputState();
	}

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Пофреймовая проверка удержания 200 мс для вращения здания.
	// Палец, стоящий на месте, не генерирует SDL-события — без этого
	// вызова вращение никогда не включится.
	if (TheMouse && m_SDLWindow) {
		SDL3Mouse* touchMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (touchMouse) {
			updateTouchHold(touchMouse, m_SDLWindow);
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
 * Handle keyboard event - dispatch to Keyboard manager
 */
void SDL3GameEngine::handleKeyboardEvent(const SDL_KeyboardEvent& event)
{
	if (TheKeyboard) {
		SDL3Keyboard* sdlKeyboard = dynamic_cast<SDL3Keyboard*>(TheKeyboard);
		if (sdlKeyboard) {
			sdlKeyboard->addSDL3KeyEvent(event);
		}
	}
}

/**
 * Handle mouse motion event - dispatch to Mouse manager
 */
void SDL3GameEngine::handleMouseMotionEvent(const SDL_MouseMotionEvent& event)
{
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseMotionEvent(event);
		}
	}
}

/**
 * Handle mouse button event - dispatch to Mouse manager
 */
void SDL3GameEngine::handleMouseButtonEvent(const SDL_MouseButtonEvent& event)
{
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseButtonEvent(event);
		}
	}
}

/**
 * Handle mouse wheel event - dispatch to Mouse manager
 */
void SDL3GameEngine::handleMouseWheelEvent(const SDL_MouseWheelEvent& event)
{
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
}

/**
 * Factory Methods for GameEngine subsystems
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

Radar *SDL3GameEngine::createRadar(Bool dummy)
{
	if (dummy) {
		fprintf(stderr, "INFO: SDL3GameEngine::createRadar() -> RadarDummy (headless)\n");
		return NEW RadarDummy;
	}
	fprintf(stderr, "INFO: SDL3GameEngine::createRadar() -> W3DRadar\n");
	return NEW W3DRadar;
}

ParticleSystemManager* SDL3GameEngine::createParticleSystemManager(Bool dummy)
{
	if (dummy) {
		fprintf(stderr, "INFO: SDL3GameEngine::createParticleSystemManager() -> ParticleSystemManagerDummy (headless)\n");
		return NEW ParticleSystemManagerDummy;
	}
	fprintf(stderr, "INFO: SDL3GameEngine::createParticleSystemManager() -> W3DParticleSystemManager\n");
	return NEW W3DParticleSystemManager;
}

WebBrowser *SDL3GameEngine::createWebBrowser(void)
{
	fprintf(stderr, "WARNING: WebBrowser not available on Linux platform\n");
	return nullptr;
}

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
	return GameEngine::createAudioManager();
#endif
}

#endif // !_WIN32
