# The app's native part: Lube (its sources, with its plugins inside it) and main.c.
cmake_minimum_required(VERSION 3.22)
project(lube_app C)

# Lube's src folder (this template's, in Lube's repository; elsewhere: -DLUBE_SRC=/path/to/lube/src)
set(LUBE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../src" CACHE PATH "Lube's src folder")
file(GLOB LUBE_CORE "${LUBE_SRC}/*.c")
list(FILTER LUBE_CORE EXCLUDE REGEX "/main\\.c$")  # (the lube command's)
file(GLOB LUBE_PLUGINS "${LUBE_SRC}/plugins/*.c")  # (all of Lube's plugins: inside the app)

add_library(native_app_glue STATIC "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC "${ANDROID_NDK}/sources/android/native_app_glue")

add_library(lube_app SHARED main.c ${LUBE_CORE} ${LUBE_PLUGINS})
set_target_properties(lube_app PROPERTIES C_STANDARD 99)
target_compile_definitions(lube_app PRIVATE LUBE_STATIC_PLUGINS)
target_include_directories(lube_app PRIVATE "${LUBE_SRC}")
# (the glue's entry point, which nothing in the library calls: kept)
target_link_options(lube_app PRIVATE "-u" "ANativeActivity_onCreate")
target_link_libraries(lube_app native_app_glue android log m dl)
