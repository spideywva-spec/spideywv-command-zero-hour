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
** SDL3Main.cpp
**
** Entry point for Linux builds using SDL3 windowing and DXVK graphics.
**
** TheSuperHackers @feature CnC_Generals_Linux 07/02/2026
** Entry point replaces WinMain() for Linux builds.
** Instantiates SDL3GameEngine and calls GameMain().
*/

#ifndef _WIN32

// SYSTEM INCLUDES
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
// On iOS, SDL renames main() to SDL_main and provides its own UIApplicationMain
// bootstrap; the app lifecycle (suspend/resume, window) is owned by SDL.
#include <SDL3/SDL_main.h>
#include <cerrno>
#include <sys/stat.h>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <fstream>
#include <unordered_map>
#endif
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <unistd.h>   // _exit()
#include <glob.h>     // glob() for Vulkan ICD discovery
#if defined(__APPLE__) && (!defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE)
#include <execinfo.h>
#include <signal.h>
#endif

// USER INCLUDES (match WinMain.cpp pattern)
#include "Lib/BaseType.h"
#include "Common/CommandLine.h"
#include "Common/CriticalSection.h"
#include "Common/GlobalData.h"
#include "Common/GameEngine.h"
#include "Common/GameMemory.h"
#include "Common/Debug.h"
#include "Common/version.h"  // GeneralsX @bugfix BenderAI 14/02/2026 Version class + TheVersion extern
#include "SDL3GameEngine.h"

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#include "IOSProfileLauncher.h"
#include "IOSGameOverlay.h"
#endif

// DXVK WSI
#define DXVK_WSI_SDL3 1
#include <wsi/native_wsi.h>

// CRITICAL SECTIONS (Linux needs these too)
static CriticalSection critSec1;
static CriticalSection critSec2;
static CriticalSection critSec3;
static CriticalSection critSec4;
static CriticalSection critSec5;

// GLOBAL COMMAND LINE ARGUMENTS
// TheSuperHackers @build felipebraz 13/02/2026
// Store argc/argv from main() for use by CommandLine.cpp parseCommandLine() on Linux
// Windows provides these automatically; Linux needs explicit globals
int __argc = 0;          ///< global argument count
char** __argv = nullptr; ///< global argument vector

// GLOBAL WINDOW HANDLE
// TheSuperHackers @build felipebraz 13/02/2026
// ApplicationHWnd is declared extern in GeneralsMD/Code/Main/WinMain.h
// On Linux, we cast SDL_Window* to HWND type for compatibility
HWND ApplicationHWnd = nullptr;  ///< our application window handle

// GLOBAL SDL3 WINDOW
// GeneralsX @feature felipebraz 16/02/2026
// SDL3 window created in main() before GameMain(), stored globally for engine access
SDL_Window* TheSDL3Window = nullptr;

// GAME TEXT FILE PATHS
// TheSuperHackers @build felipebraz 13/02/2026
// GameText.cpp uses these paths to load CSF and STR files (game localization)
// Format %s is replaced with language code in GameTextManager::init()
// GeneralsX @bugfix BenderAI 13/02/2026 - Fix case-sensitivity on Linux (generals.csf vs Generals.csf)
const Char *g_csfFile = "data/%s/generals.csf";  ///< CSF file path (lowercase for Linux compatibility)
const Char *g_strFile = "data/Generals.str";     ///< STR file path

// Extern declarations (from GameMain.cpp)
extern Int GameMain();

/**
 * FilterSoftwareVulkanICDs
 *
 * Sets VK_DRIVER_FILES to only hardware Vulkan ICDs, excluding LLVMpipe/lavapipe.
 *
 * Workaround for Mesa/LLVM 20.x bug: libvulkan_lvp.so (LLVMpipe Vulkan ICD) crashes
 * during dlopen() static initialization with a null-ptr deref in llvm::Regex::Regex().
 * The Vulkan loader loads ALL ICDs found in the ICD directories when
 * vkEnumerateInstanceExtensionProperties() is called, which triggers the crash.
 * Filtering hardware-only ICDs via VK_DRIVER_FILES prevents loading libvulkan_lvp.so.
 *
 * Only applied when neither VK_DRIVER_FILES nor VK_ICD_FILENAMES is already set,
 * so the user can always override by setting those variables externally.
 *
 * GeneralsX @bugfix BenderAI 06/03/2026
 */
static void FilterSoftwareVulkanICDs()
{
	if (getenv("VK_DRIVER_FILES") || getenv("VK_ICD_FILENAMES")) {
		return;
	}

	auto icd_is_software = [](const char *name) -> bool {
		char low[256] = "";
		for (int i = 0; name[i] && i < 255; ++i) {
			low[i] = (char)tolower((unsigned char)name[i]);
		}
		return strstr(low, "lvp") || strstr(low, "lavapipe") || strstr(low, "softpipe") || strstr(low, "llvmpipe");
	};

	static char hw_icds[4096] = "";
	const char *patterns[] = {
		"/usr/share/vulkan/icd.d/*.json",
		"/etc/vulkan/icd.d/*.json",
		nullptr
	};

	glob_t gl = {};
	int gflags = 0;
	for (int i = 0; patterns[i]; ++i) {
		if (glob(patterns[i], gflags, nullptr, &gl) == 0) {
			gflags = GLOB_APPEND;
		}
	}

	bool found_hw = false;
	for (size_t i = 0; i < gl.gl_pathc; ++i) {
		const char *path = gl.gl_pathv[i];
		const char *base = strrchr(path, '/');
		base = base ? base + 1 : path;
		if (icd_is_software(base)) {
			fprintf(stderr, "INFO: Vulkan ICD filter: skipping software ICD '%s'\n", base);
			continue;
		}
		if (found_hw) {
			strncat(hw_icds, ":", sizeof(hw_icds) - strlen(hw_icds) - 1);
		}
		strncat(hw_icds, path, sizeof(hw_icds) - strlen(hw_icds) - 1);
		found_hw = true;
	}
	globfree(&gl);

	if (found_hw) {
		setenv("VK_DRIVER_FILES", hw_icds, 1);
		fprintf(stderr, "INFO: Vulkan ICD filter: VK_DRIVER_FILES=%s\n", hw_icds);
	} else {
		fprintf(stderr, "WARNING: Vulkan ICD filter: no hardware ICDs found, LLVMpipe exclusion skipped\n");
		fprintf(stderr, "WARNING: If startup crashes in libvulkan_lvp.so, set VK_DRIVER_FILES manually\n");
	}
}

/**
 * FilterPipeWireOpenAL
 *
 * Sets ALSOFT_DRIVERS to skip PipeWire, falling back to pulse/alsa.
 *
 * Workaround for openal-soft PipeWire backend crash: alcOpenDevice() segfaults
 * inside the PipeWire backend while opening the default playback device.
 * The crash occurs in PipeWire's stream/context internals and is unrecoverable
 * from userspace. Excluding PipeWire via ALSOFT_DRIVERS causes openal-soft to
 * fall back to the PulseAudio backend, which works correctly on PipeWire systems
 * via the PulseAudio compatibility layer.
 *
 * NOTE: openal-soft reads ALSOFT_DRIVERS from a static global constructor when
 * libopenal.so is loaded by the dynamic linker, which is before main() runs.
 * This function is therefore only effective for builds that use lazy
 * initialization. The authoritative fix is in the launch scripts (run-linux-zh.sh
 * etc.), which set ALSOFT_DRIVERS before the binary starts.
 *
 * Only applied when ALSOFT_DRIVERS is not already set by the user.
 *
 * GeneralsX @bugfix 09/03/2026
 */
static void FilterPipeWireOpenAL()
{
	// GeneralsX @bugfix Copilot 24/03/2026 PipeWire/OpenAL workaround is Linux-only; keep macOS CoreAudio backend selection untouched.
	#if defined(__linux__)
	// Crash: alcOpenDevice() hits 'movaps %xmm1,0x26260(%rbx)' — SSE movaps requires
	// 16-byte alignment; a misaligned ALCdevice struct faults regardless of backend.
	// Disabling CPU extensions forces openal-soft to use scalar code that has no
	// alignment requirements. Also exclude pipewire which has its own crash at
	// device-open time on PipeWire 1.4.x.
	// NOTE: these env vars are authoritative only when set before the binary loads
	// (openal-soft reads them from a static constructor). The launch scripts set them
	// first; this is a best-effort fallback for lazy-init builds.
	if (!getenv("ALSOFT_DISABLE_CPU_EXTS")) {
		setenv("ALSOFT_DISABLE_CPU_EXTS", "all", 1);
		fprintf(stderr, "INFO: OpenAL: ALSOFT_DISABLE_CPU_EXTS=all (movaps alignment crash workaround)\n");
	}
	if (!getenv("ALSOFT_DRIVERS")) {
		setenv("ALSOFT_DRIVERS", "pulse,alsa,oss,jack,null,wave", 1);
		fprintf(stderr, "INFO: OpenAL: ALSOFT_DRIVERS=pulse,alsa,oss,jack,null,wave (pipewire excluded)\n");
	}
	#else
	fprintf(stderr, "INFO: OpenAL: keeping default driver selection on non-Linux platform\n");
	#endif
}

/**
 * CreateGameEngine
 *
 * Factory function for SDL3GameEngine on Linux.
 * Called by GameMain() to instantiate platform-specific engine.
 *
 * @return SDL3GameEngine instance
 */
GameEngine *CreateGameEngine(void)
{
	fprintf(stderr, "INFO: CreateGameEngine() - Creating SDL3GameEngine for Linux\n");
	SDL3GameEngine *engine = NEW SDL3GameEngine();
	return engine;
}

/**
 * main
 *
 * Linux entry point (replaces WinMain on Windows).
 * Initializes subsystems and calls GameMain().
 *
 * @param argc Command line argument count
 * @param argv Command line arguments
 * @return Exit code (0 = success)
 */
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
static std::string IOSContraTrim(const std::string &value)
{
    size_t first = 0;
    while (first < value.size() && std::isspace((unsigned char)value[first]))
        ++first;

    size_t last = value.size();
    while (last > first && std::isspace((unsigned char)value[last - 1]))
        --last;

    return value.substr(first, last - first);
}

static std::string IOSContraLower(std::string value)
{
    for (char &c : value)
        c = (char)std::tolower((unsigned char)c);
    return value;
}

static bool IOSContraEndsWith(const std::string &value, const char *suffix)
{
    const size_t suffixLength = strlen(suffix);
    return value.size() >= suffixLength &&
           value.compare(value.size() - suffixLength, suffixLength, suffix) == 0;
}

static std::filesystem::path IOSContraSettingsPath()
{
    const char *home = getenv("HOME");
    if (home == nullptr || home[0] == '\0')
        return std::filesystem::path("ContraSettings.ini");
    return std::filesystem::path(home) / "Documents" / "ContraSettings.ini";
}

static std::unordered_map<std::string, std::string> IOSLoadContraSettings()
{
    std::unordered_map<std::string, std::string> values;
    std::ifstream input(IOSContraSettingsPath());
    std::string line;
    while (std::getline(input, line))
    {
        line = IOSContraTrim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        const size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;

        std::string key = IOSContraLower(IOSContraTrim(line.substr(0, equals)));
        std::string value = IOSContraTrim(line.substr(equals + 1));
        if (!key.empty())
            values[key] = value;
    }
    return values;
}

static std::string IOSContraSetting(
    const std::unordered_map<std::string, std::string> &settings,
    const char *key,
    const char *fallback)
{
    auto it = settings.find(IOSContraLower(key));
    return it == settings.end() || it->second.empty() ? fallback : it->second;
}

static bool IOSContraSettingBool(
    const std::unordered_map<std::string, std::string> &settings,
    const char *key,
    bool fallback)
{
    std::string value = IOSContraLower(IOSContraSetting(settings, key, fallback ? "Yes" : "No"));
    return value == "yes" || value == "true" || value == "1" || value == "on";
}

static bool IOSContraCoreArchive(const std::string &logicalCtr)
{
    static const char *required[] = {
        "_ini.ctr",
        "_maps.ctr",
        "_ai.ctr",
        "_terrain.ctr",
        "_textures.ctr",
        "_w3d.ctr",
        "_window.ctr",
        "_audio.ctr",
        "_gamedata.ctr",
        "_patch1.ctr",
        nullptr
    };

    for (int i = 0; required[i] != nullptr; ++i)
    {
        if (IOSContraEndsWith(logicalCtr, required[i]))
            return true;
    }
    return false;
}

static bool IOSContraArchiveShouldBeActive(
    const std::filesystem::path &source,
    const std::unordered_map<std::string, std::string> &settings)
{
    std::string lower = IOSContraLower(source.filename().string());
    const bool distributedActive = IOSContraEndsWith(lower, ".big");

    if (distributedActive)
        lower.replace(lower.size() - 4, 4, ".ctr");

    if (IOSContraCoreArchive(lower))
        return true;

    const std::string voices = IOSContraLower(IOSContraSetting(settings, "UnitVoices", "English"));
    if (IOSContraEndsWith(lower, "_unitvoicesenglish.ctr"))
        return voices != "native";
    if (IOSContraEndsWith(lower, "_unitvoicesnative.ctr"))
        return voices == "native";

    const std::string hotkeys = IOSContraLower(IOSContraSetting(settings, "Hotkeys", "Original"));
    const std::string hotkeyLanguage =
        IOSContraLower(IOSContraSetting(settings, "HotkeyLanguage", "English"));
    if (IOSContraEndsWith(lower, "_hotkeysoriginal_english.ctr"))
        return hotkeys == "original" && hotkeyLanguage == "english";
    if (IOSContraEndsWith(lower, "_hotkeysoriginal_russian.ctr"))
        return hotkeys == "original" && hotkeyLanguage == "russian";
    if (IOSContraEndsWith(lower, "_hotkeysleikeze_english.ctr"))
        return hotkeys == "leikeze" && hotkeyLanguage == "english";
    if (IOSContraEndsWith(lower, "_hotkeysleikeze_russian.ctr"))
        return hotkeys == "leikeze" && hotkeyLanguage == "russian";

    const std::string controlBar = IOSContraLower(IOSContraSetting(settings, "ControlBar", "Contra"));
    if (IOSContraEndsWith(lower, "_controlbarpro.ctr"))
        return controlBar == "pro";
    if (IOSContraEndsWith(lower, "_controlbarstandard.ctr"))
        return controlBar == "standard";

    const std::string cameos = IOSContraLower(IOSContraSetting(settings, "Cameos", "Standard"));
    if (IOSContraEndsWith(lower, "_cameoshd.ctr"))
        return cameos == "hd";

    const std::string music = IOSContraLower(IOSContraSetting(settings, "Music", "Standard"));
    if (IOSContraEndsWith(lower, "_musicenhanced.ctr") || IOSContraEndsWith(lower, "_newmusic.ctr"))
        return music == "enhanced";
    if (IOSContraEndsWith(lower, "_musicthescore.ctr"))
        return music == "the score";

    const std::string portraits = IOSContraLower(IOSContraSetting(settings, "Portraits", "Standard"));
    if (IOSContraEndsWith(lower, "_funnygeneralportraits.ctr"))
        return portraits == "funny";

    if (IOSContraEndsWith(lower, "_disablefogeffects.ctr"))
        return !IOSContraSettingBool(settings, "FogEffects", false);
    if (IOSContraEndsWith(lower, "_disablewatereffects.ctr"))
        return !IOSContraSettingBool(settings, "WaterEffects", true);
    if (IOSContraEndsWith(lower, "_disableextrabuildingprops.ctr"))
        return !IOSContraSettingBool(settings, "ExtraBuildingProps", true);

    // Keep unknown distributed .big files active and unknown .ctr files inactive.
    return distributedActive;
}

static bool IOSPrepareContraRuntimeProfile(
    const char *sourceProfilePath,
    char *runtimePath,
    size_t runtimePathSize,
    bool *forceFullViewport)
{
    if (sourceProfilePath == nullptr || runtimePath == nullptr || runtimePathSize == 0)
        return false;

    const char *home = getenv("HOME");
    if (home == nullptr || home[0] == '\0')
    {
        fprintf(stderr, "[CONTRA-SETTINGS] HOME is unavailable; using bundled profile\n");
        return false;
    }

    auto settings = IOSLoadContraSettings();
    fprintf(stderr, "[CONTRA-SETTINGS] runtime-overlay-version=1 settings='%s'\n",
            IOSContraSettingsPath().string().c_str());
    const std::string controlBar =
        IOSContraLower(IOSContraSetting(settings, "ControlBar", "Contra"));
    if (forceFullViewport != nullptr)
        *forceFullViewport = controlBar == "pro";

    std::filesystem::path source(sourceProfilePath);
    std::filesystem::path runtime =
        std::filesystem::path(home) / "Documents" / "ContraRuntime";

    std::error_code ec;
    std::filesystem::remove_all(runtime, ec);
    ec.clear();
    std::filesystem::create_directories(runtime, ec);
    if (ec)
    {
        fprintf(stderr, "[CONTRA-SETTINGS] failed to create runtime profile: %s\n",
                ec.message().c_str());
        return false;
    }

    int activeArchives = 0;
    int inactiveArchives = 0;
    int linkedEntries = 0;

    for (std::filesystem::directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::filesystem::path sourceEntry = it->path();
        std::filesystem::path targetName = sourceEntry.filename();

        std::error_code typeError;
        if (it->is_directory(typeError) && !typeError)
        {
            std::filesystem::path target = runtime / targetName;
            std::error_code linkError;
            std::filesystem::create_directory_symlink(sourceEntry, target, linkError);
            if (linkError)
            {
                fprintf(stderr,
                        "[CONTRA-SETTINGS] directory-link-failed source='%s' error='%s'\n",
                        sourceEntry.string().c_str(),
                        linkError.message().c_str());
                return false;
            }
            ++linkedEntries;
            continue;
        }

        std::string ext = IOSContraLower(sourceEntry.extension().string());
        if (ext == ".big" || ext == ".ctr")
        {
            const bool active = IOSContraArchiveShouldBeActive(sourceEntry, settings);
            targetName.replace_extension(active ? ".big" : ".ctr");

            // Contra's optional control-bar packs are named with a leading "!!".
            // Our portable BIG loader sorts filenames ascending and, for -mod
            // directories, later archives overwrite earlier ones. That means
            // !ContraXBeta2_Window.big can otherwise overwrite the selected
            // !!...ControlBarPro/Standard archive, making every launcher choice
            // look identical. Give the selected control-bar overlay an explicit
            // last-sorting runtime name so it wins only when that option is active.
            std::string normalizedArchiveName = IOSContraLower(sourceEntry.filename().string());
            if (active &&
                (IOSContraEndsWith(normalizedArchiveName, "_controlbarpro.ctr") ||
                 IOSContraEndsWith(normalizedArchiveName, "_controlbarstandard.ctr") ||
                 IOSContraEndsWith(normalizedArchiveName, "_controlbarpro.big") ||
                 IOSContraEndsWith(normalizedArchiveName, "_controlbarstandard.big")))
            {
                targetName = "zzzz__IOS_Selected_ControlBar.big";
                fprintf(stderr,
                        "[CONTRA-SETTINGS] controlbar-priority source='%s' runtime='%s'\n",
                        sourceEntry.filename().string().c_str(),
                        targetName.string().c_str());
            }

            if (active)
                ++activeArchives;
            else
                ++inactiveArchives;

            fprintf(stderr,
                    "[CONTRA-SETTINGS] archive='%s' state=%s runtime='%s'\n",
                    sourceEntry.filename().string().c_str(),
                    active ? "active" : "inactive",
                    targetName.string().c_str());
        }

        std::filesystem::path target = runtime / targetName;
        std::error_code linkError;
        std::filesystem::create_symlink(sourceEntry, target, linkError);
        if (linkError)
        {
            fprintf(stderr,
                    "[CONTRA-SETTINGS] file-link-failed source='%s' target='%s' error='%s'\n",
                    sourceEntry.string().c_str(),
                    target.string().c_str(),
                    linkError.message().c_str());
            return false;
        }
        ++linkedEntries;
    }

    if (ec)
    {
        fprintf(stderr, "[CONTRA-SETTINGS] runtime scan failed: %s\n", ec.message().c_str());
        return false;
    }

    const std::string runtimeString = runtime.string();
    if (runtimeString.size() + 1 > runtimePathSize)
    {
        fprintf(stderr, "[CONTRA-SETTINGS] runtime path is too long\n");
        return false;
    }
    strlcpy(runtimePath, runtimeString.c_str(), runtimePathSize);

    fprintf(stderr,
            "[CONTRA-SETTINGS] runtime-ready path='%s' controlBar='%s' cameos='%s' music='%s' voices='%s' hotkeys='%s/%s' active=%d inactive=%d entries=%d forceFullViewport=%d\n",
            runtimePath,
            IOSContraSetting(settings, "ControlBar", "Contra").c_str(),
            IOSContraSetting(settings, "Cameos", "Standard").c_str(),
            IOSContraSetting(settings, "Music", "Standard").c_str(),
            IOSContraSetting(settings, "UnitVoices", "English").c_str(),
            IOSContraSetting(settings, "Hotkeys", "Original").c_str(),
            IOSContraSetting(settings, "HotkeyLanguage", "English").c_str(),
            activeArchives,
            inactiveArchives,
            linkedEntries,
            forceFullViewport != nullptr && *forceFullViewport ? 1 : 0);

    return true;
}

static void LogIOSProfileContents(const char *modPath)
{
    if (modPath == nullptr || modPath[0] == '\0')
        return;

    fprintf(stderr, "[CONTRA-DIAG] profile-path=%s\n", modPath);

    std::error_code ec;
    std::filesystem::path root(modPath);
    std::filesystem::recursive_directory_iterator it(
        root,
        std::filesystem::directory_options::follow_directory_symlink,
        ec);
    std::filesystem::recursive_directory_iterator end;

    int activeBigCount = 0;
    int inactiveCtrCount = 0;
    for (; !ec && it != end; it.increment(ec))
    {
        std::error_code typeError;
        if (!it->is_regular_file(typeError) || typeError)
            continue;

        std::string ext = it->path().extension().string();
        for (char &c : ext)
            c = (char)std::tolower((unsigned char)c);

        if (ext == ".big")
        {
            ++activeBigCount;
            std::error_code relError;
            std::filesystem::path rel = std::filesystem::relative(it->path(), root, relError);
            fprintf(stderr, "[CONTRA-DIAG] active-big=%s\n",
                    (relError ? it->path() : rel).string().c_str());
        }
        else if (ext == ".ctr")
        {
            ++inactiveCtrCount;
        }
    }

    if (ec)
        fprintf(stderr, "[CONTRA-DIAG] profile-scan-error=%s\n", ec.message().c_str());

    std::error_code scriptsError;
    bool scripts = std::filesystem::exists(root / "Data" / "Scripts", scriptsError);
    scriptsError.clear();
    bool scripts1 = std::filesystem::exists(root / "Data" / "Scripts1", scriptsError);

    fprintf(stderr,
            "[CONTRA-DIAG] active-big-count=%d inactive-ctr-count=%d Data/Scripts=%s Data/Scripts1=%s\n",
            activeBigCount,
            inactiveCtrCount,
            scripts ? "yes" : "no",
            scripts1 ? "yes" : "no");
}

// GeneralsX @feature dvorovrus 25/09/2026 Convert the launcher's profile choice
// into the engine's existing -mod directory mechanism. Base assets remain in
// GameData; mod-only overlays live outside it in <bundle>/Profiles so vanilla
// never sees Enhanced/Contra archives unless explicitly selected.
static void InjectIOSProfileModArgument(const char *profileId)
{
    if (profileId == nullptr || strcmp(profileId, "vanilla") == 0)
        return;

    for (int i = 1; i < __argc; ++i)
    {
        if (__argv[i] != nullptr &&
            (strcmp(__argv[i], "-mod") == 0 || strcmp(__argv[i], "--mod") == 0))
        {
            fprintf(stderr, "INFO: iOS launcher: explicit -mod already present, keeping caller override\n");
            return;
        }
    }

    const char *profileDir = nullptr;
    if (strcmp(profileId, "enhanced") == 0)
        profileDir = "enhanced";
    else if (strcmp(profileId, "contra-x") == 0)
        profileDir = "contra-x";
    else
        return;

    const bool isContra = strcmp(profileId, "contra-x") == 0;

    if (__argc <= 0 || __argv[0] == nullptr)
        return;

    const char *slash = strrchr(__argv[0], '/');
    if (slash == nullptr)
        return;

    const size_t appDirLength = (size_t)(slash - __argv[0]);
    static char bundledModPath[1024];
    if (appDirLength + strlen(profileDir) + 12 >= sizeof(bundledModPath))
    {
        fprintf(stderr, "ERROR: iOS launcher: profile path is too long\n");
        return;
    }

    memcpy(bundledModPath, __argv[0], appDirLength);
    bundledModPath[appDirLength] = '\0';
    strncat(bundledModPath, "/Profiles/", sizeof(bundledModPath) - strlen(bundledModPath) - 1);
    strncat(bundledModPath, profileDir, sizeof(bundledModPath) - strlen(bundledModPath) - 1);

    if (access(bundledModPath, R_OK) != 0)
    {
        fprintf(stderr, "ERROR: iOS launcher: selected profile '%s' is missing at %s\n",
                profileId, bundledModPath);
        return;
    }

    static char runtimeModPath[1024];
    const char *selectedModPath = bundledModPath;
    bool forceFullViewport = false;

    if (isContra &&
        IOSPrepareContraRuntimeProfile(
            bundledModPath,
            runtimeModPath,
            sizeof(runtimeModPath),
            &forceFullViewport))
    {
        selectedModPath = runtimeModPath;
    }
    else if (isContra)
    {
        auto settings = IOSLoadContraSettings();
        forceFullViewport =
            IOSContraLower(IOSContraSetting(settings, "ControlBar", "Contra")) == "pro";
        fprintf(stderr,
                "[CONTRA-SETTINGS] runtime overlay unavailable; using bundled profile\n");
    }

    static char modFlag[] = "-mod";
    static char forceFullViewportFlag[] = "-forcefullviewport";
    static char *profileArgv[64];
    int count = 0;
    const int maxBaseArgs = forceFullViewport ? 60 : 61;
    for (int i = 0; i < __argc && count < maxBaseArgs; ++i)
        profileArgv[count++] = __argv[i];

    profileArgv[count++] = modFlag;
    profileArgv[count++] = const_cast<char *>(selectedModPath);

    // Control Bar Pro expects GenTool-style full viewport behavior. Contra's
    // native and standard control bars intentionally do not enable this flag.
    if (forceFullViewport)
        profileArgv[count++] = forceFullViewportFlag;

    profileArgv[count] = nullptr;

    __argv = profileArgv;
    __argc = count;

    fprintf(stderr, "INFO: iOS launcher: profile '%s' -> -mod %s%s\n",
            profileId,
            selectedModPath,
            forceFullViewport ? " -forcefullviewport" : "");

    if (isContra)
        LogIOSProfileContents(selectedModPath);
}

static int g_iosDiagnosticLogFd = -1;
static size_t g_iosDiagnosticLogWritten = 0;
static bool g_iosDiagnosticLogCapMarked = false;

static void IOSDiagnosticArchivePath(const char *home, int index, char *outPath, size_t outSize)
{
	snprintf(outPath, outSize, "%s/Documents/generals-stderr-%02d.log", home, index);
}

void GeneralsXClearIOSDiagnosticLogs()
{
	const char *home = getenv("HOME");
	if (home == nullptr || home[0] == '\0')
		return;

	fflush(stderr);

	for (int index = 1; index <= 9; ++index)
	{
		char archivePath[1024];
		IOSDiagnosticArchivePath(home, index, archivePath, sizeof(archivePath));
		remove(archivePath);
	}

	char legacyPrevPath[1024];
	snprintf(legacyPrevPath, sizeof(legacyPrevPath), "%s/Documents/generals-stderr-prev.log", home);
	remove(legacyPrevPath);

	if (g_iosDiagnosticLogFd >= 0)
	{
		ftruncate(g_iosDiagnosticLogFd, 0);
		lseek(g_iosDiagnosticLogFd, 0, SEEK_SET);
		g_iosDiagnosticLogWritten = 0;
		g_iosDiagnosticLogCapMarked = false;
	}
	else
	{
		char currentPath[1024];
		snprintf(currentPath, sizeof(currentPath), "%s/Documents/generals-stderr.log", home);
		remove(currentPath);
	}

	fprintf(stderr, "INFO: diagnostic log history cleared by user\n");
}

#endif

#if defined(__APPLE__) && (!defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE)
static void MacFatalSignalHandler(int signalNumber)
{
	char header[128];
	const int headerLength = snprintf(header, sizeof(header),
	                                  "\n[FATAL-SIGNAL] signal=%d\n[FATAL-SIGNAL] native backtrace follows:\n",
	                                  signalNumber);
	if (headerLength > 0)
	{
		write(STDERR_FILENO, header, (size_t)headerLength);
	}

	void *frames[64];
	const int frameCount = backtrace(frames, (int)(sizeof(frames) / sizeof(frames[0])));
	if (frameCount > 0)
	{
		backtrace_symbols_fd(frames, frameCount, STDERR_FILENO);
	}
	_exit(128 + signalNumber);
}

static void InstallMacFatalSignalHandlers()
{
	struct sigaction action = {};
	action.sa_handler = MacFatalSignalHandler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_RESETHAND;

	const int signals[] = { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE };
	for (const int signalNumber : signals)
	{
		sigaction(signalNumber, &action, nullptr);
	}
}
#endif

int main(int argc, char* argv[])
{
	int exitcode = 1;

#if defined(__APPLE__) && (!defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE)
	InstallMacFatalSignalHandlers();
	fprintf(stderr, "[CRASH-DIAG] macOS fatal-signal backtrace handlers installed\n");
#endif

	// TheSuperHackers @build felipebraz 13/02/2026
	// Store command line arguments in globals for CommandLine.cpp parser
	__argc = argc;
	__argv = argv;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Diagnostic capture: keep the current launch plus the nine previous app
	// sessions in Documents. Each process launch is one session, so reproducing
	// an intermittent crash no longer loses the useful log after reopening the app.
	{
		setenv("DXVK_LOG_LEVEL", "none", 0);
		const char *diagHome = getenv("HOME");
		if (diagHome != nullptr)
		{
			char diagPath[1024];
			char legacyPrevPath[1024];
			snprintf(diagPath, sizeof(diagPath), "%s/Documents/generals-stderr.log", diagHome);
			snprintf(legacyPrevPath, sizeof(legacyPrevPath), "%s/Documents/generals-stderr-prev.log", diagHome);

			bool hadArchiveHistory = false;
			for (int index = 1; index <= 9; ++index)
			{
				char archivePath[1024];
				IOSDiagnosticArchivePath(diagHome, index, archivePath, sizeof(archivePath));
				if (access(archivePath, F_OK) == 0)
				{
					hadArchiveHistory = true;
					break;
				}
			}

			char oldestPath[1024];
			IOSDiagnosticArchivePath(diagHome, 9, oldestPath, sizeof(oldestPath));
			remove(oldestPath);
			for (int index = 8; index >= 1; --index)
			{
				char sourcePath[1024];
				char destinationPath[1024];
				IOSDiagnosticArchivePath(diagHome, index, sourcePath, sizeof(sourcePath));
				IOSDiagnosticArchivePath(diagHome, index + 1, destinationPath, sizeof(destinationPath));
				if (access(sourcePath, F_OK) == 0)
					rename(sourcePath, destinationPath);
			}

			const bool hadCurrentLog = access(diagPath, F_OK) == 0;
			if (hadCurrentLog)
			{
				char newestArchivePath[1024];
				IOSDiagnosticArchivePath(diagHome, 1, newestArchivePath, sizeof(newestArchivePath));
				rename(diagPath, newestArchivePath);
			}

			// Migrate the old current/previous pair once when upgrading from the
			// two-file scheme, preserving both sessions where possible.
			if (access(legacyPrevPath, F_OK) == 0)
			{
				if (!hadArchiveHistory)
				{
					char migrationPath[1024];
					IOSDiagnosticArchivePath(diagHome, hadCurrentLog ? 2 : 1, migrationPath, sizeof(migrationPath));
					if (access(migrationPath, F_OK) != 0)
						rename(legacyPrevPath, migrationPath);
					else
						remove(legacyPrevPath);
				}
				else
				{
					remove(legacyPrevPath);
				}
			}

			g_iosDiagnosticLogFd = open(diagPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (g_iosDiagnosticLogFd >= 0)
			{
				g_iosDiagnosticLogWritten = 0;
				g_iosDiagnosticLogCapMarked = false;

				FILE *sink = funopen(nullptr,
					nullptr,
					[](void *, const char *buf, int len) -> int {
						static const size_t kLogCap = 8u * 1024u * 1024u;
						if (g_iosDiagnosticLogFd < 0)
							return len;

						if (len > 13 &&
						    (memcmp(buf, "[GX-ISSUE144]", 13) == 0 ||
						     memcmp(buf, "[INI] ", 6) == 0 ||
						     memcmp(buf, "warn:  D3D8De", 13) == 0))
						{
							return len;
						}

						if (g_iosDiagnosticLogWritten >= kLogCap)
						{
							if (!g_iosDiagnosticLogCapMarked)
							{
								g_iosDiagnosticLogCapMarked = true;
								const char *mark = "[log capped: non-error lines dropped from here]\n";
								write(g_iosDiagnosticLogFd, mark, strlen(mark));
							}
							if (len > 4 &&
							    (memcmp(buf, "err:", 4) == 0 ||
							     memcmp(buf, "ERROR", 5) == 0 ||
							     memcmp(buf, "FATAL", 5) == 0))
							{
								write(g_iosDiagnosticLogFd, buf, (size_t)len);
							}
							return len;
						}

						ssize_t written = write(g_iosDiagnosticLogFd, buf, (size_t)len);
						if (written > 0)
							g_iosDiagnosticLogWritten += (size_t)written;
						return len;
					},
					nullptr, nullptr);

				if (sink != nullptr)
				{
					*stderr = *sink;
					setvbuf(stderr, nullptr, _IOLBF, 0);
				}
			}
		}
	}

	// The engine resolves all game data relative to the working directory.
	// Preferred layout: assets ship read-only INSIDE the signed app bundle
	// (<bundle>/GameData), the iOS-sanctioned home for app resources — the
	// install is then fully self-contained. Dev builds packaged without
	// assets fall back to the Documents folder (Files-app accessible).
	// User data (saves, Options.ini) always lives in Library/Application
	// Support via the engine's user-data path; never in the bundle.
	{
		const char *home = getenv("HOME");

		// <bundle>/GameData, derived from the executable path (argv[0])
		char bundleData[1024] = {0};
		if (argc > 0 && argv[0] != nullptr) {
			const char *slash = strrchr(argv[0], '/');
			if (slash != nullptr) {
				const size_t dirLen = (size_t)(slash - argv[0]);
				if (dirLen < sizeof(bundleData) - 16) {
					memcpy(bundleData, argv[0], dirLen);
					snprintf(bundleData + dirLen, sizeof(bundleData) - dirLen, "/GameData");
				}
			}
		}

		bool usingBundleData = false;
		if (bundleData[0] != '\0' && access(bundleData, R_OK) == 0) {
			if (chdir(bundleData) == 0) {
				usingBundleData = true;
				fprintf(stderr, "INFO: iOS working directory (bundle): %s\n", bundleData);
			}
		}
		if (!usingBundleData && home != nullptr) {
			char docs[1024];
			snprintf(docs, sizeof(docs), "%s/Documents", home);
			if (chdir(docs) != 0) {
				fprintf(stderr, "WARNING: chdir(%s) failed: %s\n", docs, strerror(errno));
			} else {
				fprintf(stderr, "INFO: iOS working directory (Documents): %s\n", docs);
			}
		}

		if (home != nullptr) {
			// Keep DXVK's shader cache in Library/Caches: purgeable under
			// storage pressure, excluded from iCloud backup, invisible in the
			// Files app. Must be set before the d3d8 dylib loads.
			char cacheDir[1024];
			snprintf(cacheDir, sizeof(cacheDir), "%s/Library/Caches", home);
			mkdir(cacheDir, 0755);
			setenv("DXVK_STATE_CACHE_PATH", cacheDir, 0);

			if (usingBundleData) {
				// Seed default settings on first run (full detail instead of the
				// 2003 auto-detect, which drops unknown GPUs to Low).
				char userDataDir[1024], optionsPath[1024];
				snprintf(userDataDir, sizeof(userDataDir),
				         "%s/Library/Application Support/GeneralsX/GeneralsZH", home);
				snprintf(optionsPath, sizeof(optionsPath), "%s/Options.ini", userDataDir);

				// iPad File Sharing bridge: Files/iTunes/Apple Devices can only see
				// Documents, while GeneralsX keeps writable user data in Library.
				// If the user drops Options.ini or SagePatch.ini into Documents,
				// mirror it into the normal user-data directory before GameMain()
				// starts so every existing settings consumer sees it naturally.
				{
					std::error_code dirError;
					std::filesystem::create_directories(userDataDir, dirError);

					char docsDir[1024];
					snprintf(docsDir, sizeof(docsDir), "%s/Documents", home);

					auto syncEditableConfigIfMissing = [&](const char *fileName) {
						char sourcePath[1024], destinationPath[1024];
						snprintf(sourcePath, sizeof(sourcePath), "%s/%s", docsDir, fileName);
						snprintf(destinationPath, sizeof(destinationPath), "%s/%s", userDataDir, fileName);

						// Documents is an import location only. Never overwrite a
						// launcher-saved file in Library/Application Support.
						if (access(sourcePath, R_OK) != 0 || access(destinationPath, F_OK) == 0)
							return;

						std::error_code copyError;
						std::filesystem::copy_file(sourcePath, destinationPath, copyError);
						if (!copyError) {
							fprintf(stderr, "INFO: iPad File Sharing imported %s (destination was missing)\\n", fileName);
						} else {
							fprintf(stderr, "WARNING: failed to import Documents/%s: %s\\n",
							        fileName, copyError.message().c_str());
						}
					};

					syncEditableConfigIfMissing("Options.ini");
					syncEditableConfigIfMissing("SagePatch.ini");
				}

				if (access(optionsPath, F_OK) != 0 && access("DefaultOptions.ini", R_OK) == 0) {
					std::error_code fsError;
					std::filesystem::create_directories(userDataDir, fsError);
					std::filesystem::copy_file("DefaultOptions.ini", optionsPath, fsError);
					if (!fsError) {
						fprintf(stderr, "INFO: Seeded default Options.ini\n");
					}
				}

				// One-time tidy-up: remove asset copies from Documents now that
				// the bundle carries them. Guarded by a sentinel so it truly runs
				// once — Documents is exposed via the Files app, and anything the
				// user places there later (mods, custom maps) must never be touched.
				// "Maps" is deliberately NOT in the list: it is where user maps live.
				char docs[1024];
				snprintf(docs, sizeof(docs), "%s/Documents", home);
				char sentinel[1024];
				snprintf(sentinel, sizeof(sentinel), "%s/.bundle-assets-tidied", docs);
				if (access(sentinel, F_OK) != 0) {
					std::error_code fsError;
					for (const auto &entry : std::filesystem::directory_iterator(docs, fsError)) {
						const std::string name = entry.path().filename().string();
						const bool isShippedAsset =
							(name.size() > 4 && name.compare(name.size() - 4, 4, ".big") == 0) ||
							name == "Data" || name == "Window" || name == "ZH_Generals" ||
							name == "fonts" || name == "_CommonRedist" ||
							name == "dxvk.conf" || name == "GeneralsXZH.dxvk-cache" ||
							name == "GeneralsXZH_d3d9.log";
						if (isShippedAsset) {
							fprintf(stderr, "INFO: tidy-up removing shipped asset copy: %s\n", name.c_str());
							std::error_code removeError;
							std::filesystem::remove_all(entry.path(), removeError);
						}
					}
					if (!fsError) {  // a failed scan must retry next launch, not fail closed forever
						FILE *s = fopen(sentinel, "w");
						if (s) fclose(s);
					}
				}
			}
		}
	}
#endif

	fprintf(stderr, "=================================================\n");
	fprintf(stderr, " Command & Conquer Generals: Zero Hour (Linux)\n");
	fprintf(stderr, " SDL3 + DXVK Build\n");
	fprintf(stderr, "=================================================\n\n");

	try {
		// Initialize critical sections (required by game engine)
		TheAsciiStringCriticalSection = &critSec1;
		TheUnicodeStringCriticalSection = &critSec2;
		TheDmaCriticalSection = &critSec3;
		TheMemoryPoolCriticalSection = &critSec4;
		TheDebugLogCriticalSection = &critSec5;

		// Initialize memory manager early (required by NEW operator)
		initMemoryManager();

		// GeneralsX @bugfix BenderAI 14/02/2026 Initialize Version singleton
		// GameEngine::init() calls updateWindowTitle() which uses TheVersion
		// Must be created before GameMain() to avoid nullptr dereference
		TheVersion = NEW Version;

		// Parse command line (CommandLine class handles argc/argv internally)
		// TheSuperHackers @build felipebraz 10/02/2026 Phase 1.5
		// Store argc/argv for CommandLine parser to access via _NSGetArgc/_NSGetArgv or /proc/self/cmdline
		// For now, let CommandLine::parseCommandLineForStartup() handle this
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		// GeneralsX @feature dvorovrus 25/09/2026 Show the bundled Vite launcher
		// before command-line parsing, then inject the selected profile through
		// the engine's native -mod directory support.
		GeneralsXSetIOSDiagnosticClearCallback(GeneralsXClearIOSDiagnosticLogs);
		const char *selectedProfile = GeneralsXRunIOSProfileLauncher();
		fprintf(stderr, "INFO: iOS launcher selected profile: %s\n",
		        selectedProfile != nullptr ? selectedProfile : "vanilla");
		InjectIOSProfileModArgument(selectedProfile);
#endif
		CommandLine::parseCommandLineForStartup();

		// GeneralsX @bugfix Copilot 17/05/2026 Skip SDL3 window bootstrap for CLI/headless replay execution.
		const bool isHeadlessMode = (TheGlobalData != nullptr && TheGlobalData->m_headless);
		if (isHeadlessMode) {
			fprintf(stderr, "INFO: Headless mode detected, skipping SDL3 video/Vulkan window initialization\n");
		} else {

		// GeneralsX @bugfix felipebraz 16/02/2026
		// Initialize SDL3 and Vulkan BEFORE creating GameEngine (fighter19 pattern)
		// This prevents LLVM SIGSEGV crash during Vulkan driver enumeration
		// Must be done here, not in SDL3GameEngine::init() which is too late
		fprintf(stderr, "INFO: Initializing SDL3 video subsystem...\n");
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		// All mouse events are synthesized by the gesture translator in
		// SDL3GameEngine.cpp; SDL's automatic touch->mouse synthesis would
		// double-deliver finger 1 and fight the two-finger pan logic.
		SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
		if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
			fprintf(stderr, "FATAL: Failed to initialize SDL3: %s\n", SDL_GetError());
			return 1;
		}

		// Set DXVK WSI driver before loading Vulkan
		setenv("DXVK_WSI_DRIVER", "SDL3", 1);

		// GeneralsX @bugfix BenderAI 06/03/2026 - Exclude LLVMpipe Vulkan ICD before loading Vulkan.
		// libvulkan_lvp.so crashes during static initialization with LLVM 20.x when the Vulkan
		// loader enumerates all ICDs. Restrict to hardware ICDs first.
		FilterSoftwareVulkanICDs();
		FilterPipeWireOpenAL();

		// Load Vulkan library for DXVK DirectX8→Vulkan translation
		fprintf(stderr, "INFO: Loading Vulkan library...\n");
		if (!SDL_Vulkan_LoadLibrary(nullptr)) {
			fprintf(stderr, "WARNING: Failed to load Vulkan: %s\n", SDL_GetError());
			fprintf(stderr, "WARNING: Continuing without Vulkan (may use software rendering)\n");
		}

		// Create SDL3 window with Vulkan support
		fprintf(stderr, "INFO: Creating SDL3 Vulkan window...\n");
		Uint32 windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN;  // Start hidden, show after D3D init
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		// Request a native-resolution Metal drawable (e.g. 2868x1320 instead of the
		// 956x440 point size). Without this the swapchain renders at point size and
		// the display upscales 3x, visibly blurring textures and terrain.
		windowFlags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
		TheSDL3Window = SDL_CreateWindow(
			"Command & Conquer Generals: Zero Hour",
			1024, 768,  // Default resolution
			windowFlags
		);

		if (!TheSDL3Window) {
			fprintf(stderr, "FATAL: Failed to create SDL3 window: %s\n", SDL_GetError());
			SDL_Quit();
			return 1;
		}

		// Store window handle globally (cast SDL_Window* to HWND for compatibility)
		ApplicationHWnd = (HWND)TheSDL3Window;
		fprintf(stderr, "INFO: SDL3 window created successfully\n");

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		// Restore the legacy in-game ESC touch control on the actual SDL UIKit window.
		GeneralsXInstallIOSEscOverlay(TheSDL3Window);
#endif

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		// Match the game's internal resolution to the phone screen's aspect ratio.
		// Without this the engine runs its 4:3 default inside the 19.5:9 display:
		// pillarboxed picture and a skewed window->game coordinate mapping. Height
		// stays at the engine's 600px design baseline (UI layouts assume >= 600);
		// width follows the real aspect. Injected as -xres/-yres argv entries so
		// the normal command-line path applies them (user-passed flags still win
		// because the parser lets later arguments override earlier ones... ours go
		// last, so only add them if the user didn't pass explicit -xres/-yres).
		{
			bool userSetRes = false;
			for (int i = 1; i < __argc; ++i) {
				if (strcmp(__argv[i], "-xres") == 0 || strcmp(__argv[i], "-yres") == 0) {
					userSetRes = true;
					break;
				}
			}
			// Use the pixel size of the high-density drawable: the game renders
			// 1:1 into the native-resolution swapchain, and fonts/UI rescale via
			// the engine's resolution-aware font scaling (GlobalLanguage).
			int winW = 0, winH = 0;
			SDL_GetWindowSizeInPixels(TheSDL3Window, &winW, &winH);
			if (!userSetRes && winW > 0 && winH > 0 && winW > winH) {
				static char xresVal[16], yresVal[16];
				static char xresFlag[] = "-xres";
				static char yresFlag[] = "-yres";
				const int yres = winH;
				int xres = winW;
				xres &= ~1;  // keep it even
				snprintf(xresVal, sizeof(xresVal), "%d", xres);
				snprintf(yresVal, sizeof(yresVal), "%d", yres);

				static char* newArgv[64];
				int n = 0;
				for (int i = 0; i < __argc && n < 59; ++i) {
					newArgv[n++] = __argv[i];
				}
				newArgv[n++] = xresFlag;
				newArgv[n++] = xresVal;
				newArgv[n++] = yresFlag;
				newArgv[n++] = yresVal;
				newArgv[n] = nullptr;
				__argv = newArgv;
				__argc = n;
				fprintf(stderr, "INFO: iOS internal resolution set to %sx%s (window %dx%d)\n",
				        xresVal, yresVal, winW, winH);
			}
		}
#endif
		}

		// Call cross-platform game entry point
		exitcode = GameMain();

		fprintf(stderr, "INFO: GameMain() returned with code %d\n", exitcode);

	} catch (const std::exception& e) {
		fprintf(stderr, "FATAL: Unhandled exception in main(): %s\n", e.what());
		exitcode = 1;
	} catch (...) {
		fprintf(stderr, "FATAL: Unknown exception in main()\n");
		exitcode = 1;
	}

	// Cleanup SDL3 resources
	if (TheSDL3Window) {
		#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		GeneralsXRemoveIOSEscOverlay();
#endif
		SDL_DestroyWindow(TheSDL3Window);
		TheSDL3Window = nullptr;
		ApplicationHWnd = nullptr;
	}
	SDL_Quit();

	// GeneralsX @bugfix BenderAI 14/02/2026 Cleanup Version singleton
	if (TheVersion) {
		delete TheVersion;
		TheVersion = nullptr;
	}

	// GeneralsX @bugfix BenderAI 19/02/2026 Shutdown memory manager BEFORE nulling critical
	// sections. Without this, global pool destructors (ObjectPoolClass) crash during atexit()
	// because they call ::operator delete after the memory manager is already gone (SIGSEGV).
	// Matches WinMain.cpp cleanup order: TheVersion -> shutdownMemoryManager -> null critSecs.
	shutdownMemoryManager();

	// Cleanup critical sections (after memory manager, which may use them during shutdown)
	TheAsciiStringCriticalSection = nullptr;
	TheUnicodeStringCriticalSection = nullptr;
	TheDmaCriticalSection = nullptr;
	TheMemoryPoolCriticalSection = nullptr;
	TheDebugLogCriticalSection = nullptr;

	fprintf(stderr, "\nExiting with code %d\n", exitcode);

	// GeneralsX @bugfix BenderAI 25/02/2026 — use _exit() to skip C++ global destructors.
	// On macOS, __cxa_finalize_ranges runs ObjectPoolClass<X,256> global dtors after main() returns.
	// Those dtors crash with a corrupted BlockListHead (SIGSEGV at 0x4ade32ec4ade0018) because
	// pool block memory was already reused/overwritten during game shutdown.
	// Windows never had this problem — ExitProcess() terminates without running C++ global dtors.
	// _exit() matches that behavior. Explicit cleanup already done above (SDL_Quit, shutdownMemoryManager).
	_exit(exitcode);
}

#endif // !_WIN32
