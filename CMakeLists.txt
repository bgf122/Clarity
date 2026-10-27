cmake_minimum_required(VERSION 3.20)
project(Clarity VERSION 0.1.14 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Anarchy Online is 32-bit: configure with -A Win32")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: no runtime DLL dependency in the client folder.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(DEPLOY_DIR "" CACHE PATH "Client folder the built dinput.dll is copied to (empty = no copy)")

set(MH ${CMAKE_SOURCE_DIR}/third_party/minhook)
add_library(minhook STATIC
    ${MH}/src/buffer.c
    ${MH}/src/hook.c
    ${MH}/src/trampoline.c
    ${MH}/src/hde/hde32.c
)
target_include_directories(minhook PUBLIC ${MH}/include)

# Version header and VERSIONINFO resource (ProductName "Clarity", read by the AoForge installer).
configure_file(src/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h)
configure_file(src/clarity.rc.in ${CMAKE_BINARY_DIR}/generated/clarity.rc)

add_library(clarity SHARED
    src/main.cpp
    src/proxy/dinput_proxy.cpp
    src/proxy/dinput.def
    src/core/config.cpp
    src/core/game.cpp
    src/core/hooks.cpp
    src/core/log.cpp
    src/core/n3.cpp
    src/core/text_raster.cpp
    src/core/threat.cpp
    src/core/wndproc.cpp
    src/features/fct/fct.cpp
    src/features/fct/atlas.cpp
    src/features/settings/settings.cpp
    src/features/nameplates/nameplates.cpp
    src/features/hideui/hideui.cpp
    src/features/bars/bars.cpp
    ${CMAKE_BINARY_DIR}/generated/clarity.rc
)
set_target_properties(clarity PROPERTIES OUTPUT_NAME dinput)
target_compile_definitions(clarity PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(clarity PRIVATE /W4 /permissive- /utf-8)
target_include_directories(clarity PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(clarity PRIVATE minhook user32)

if(DEPLOY_DIR)
    add_custom_command(TARGET clarity POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:clarity> "${DEPLOY_DIR}/dinput.dll"
        COMMENT "Deploying dinput.dll to ${DEPLOY_DIR}")
endif()
