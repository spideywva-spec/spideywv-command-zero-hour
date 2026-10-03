#!/bin/bash
# Build an unsigned iOS shell containing only the native GeneralsXZH engine
# and runtime frameworks. Retail GameData and mod profiles are injected later
# on Windows by build-all-in-one.ps1.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build/ios-vulkan"
IOS_DIR="${PROJECT_ROOT}/ios"
DERIVED="${IOS_DIR}/build"
OUT_DIR="${PROJECT_ROOT}/build/ios-package"
APP_NAME="GeneralsXZH"
BUNDLE_ID="${GX_BUNDLE_ID:-com.dvorov.generalszh.launcher}"
VERSION_FILE="${IOS_DIR}/version.env"

PROJECT_VERSION="0.0.0"
ENGINE_VERSION="0.0.0"
LAUNCHER_VERSION="0.0.0"
if [[ -f "${VERSION_FILE}" ]]; then
  # shellcheck disable=SC1090
  source "${VERSION_FILE}"
fi

GAME_BIN="${BUILD_DIR}/GeneralsMD/GeneralsXZH.app/GeneralsXZH"
DXVK_BUILD="${BUILD_DIR}/_deps/dxvk-build-macos"
GAMESPY_LIB="$(find "${BUILD_DIR}" -type f -name 'libgamespy.dylib' -print -quit)"
LAUNCHER_LIB="$(find "${BUILD_DIR}" -type f -name 'libGeneralsXLauncher.dylib' -print -quit)"

test -f "${GAME_BIN}" || {
  echo "ERROR: missing engine binary: ${GAME_BIN}"
  exit 1
}

test -n "${GAMESPY_LIB}" && test -f "${GAMESPY_LIB}" || {
  echo "ERROR: missing GameSpy runtime library (libgamespy.dylib) under ${BUILD_DIR}"
  exit 1
}

test -n "${LAUNCHER_LIB}" && test -f "${LAUNCHER_LIB}" || {
  echo "ERROR: missing native launcher runtime library (libGeneralsXLauncher.dylib) under ${BUILD_DIR}"
  exit 1
}

echo "==> Generating unsigned iOS shell"
(cd "${IOS_DIR}" && xcodegen generate --quiet)

xcodebuild -project "${IOS_DIR}/${APP_NAME}.xcodeproj" \
  -scheme "${APP_NAME}" -configuration Release \
  -destination 'generic/platform=iOS' \
  -derivedDataPath "${DERIVED}" \
  PRODUCT_BUNDLE_IDENTIFIER="${BUNDLE_ID}" \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO \
  build | tail -3

SHELL_APP="${DERIVED}/Build/Products/Release-iphoneos/${APP_NAME}.app"
test -d "${SHELL_APP}" || {
  echo "ERROR: Xcode shell app not produced: ${SHELL_APP}"
  exit 1
}

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"
cp -R "${SHELL_APP}" "${OUT_DIR}/"
APP="${OUT_DIR}/${APP_NAME}.app"

# Embed the launcher intro video directly into the final app bundle.
# IOSProfileLauncher.mm resolves it through [NSBundle mainBundle].
INTRO_VIDEO="${PROJECT_ROOT}/resources/intro.mp4"
test -f "${INTRO_VIDEO}" || {
  echo "ERROR: missing launcher video: ${INTRO_VIDEO}"
  exit 1
}
cp "${INTRO_VIDEO}" "${APP}/intro.mp4"
echo "OK: embedded launcher video: ${APP}/intro.mp4"


PLIST="${APP}/Info.plist"
if [[ -f "${PLIST}" ]]; then
  /usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString ${PROJECT_VERSION}" "${PLIST}"
fi

cp "${GAME_BIN}" "${APP}/${APP_NAME}"
mkdir -p "${APP}/Frameworks"

for lib in \
  "${DXVK_BUILD}/src/d3d8/libdxvk_d3d8.0.dylib" \
  "${DXVK_BUILD}/src/d3d9/libdxvk_d3d9.0.dylib" \
  "${BUILD_DIR}/_deps/sdl3-build/libSDL3.0.dylib" \
  "${BUILD_DIR}/_deps/sdl3_image-build/libSDL3_image.0.dylib" \
  "${BUILD_DIR}/_deps/openal_soft-build/libopenal.1.24.2.dylib" \
  "${GAMESPY_LIB}" \
  "${LAUNCHER_LIB}"; do
  test -f "${lib}" || {
    echo "ERROR: required runtime library missing: ${lib}"
    exit 1
  }
  cp "${lib}" "${APP}/Frameworks/"
done

if [[ -f "${APP}/Frameworks/libopenal.1.24.2.dylib" ]]; then
  mv "${APP}/Frameworks/libopenal.1.24.2.dylib" "${APP}/Frameworks/libopenal.1.dylib"
fi

MVK_FRAMEWORK="${GX_MOLTENVK:-${HOME}/GeneralsX/MoltenVK/MoltenVK/MoltenVK/dynamic/MoltenVK.xcframework/ios-arm64/MoltenVK.framework}"
test -d "${MVK_FRAMEWORK}" || {
  echo "ERROR: MoltenVK.framework missing: ${MVK_FRAMEWORK}"
  exit 1
}
cp -R "${MVK_FRAMEWORK}" "${APP}/Frameworks/"

ICON_SRC="${IOS_DIR}/Stub/Assets.xcassets/AppIcon.appiconset/icon.png"
if [[ -f "${ICON_SRC}" ]]; then
  sips -z 120 120 "${ICON_SRC}" --out "${APP}/AppIcon60x60@2x.png" >/dev/null
  sips -z 152 152 "${ICON_SRC}" --out "${APP}/AppIcon76x76@2x.png" >/dev/null
  sips -z 167 167 "${ICON_SRC}" --out "${APP}/AppIcon83.5x83.5@2x.png" >/dev/null
fi

install_name_tool -add_rpath "@executable_path/Frameworks" "${APP}/${APP_NAME}" 2>/dev/null || true

# Fail packaging if the executable references an @rpath runtime that was not
# embedded. Missing dylibs are fatal before main(), so our in-app stderr log
# cannot report them on device.
echo "==> Auditing Mach-O runtime dependencies"
while IFS= read -r dependency; do
  [[ -n "${dependency}" ]] || continue
  relative="${dependency#@rpath/}"
  candidate="${APP}/Frameworks/${relative}"
  if [[ ! -e "${candidate}" ]]; then
    echo "ERROR: missing embedded runtime dependency: ${dependency}"
    echo "       expected: ${candidate}"
    exit 1
  fi
  echo "OK: ${dependency}"
done < <(otool -L "${APP}/${APP_NAME}" | awk '$1 ~ /^@rpath\// { print $1 }')

find "${APP}" -name "_CodeSignature" -type d -prune -exec rm -rf {} + 2>/dev/null || true
rm -f "${APP}/embedded.mobileprovision"

IPA_STAGE="${OUT_DIR}/ipa-stage"
IPA_PATH="${OUT_DIR}/GeneralsXZH-launcher-unsigned.ipa"
rm -rf "${IPA_STAGE}" "${IPA_PATH}"
mkdir -p "${IPA_STAGE}/Payload"
cp -R "${APP}" "${IPA_STAGE}/Payload/"
(cd "${IPA_STAGE}" && /usr/bin/zip -qry "${IPA_PATH}" Payload)

test -f "${IPA_PATH}"
echo "==> Unsigned native shell ready: ${IPA_PATH}"
du -h "${IPA_PATH}"
