# iOS device (arm64) toolchain for openpete. Pointed at by the `ios-arm64`
# preset in CMakePresets.json. Needs macOS with Xcode.
#
# CMAKE_SYSTEM_NAME must be set before project() runs the compiler checks, so
# it lives here rather than in cmake/presets/ios-arm64.cmake (which is included
# at the end of project()).
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_OSX_SYSROOT iphoneos)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_DEPLOYMENT_TARGET 16.0 CACHE STRING "Minimum iOS version")

# CI builds are unsigned; signing happens when the .ipa is packaged.
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED NO)
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "")
