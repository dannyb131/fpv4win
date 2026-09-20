cmake_minimum_required(VERSION 3.20)
project(fpv4win)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(FFmpeg REQUIRED)
find_package(SDL2 REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h PATH_SUFFIXES libusb-1.0 REQUIRED)
find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0 REQUIRED)
include_directories(${FFMPEG_INCLUDE_DIRS})

find_package(Qt5 COMPONENTS Quick Multimedia  REQUIRED)

# The original bundled RTL8812 receive-only driver cannot inject uplink
# frames. Build the current OpenIPC userspace driver for the RTL8812AU family
# only; it supports concurrent RX/TX on Windows through libusb.
set(DEVOURER_JAGUAR1 ON CACHE BOOL "" FORCE)
set(DEVOURER_8814 OFF CACHE BOOL "" FORCE)
set(DEVOURER_JAGUAR2_8822B OFF CACHE BOOL "" FORCE)
set(DEVOURER_JAGUAR2_8821C OFF CACHE BOOL "" FORCE)
set(DEVOURER_JAGUAR3_8822C OFF CACHE BOOL "" FORCE)
set(DEVOURER_JAGUAR3_8822E OFF CACHE BOOL "" FORCE)
set(DEVOURER_8733B OFF CACHE BOOL "" FORCE)
set(DEVOURER_KESTREL_8852B OFF CACHE BOOL "" FORCE)
set(DEVOURER_KESTREL_8852C OFF CACHE BOOL "" FORCE)
set(DEVOURER_MT7612U OFF CACHE BOOL "" FORCE)
set(DEVOURER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(3rd/devourer EXCLUDE_FROM_ALL)


if(CMAKE_BUILD_TYPE MATCHES "Debug")
    message("build debug")
    add_definitions(-DDEBUG_MODE)
endif()


file(GLOB_RECURSE SRC_LIST
        src/*.cpp
        src/*.c
        src/*.h
)
list(FILTER SRC_LIST EXCLUDE REGEX ".*/3rd.*/src/main.cpp")

include_directories(src)
add_executable(${PROJECT_NAME} ${SRC_LIST} qml.qrc)

target_link_libraries(${PROJECT_NAME}
        Qt5::Quick ${FFMPEG_LIBRARIES}
        ${SDL2_LIBRARIES}
        ${LIBUSB_LIBRARY} devourer
        unofficial-sodium::sodium)

target_include_directories(${PROJECT_NAME} PRIVATE ${LIBUSB_INCLUDE_DIR})

if (WIN32 AND DEFINED VCPKG_INSTALLED_DIR)
    # vcpkg's app-local deployment only copies QML imports when the output
    # qml directory does not already exist. Refresh it on every build so newly
    # installed modules such as QtQuick.Controls and Qt.labs.platform ship too.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/qml"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/qml")
endif ()

if (WIN32 AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(DEBUG_SUFFIX)
    if (MSVC AND CMAKE_BUILD_TYPE MATCHES "Debug")
        set(DEBUG_SUFFIX "d")
    endif ()
    set(QT_INSTALL_PATH "${CMAKE_PREFIX_PATH}")
    if (NOT EXISTS "${QT_INSTALL_PATH}/bin")
        set(QT_INSTALL_PATH "${QT_INSTALL_PATH}/..")
        if (NOT EXISTS "${QT_INSTALL_PATH}/bin")
            set(QT_INSTALL_PATH "${QT_INSTALL_PATH}/..")
        endif ()
    endif ()
    if (EXISTS "${QT_INSTALL_PATH}/plugins/platforms/qwindows${DEBUG_SUFFIX}.dll")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/plugins/platforms/")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy
                "${QT_INSTALL_PATH}/plugins/platforms/qwindows${DEBUG_SUFFIX}.dll"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/plugins/platforms/")
    endif ()
    foreach (QT_LIB Core)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy
                "${QT_INSTALL_PATH}/bin/Qt5${QT_LIB}${DEBUG_SUFFIX}.dll"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
    endforeach (QT_LIB)
endif ()

include(CTest)
if (BUILD_TESTING)
    add_executable(wfb_roundtrip_test
            tests/WFBTransmitterRoundTrip.cpp
            src/wifi/WFBTransmitter.cpp
            src/wifi/WFBProcessor.cpp
            src/wifi/fec.c)
    target_include_directories(wfb_roundtrip_test PRIVATE src src/wifi)
    target_link_libraries(wfb_roundtrip_test PRIVATE Qt5::Core unofficial-sodium::sodium)
    add_test(NAME wfb_roundtrip COMMAND wfb_roundtrip_test)
endif ()
