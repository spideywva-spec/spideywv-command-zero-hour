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
// Строго один файл: SDL3GameEngine.cpp.
// Никаких дополнительных инклудов. Никаких TouchInput / GXTrace / AndroidTextEditor.
// Всё идёт через SDL3Mouse::addSDLEvent() как обычные mouse-события.
//
// Один палец:
//   тап без движения                -> LMB click (двойной тап = clicks=2)
//   движение до 250 мс              -> RMB drag, камера за пальцем (без инерции)
//   250 мс покоя, потом движение    -> LMB drag, рамка выделения
//   250 мс покоя, потом отпускание  -> LMB click в точке удержания
//
// Режим стройки (курсор = BUILD_PLACEMENT / INVALID_BUILD_PLACEMENT):
//   движение сразу                  -> превью за пальцем, xrel=yrel=0
//   отпустил после движения         -> превью фиксируется, стройки нет
//   тап < 200 мс                    -> LMB click, стройка начинается
//   200 мс покоя                    -> MMB down, вращение здания
//   отпустил после вращения         -> MMB up + LMB click, авто-стройка
//
// Два пальца:
//   pinch                           -> wheel (зум), плавно, по скорости
//   twist после 40°                 -> MMB drag, поворот камеры
//   короткий синхронный шлепок      -> RMB click (отмена)
//   движение без 40°                -> только зум, кнопок нет
//
// ИНЕРЦИИ НЕТ НИГДЕ. Отпустил палец — движение прекратилось мгновенно.
// ---------------------------------------------------------------------------
namespace {

struct TouchState {
	enum Phase {
		Idle,
		OnePending,      // палец 1 нажат, жест ещё не классифицирован
		CameraPan,       // RMB drag, камера за пальцем
		Selection,       // LMB down, рамка выделения
		BuildPending,    // режим стройки: палец нажат, ждём тап / drag / удержание
		BuildMoving,     // режим стройки: превью едет за пальцем
		BuildRotate,     // режим стройки: MMB зажат, вращение 360°
		TwoFinger
	};

	Phase phase = Idle;

	SDL_FingerID finger1 = 0;
	SDL_FingerID finger2 = 0;

	float downX = 0.0f, downY = 0.0f;   // точка касания finger1
	float lastX = 0.0f, lastY = 0.0f;   // последняя позиция finger1
	float f1x = 0.0f, f1y = 0.0f;       // finger1 в нормализованных координатах
	float f2x = 0.0f, f2y = 0.0f;       // finger2 в нормализованных координатах

	Uint64 downTicks = 0;

	// Двойной тап
	Uint64 lastTapTicks = 0;
	float  lastTapX = 0.0f, lastTapY = 0.0f;
	bool   haveLastTap = false;

	// Признак, что первый палец реально двигался (нужен, чтобы короткий
	// двухпальцевый финал после панорамы не сработал как ПКМ).
	bool firstFingerMoved = false;

	// Двухпальцевый жест
	float pinchCurrentDistance = 0.0f;
	float pinchCurrentAngle = 0.0f;
	bool  pinchMoved = false;
	bool  rotationArmed = false;
	float rotationAccum = 0.0f;
	Uint64 twoFingerDownTicks = 0;
};

TouchState s_touch;

// Синтетическая позиция курсора (для корректных xrel/yrel).
float s_lastSyntheticX = 0.0f;
float s_lastSyntheticY = 0.0f;
bool  s_haveSyntheticPos = false;

constexpr Uint64 kSelectionHoldMs   = 250;   // удержание без движения -> рамка
constexpr Uint64 kBuildRotateHoldMs = 200;   // удержание без движения -> вращение здания
constexpr Uint64 kDoubleTapMs       = 300;   // окно двойного тапа
constexpr Uint64 kTwoFingerTapMs    = 300;   // максимальная длительность двухпальцевого шлепка

constexpr float kMoveDeadzonePx     = 5.0f;  // дрожание пальца до старта жеста
constexpr float kDoubleTapDistPx    = 40.0f; // максимальное смещение между двумя тапами
constexpr float kTwoFingerTapMaxPx  = 20.0f; // максимальное суммарное движение для шлепка
constexpr float kPinchWheelScale    = 0.02f; // пиксели пинча -> единицы колеса
constexpr float kRotateThresholdDeg = 40.0f; // мёртвая зона перед поворотом камеры
constexpr float kRotateScale        = 8.0f;  // радианы twist -> пиксели MMB-сдвига

constexpr float kPi = 3.14159265358979323846f;

static bool isBuildingPlacementMode(SDL3Mouse *mouse)
{
	if (!mouse) return false;
	const Mouse::MouseCursor cursor = mouse->getMouseCursor();
	return cursor == Mouse::BUILD_PLACEMENT ||
	       cursor == Mouse::INVALID_BUILD_PLACEMENT;
}

static float normalizedAngleDelta(float current, float previous)
{
	float delta = current - previous;
	while (delta >  kPi) delta -= 2.0f * kPi;
	while (delta < -kPi) delta += 2.0f * kPi;
	return delta;
}

static float touchDistance(float x1, float y1, float x2, float y2)
{
	const float dx = x1 - x2;
	const float dy = y1 - y2;
	return SDL_sqrtf(dx * dx + dy * dy);
}

static float touchAngle(float x1, float y1, float x2, float y2)
{
	return SDL_atan2f(y2 - y1, x2 - x1);
}

static void resetTouchState()
{
	const Uint64 lastTapTicks = s_touch.lastTapTicks;
	const float  lastTapX     = s_touch.lastTapX;
	const float  lastTapY     = s_touch.lastTapY;
	const bool   haveLastTap  = s_touch.haveLastTap;

	s_touch = TouchState{};
	s_touch.lastTapTicks = lastTapTicks;
	s_touch.lastTapX     = lastTapX;
	s_touch.lastTapY     = lastTapY;
	s_touch.haveLastTap  = haveLastTap;

	s_haveSyntheticPos = false;
}

// Отправка одного синтетического mouse-события в игру.
// xrel/yrel вычисляются автоматически, если не подавлены.
static void sendMouse(SDL3Mouse *mouse, SDL_Window *window,
                      Uint32 type, float x, float y,
                      Uint8 button = 0, float wheelY = 0.0f,
                      Uint8 clicks = 1, bool suppressDelta = false)
{
	if (!mouse || !window) return;

	SDL_Event event;
	SDL_zero(event);
	event.type = type;

	const SDL_WindowID windowID = SDL_GetWindowID(window);

	switch (type) {
	case SDL_EVENT_MOUSE_MOTION:
		event.motion.windowID = windowID;
		event.motion.which    = 0;
		event.motion.x        = x;
		event.motion.y        = y;
		if (s_haveSyntheticPos && !suppressDelta) {
			event.motion.xrel = x - s_lastSyntheticX;
			event.motion.yrel = y - s_lastSyntheticY;
		} else {
			event.motion.xrel = 0.0f;
			event.motion.yrel = 0.0f;
		}
		s_lastSyntheticX   = x;
		s_lastSyntheticY   = y;
		s_haveSyntheticPos = true;
		break;

	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
		event.button.windowID = windowID;
		event.button.which    = 0;
		event.button.button   = button;
		event.button.down     = (type == SDL_EVENT_MOUSE_BUTTON_DOWN);
		event.button.clicks   = clicks;
		event.button.x        = x;
		event.button.y        = y;
		break;

	case SDL_EVENT_MOUSE_WHEEL:
		event.wheel.windowID = windowID;
		event.wheel.which    = 0;
		event.wheel.x        = 0.0f;
		event.wheel.y        = wheelY;
		event.wheel.mouse_x  = x;
		event.wheel.mouse_y  = y;
		break;

	default:
		return;
	}

	mouse->addSDLEvent(&event);
}

// Отправка чистого одиночного тапа. Если это второй тап подряд в пределах
// окна двойного тапа — clicks=2, иначе clicks=1.
static void emitTap(SDL3Mouse *mouse, SDL_Window *window, float x, float y)
{
	const Uint64 now = SDL_GetTicks();
	const float dx = x - s_touch.lastTapX;
	const float dy = y - s_touch.lastTapY;
	const bool isDouble = s_touch.haveLastTap &&
	                      (now - s_touch.lastTapTicks) <= kDoubleTapMs &&
	                      SDL_sqrtf(dx * dx + dy * dy) <= kDoubleTapDistPx;
	const Uint8 clicks = isDouble ? 2 : 1;

	// Наводим курсор в точку тапа без дельты движения камеры.
	sendMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, x, y, 0, 0.0f, 1, true);
	sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_LEFT, 0.0f, clicks);
	sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,   x, y, SDL_BUTTON_LEFT, 0.0f, clicks);

	if (isDouble) {
		s_touch.haveLastTap = false;
	} else {
		s_touch.lastTapTicks = now;
		s_touch.lastTapX     = x;
		s_touch.lastTapY     = y;
		s_touch.haveLastTap  = true;
	}
}

// Старт панорамы камеры (RMB drag) — с предварительным motion в точку нажатия,
// чтобы первый xrel/yrel был корректным и не было рывка камеры.
static void startCameraPan(SDL3Mouse *mouse, SDL_Window *window, float x, float y)
{
	s_touch.phase = TouchState::CameraPan;
	s_touch.lastX = x;
	s_touch.lastY = y;
	sendMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, x, y, 0, 0.0f, 1, true);
	sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_RIGHT);
}

// Старт рамки выделения (LMB drag).
static void startSelection(SDL3Mouse *mouse, SDL_Window *window, float x, float y)
{
	s_touch.phase = TouchState::Selection;
	s_touch.lastX = x;
	s_touch.lastY = y;
	sendMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, x, y, 0, 0.0f, 1, true);
	sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_LEFT);
}

// Включение вращения здания (MMB down) после удержания.
static void startBuildRotation(SDL3Mouse *mouse, SDL_Window *window)
{
	if (s_touch.phase != TouchState::BuildPending) return;
	if (s_touch.firstFingerMoved) return; // палец уже поехал — это не удержание

	s_touch.phase = TouchState::BuildRotate;
	s_touch.lastX = s_touch.downX;
	s_touch.lastY = s_touch.downY;
	sendMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.lastX, s_touch.lastY, 0, 0.0f, 1, true);
	sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN, s_touch.lastX, s_touch.lastY, SDL_BUTTON_MIDDLE);
}

// Отпускание всех зажатых кнопок перед сбросом состояния.
// Инвариант: после resetTouchState() в движке не осталось ни одной зажатой кнопки.
static void releaseAllButtons(SDL3Mouse *mouse, SDL_Window *window)
{
	if (!mouse || !window) return;

	switch (s_touch.phase) {
	case TouchState::CameraPan:
		sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, s_touch.lastX, s_touch.lastY, SDL_BUTTON_RIGHT);
		break;
	case TouchState::Selection:
		sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, s_touch.lastX, s_touch.lastY, SDL_BUTTON_LEFT);
		break;
	case TouchState::BuildRotate:
		sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, s_touch.lastX, s_touch.lastY, SDL_BUTTON_MIDDLE);
		break;
	default:
		break;
	}
}

// Пофреймовая проверка таймера удержания. Палец, стоящий на месте, не
// генерирует SDL-события, поэтому 200 мс для вращения здания надо
// проверять каждый кадр.
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

	// === ОТМЕНА ========================================================
	// FINGER_CANCELED: никаких кликов, только отпускаем зажатые кнопки.
	if (event.type == SDL_EVENT_FINGER_CANCELED) {
		releaseAllButtons(mouse, window);
		const bool keepBuildFixed = (s_touch.phase == TouchState::BuildMoving);
		resetTouchState();
		if (keepBuildFixed) {
			// Превью осталось на земле — фиксированное состояние сохраняем.
			s_touch.phase = TouchState::BuildPending;
		}
		return;
	}

	switch (event.type) {

	// === КАСАНИЕ =======================================================
	case SDL_EVENT_FINGER_DOWN:
	{
		const SDL_FingerID id = event.tfinger.fingerID;

		// Первый палец.
		if (s_touch.phase == TouchState::Idle) {
			s_touch.finger1 = id;
			s_touch.downX = s_touch.lastX = x;
			s_touch.downY = s_touch.lastY = y;
			s_touch.f1x = event.tfinger.x;
			s_touch.f1y = event.tfinger.y;
			s_touch.downTicks = SDL_GetTicks();
			s_touch.firstFingerMoved = false;
			s_touch.phase = isBuildingPlacementMode(mouse)
				? TouchState::BuildPending
				: TouchState::OnePending;
			// Наводим курсор в точку касания — нужно для hover-виджетов.
			sendMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, x, y, 0, 0.0f, 1, true);
			return;
		}

		// Второй палец (или дополнительный в любой фазе).
		if (s_touch.finger1 != 0 && id != s_touch.finger1 && s_touch.finger2 == 0) {
			// Отпускаем зажатое первым пальцем.
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

	// === ДВИЖЕНИЕ ======================================================
	case SDL_EVENT_FINGER_MOTION:
	{
		const SDL_FingerID id = event.tfinger.fingerID;

		// Обновляем координаты пальцев.
		if (id == s_touch.finger1) {
			s_touch.f1x = event.tfinger.x;
			s_touch.f1y = event.tfinger.y;
		} else if (id == s_touch.finger2) {
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
		} else {
			return;
		}

		// --- Два пальца ------------------------------------------------
		if (s_touch.phase == TouchState::TwoFinger) {
			const float dist  = touchDistance(s_touch.f1x, s_touch.f1y,
			                                  s_touch.f2x, s_touch.f2y);
			const float angle = touchAngle(s_touch.f1x, s_touch.f1y,
			                               s_touch.f2x, s_touch.f2y);
			const float distDelta  = dist - s_touch.pinchCurrentDistance;
			const float angleDelta = normalizedAngleDelta(angle, s_touch.pinchCurrentAngle);
			s_touch.pinchCurrentDistance = dist;
			s_touch.pinchCurrentAngle    = angle;

			const float cx = (s_touch.f1x + s_touch.f2x) * 0.5f * (float)width;
			const float cy = (s_touch.f1y + s_touch.f2y) * 0.5f * (float)height;

			// Зум: плавно, по каждому кадру.
			if (SDL_fabsf(distDelta) > 0.5f) {
				s_touch.pinchMoved = true;
				sendMouse(mouse, window, SDL_EVENT_MOUSE_WHEEL,
				          cx, cy, 0, distDelta * kPinchWheelScale);
			}

			// Поворот включается только после порога 40°.
			if (!s_touch.rotationArmed) {
				s_touch.rotationAccum += angleDelta;
				const float deg = SDL_fabsf(s_touch.rotationAccum) * (180.0f / kPi);
				if (deg >= kRotateThresholdDeg) {
					s_touch.rotationArmed = true;
					s_touch.pinchMoved = true;
					sendMouse(mouse, window, SDL_EVENT_MOUSE_MOTION,
					          cx, cy, 0, 0.0f, 1, true);
					sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
					          cx, cy, SDL_BUTTON_MIDDLE);
				}
			} else if (SDL_fabsf(angleDelta) > 0.0001f) {
				// Скорость поворота камеры линейна от скорости кручения.
				sendMouse(mouse, window, SDL_EVENT_MOUSE_MOTION,
				          s_lastSyntheticX + angleDelta * kRotateScale,
				          s_lastSyntheticY);
			}
			return;
		}

		// --- Только первый палец дальше ---------------------------------
		if (id != s_touch.finger1) return;

		// Отсев дублирующихся motion — SDL3 иногда присылает одинаковые точки.
		if (SDL_fabsf(x - s_touch.lastX) < 0.5f &&
		    SDL_fabsf(y - s_touch.lastY) < 0.5f) {
			return;
		}

		// --- Режим стройки: превью едет за пальцем ----------------------
		if (s_touch.phase == TouchState::BuildPending) {
			const float dx = x - s_touch.downX;
			const float dy = y - s_touch.downY;
			if (SDL_sqrtf(dx * dx + dy * dy) < kMoveDeadzonePx) {
				return; // ещё в мёртвой зоне, палец не поехал
			}
			s_touch.firstFingerMoved = true;
			s_touch.phase = TouchState::BuildMoving;
			// Абсолютное движение без дельты — камера не едет.
			sendMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, x, y, 0, 0.0f, 1, true);
			s_touch.lastX = x;
			s_touch.lastY = y;
			return;
		}

		// --- Режим стройки: превью следует за пальцем -------------------
		if (s_touch.phase == TouchState::BuildMoving) {
			sendMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, x, y, 0, 0.0f, 1, true);
			s_touch.lastX = x;
			s_touch.lastY = y;
			return;
		}

		// --- Режим стройки: MMB drag вращает здание ---------------------
		if (s_touch.phase == TouchState::BuildRotate) {
			// suppressDelta=false — sendMouse сам посчитает xrel/yrel из
			// s_lastSyntheticX/Y. Камера при этом не поедет, потому что
			// MMB-drag в игре завязан на поворот здания, а не на камеру.
			sendMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, x, y);
			s_touch.lastX = x;
			s_touch.lastY = y;
			return;
		}

		// --- Один палец, PENDING: первое движение решает -----------------
		if (s_touch.phase == TouchState::OnePending) {
			const float dx = x - s_touch.downX;
			const float dy = y - s_touch.downY;
			if (SDL_sqrtf(dx * dx + dy * dy) < kMoveDeadzonePx) {
				return;
			}

			const Uint64 held = SDL_GetTicks() - s_touch.downTicks;
			s_touch.firstFingerMoved = true;

			if (held >= kSelectionHoldMs) {
				// Палец стоял 250 мс — это рамка выделения.
				startSelection(mouse, window, s_touch.downX, s_touch.downY);
			} else {
				// Раннее движение — панорама камеры за пальцем.
				startCameraPan(mouse, window, s_touch.downX, s_touch.downY);
			}

			s_touch.lastX = x;
			s_touch.lastY = y;
			sendMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, x, y);
			return;
		}

		// --- Панорама камеры --------------------------------------------
		if (s_touch.phase == TouchState::CameraPan) {
			sendMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, x, y);
			s_touch.lastX = x;
			s_touch.lastY = y;
			return;
		}

		// --- Рамка выделения --------------------------------------------
		if (s_touch.phase == TouchState::Selection) {
			sendMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, x, y);
			s_touch.lastX = x;
			s_touch.lastY = y;
			return;
		}

		return;
	}

	// === ОТПУСКАНИЕ ====================================================
	case SDL_EVENT_FINGER_UP:
	{
		const SDL_FingerID id = event.tfinger.fingerID;

		// --- Два пальца: отпускание -------------------------------------
		if (s_touch.phase == TouchState::TwoFinger) {
			if (id == s_touch.finger1)      s_touch.finger1 = 0;
			else if (id == s_touch.finger2) s_touch.finger2 = 0;
			else return;

			if (s_touch.finger1 != 0 || s_touch.finger2 != 0) {
				return; // ждём второй палец
			}

			if (s_touch.rotationArmed) {
				sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
				          s_lastSyntheticX, s_lastSyntheticY, SDL_BUTTON_MIDDLE);
			}

			// Короткий синхронный шлепок = ПКМ (отмена).
			// Условие: быстрый, без движения, без поворота, и первый палец
			// тоже не двигался до приставления второго.
			const Uint64 held = SDL_GetTicks() - s_touch.twoFingerDownTicks;
			const bool isTap = (held <= kTwoFingerTapMs) &&
			                   !s_touch.pinchMoved &&
			                   !s_touch.rotationArmed &&
			                   !s_touch.firstFingerMoved;

			if (isTap) {
				const float cx = s_lastSyntheticX;
				const float cy = s_lastSyntheticY;
				sendMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, cx, cy, 0, 0.0f, 1, true);
				sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN, cx, cy, SDL_BUTTON_RIGHT);
				sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,   cx, cy, SDL_BUTTON_RIGHT);
			}
			resetTouchState();
			return;
		}

		if (id != s_touch.finger1) return;

		// --- Режим стройки: MMB up + LMB клик = авто-стройка -----------
		if (s_touch.phase == TouchState::BuildRotate) {
			sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, x, y, SDL_BUTTON_MIDDLE);
			sendMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, x, y, 0, 0.0f, 1, true);
			sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_LEFT);
			sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,   x, y, SDL_BUTTON_LEFT);
			resetTouchState();
			return;
		}

		// --- Режим стройки: превью двигалось, отпустили ----------------
		if (s_touch.phase == TouchState::BuildMoving) {
			// Превью фиксируется. Кнопок не отправляли — стройка не началась.
			// Следующее касание снова попадёт в BuildPending и сможет либо
			// подтвердить постройку (тап < 200 мс), либо включить вращение
			// (удержание 200 мс).
			s_touch.finger1 = 0;
			s_touch.firstFingerMoved = false;
			s_touch.phase = TouchState::BuildPending;
			return;
		}

		// --- Режим стройки: тап без движения = LMB клик (стройка) ------
		if (s_touch.phase == TouchState::BuildPending) {
			const Uint64 held = SDL_GetTicks() - s_touch.downTicks;
			if (held < kBuildRotateHoldMs && !s_touch.firstFingerMoved) {
				sendMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, x, y, 0, 0.0f, 1, true);
				sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_LEFT);
				sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,   x, y, SDL_BUTTON_LEFT);
			}
			// Иначе — фаза BuildRotate, но её обрабатывает updateTouchHold;
			// если палец отпустили раньше 200 мс, ничего не делаем.
			resetTouchState();
			return;
		}

		// --- Панорама камеры: RMB up -----------------------------------
		if (s_touch.phase == TouchState::CameraPan) {
			sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, x, y, SDL_BUTTON_RIGHT);
			resetTouchState();
			return;
		}

		// --- Рамка выделения: LMB up -----------------------------------
		if (s_touch.phase == TouchState::Selection) {
			sendMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, x, y, SDL_BUTTON_LEFT);
			resetTouchState();
			return;
		}

		// --- Один палец, без движения = тап ----------------------------
		if (s_touch.phase == TouchState::OnePending) {
			if (!s_touch.firstFingerMoved) {
				emitTap(mouse, window, s_touch.downX, s_touch.downY);
			}
			resetTouchState();
			return;
		}

		return;
	}

	default:
		return;
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
	// Пофреймовая проверка удержания 200 мс для вращения здания. Палец,
	// стоящий на месте, не генерирует SDL-события — без этого вызова
	// удержание никогда бы не сработало.
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