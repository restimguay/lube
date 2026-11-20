# A Lube program as an iOS app: Lube (its sources, with its plugins inside it), main.m, and the program's
# files (mobile/game/). Make an Xcode project, then build and run it from Xcode:
#
#   cmake -G Xcode -S mobile/ios -B build/ios -DCMAKE_SYSTEM_NAME=iOS
#   open build/ios/LubeApp.xcodeproj
#
# (A phone needs a signing team: set it in Xcode, or add -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=...)
cmake_minimum_required(VERSION 3.20)
set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "the oldest iOS it runs on")
project(LubeApp C OBJC)

# Lube's src folder (this template's, in Lube's repository; elsewhere: -DLUBE_SRC=/path/to/lube/src)
set(LUBE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../src" CACHE PATH "Lube's src folder")
# The program: main.lube and the files it reads, copied into the app (beside each other)
set(LUBE_GAME "${CMAKE_CURRENT_SOURCE_DIR}/../game" CACHE PATH "the program's folder")

file(GLOB LUBE_CORE "${LUBE_SRC}/*.c")
list(FILTER LUBE_CORE EXCLUDE REGEX "/main\\.c$")  # (the lube command's)
file(GLOB LUBE_PLUGINS "${LUBE_SRC}/plugins/*.c")  # (fs, net, audio, gfx: inside the app)
file(GLOB GAME_FILES "${LUBE_GAME}/*")

add_executable(LubeApp MACOSX_BUNDLE main.m ${LUBE_CORE} ${LUBE_PLUGINS} ${GAME_FILES})
set_target_properties(LubeApp PROPERTIES
    C_STANDARD 99
    RESOURCE "${GAME_FILES}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.lube.app"  # (your own, for an app of your own)
    MACOSX_BUNDLE_BUNDLE_NAME "Lube"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    MACOSX_BUNDLE_BUNDLE_VERSION "1"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "dev.lube.app"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"  # iPhone and iPad
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic")
target_compile_definitions(LubeApp PRIVATE LUBE_STATIC_PLUGINS)
target_include_directories(LubeApp PRIVATE "${LUBE_SRC}")
target_compile_options(LubeApp PRIVATE $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
target_link_libraries(LubeApp PRIVATE "-framework UIKit" "-framework QuartzCore" "-framework CoreGraphics" "-framework Foundation")
