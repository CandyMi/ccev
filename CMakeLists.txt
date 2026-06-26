cmake_minimum_required(VERSION 3.10)
project(ccev VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Options ──
option(BUILD_TESTING "Build tests" ON)
option(BUILD_EXAMPLES "Build examples" ON)
option(CCEV_SANITIZE "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# ── Sanitizer setup (ASAN + UBSAN) ────────────────────────────
if(CCEV_SANITIZE)
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fsanitize=address,undefined -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=address,undefined")
    if(APPLE)
        set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DASAN_OPTIONS=detect_leaks=0")
    endif()
endif()

# ── Submodule check ──
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/deps/epoll/include/epoll/epoll.h" OR
   NOT EXISTS "${CMAKE_SOURCE_DIR}/deps/ccalg/include/ccheap.h" OR
   NOT EXISTS "${CMAKE_SOURCE_DIR}/deps/ccsocket/ccsocket.h")
    message(FATAL_ERROR
        "Git submodules not initialized.\n"
        "  Run:  git submodule update --init --recursive\n"
        "  Or:   scripts/init-deps.sh (POSIX) / init-deps.cmd (Windows)")
endif()

# ── epoll backend source ────────────────────────────────────
if(WIN32)
    set(EPOLL_SRC deps/epoll/src/wepoll.c)
elseif(APPLE)
    set(EPOLL_SRC deps/epoll/src/kepoll.c)
elseif(UNIX)
    set(EPOLL_SRC deps/epoll/src/epoll.c)
else()
    set(EPOLL_SRC deps/epoll/src/uepoll.c)
endif()

# ── ccev library (single .a — everything merged) ─────────────
set(CCEV_SOURCES
    src/ccev.c
    src/ccev_timer.c
    src/ccev_conn.c
    src/ccev_stream.c
    src/ccev_dns.c
    src/ccev_icmp.c
    src/ccev_signal.c
    deps/ccsocket/ccsocket.c
    deps/ccsocket/ccdns.c
    deps/ccsocket/ccicmp.c
    ${EPOLL_SRC}
)
add_library(ccev STATIC ${CCEV_SOURCES})

target_include_directories(ccev PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/deps/ccsocket>
)
target_include_directories(ccev PRIVATE
    deps/ccalg/include
    deps/epoll/include/epoll
    deps/epoll/include
)

# ccsocket compile definitions
target_compile_definitions(ccev PRIVATE
    CCICMP_MAX_PAYLOAD=65500
    CCICMP_RECV_BUFSZ=65535
)

if(WIN32)
    target_link_libraries(ccev PUBLIC ws2_32)
endif()

# Linux timerfd
if(CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
    target_compile_definitions(ccev PRIVATE _GNU_SOURCE)
endif()

# MSVC
if(MSVC)
    target_compile_options(ccev PRIVATE /std:c11 /wd4996)
    target_compile_definitions(ccev PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Suppress SOCKET redefinition (epoll.h and ccsocket.h both define it)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ccev PRIVATE -Wno-typedef-redefinition)
endif()

# ── Install ─────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS ccev
    EXPORT ccev-targets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES
    src/ccev.h
    deps/ccsocket/ccsocket.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(EXPORT ccev-targets
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ccev
    NAMESPACE ccev::
)

# ── Tests ──
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Examples ──
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
