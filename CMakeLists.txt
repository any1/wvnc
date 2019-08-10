cmake_minimum_required (VERSION 3.9)
project (wvnc)

set (CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (${CMAKE_BUILD_TYPE} MATCHES "Release")
	set (CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
find_package (Wayland REQUIRED)
find_package (WaylandScanner REQUIRED)
find_package (PkgConfig REQUIRED)
pkg_search_module (PIXMAN REQUIRED pixman-1)
pkg_search_module (LIBVNCSERVER REQUIRED libvncserver)
pkg_search_module (XKBCOMMON REQUIRED xkbcommon)
pkg_search_module (LIBUV REQUIRED libuv)

option (WITH_ASAN "Enable ASan" OFF)

if (WITH_ASAN)
	set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
endif ()

ecm_add_wayland_client_protocol (
	WLR_SCREENCOPY_SRC
	PROTOCOL wlr-protocols/unstable/wlr-screencopy-unstable-v1.xml
	BASENAME wlr-screencopy
)

ecm_add_wayland_client_protocol (
	XDG_OUTPUT_SRC
	PROTOCOL wayland-protocols/unstable/xdg-output/xdg-output-unstable-v1.xml
	BASENAME xdg-output
)

ecm_add_wayland_client_protocol (
	VIRTUAL_KEYBOARD_SRC
	PROTOCOL virtual-keyboard-unstable-v1.xml
	BASENAME virtual-keyboard
)

set (CMAKE_C_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ggdb -O0")
set (CMAKE_C_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas -Wno-unused")

include_directories ("${CMAKE_BINARY_DIR}")
include_directories (${LIBVNCSERVER_INCLUDE_DIRS})
include_directories (${PIXMAN_INCLUDE_DIRS})
include_directories (${XKBCOMMON_INCLUDE_DIRS})
include_directories (${LIBUV_INCLUDE_DIRS})

add_executable (wvnc main.c buffer.c utils.c uinput.c damage.c ${VIRTUAL_KEYBOARD_SRC}
	${WLR_SCREENCOPY_SRC} ${XDG_OUTPUT_SRC})
target_link_libraries (wvnc rt m ${Wayland_LIBRARIES} ${LIBVNCSERVER_LIBRARIES}
	${XKBCOMMON_LIBRARIES} ${PIXMAN_LIBRARIES} ${LIBUV_LIBRARIES})

install (TARGETS wvnc RUNTIME DESTINATION bin COMPONENT bin)
