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
    # suppress macOS ASAN malloc-leak detection (not available without malloc(3) interpose)
    if(APPLE)
        set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DASAN_OPTIONS=detect_leaks=0")
    endif()
endif()

# ── Submodule check (informative error if not initialized) ──
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/deps/epoll/include/epoll/epoll.h" OR
   NOT EXISTS "${CMAKE_SOURCE_DIR}/deps/ccalg/include/ccheap.h" OR
   NOT EXISTS "${CMAKE_SOURCE_DIR}/deps/ccsocket/ccsocket.h")
    message(FATAL_ERROR
        "Git submodules not initialized.\n"
        "  Run:  git submodule update --init --recursive\n"
        "  Or:   scripts/init-deps.sh (POSIX) / init-deps.cmd (Windows)")
endif()

# ── Dependencies: epoll (cross-platform epoll ABI) ──
add_subdirectory(deps/epoll)

# ── Dependencies: ccsocket ──
add_library(ccsocket STATIC
    deps/ccsocket/ccsocket.c
    deps/ccsocket/ccdns.c
    deps/ccsocket/ccicmp.c
)
target_include_directories(ccsocket PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/deps/ccsocket>)
target_compile_definitions(ccsocket PRIVATE
    CCICMP_MAX_PAYLOAD=65500
    CCICMP_RECV_BUFSZ=65535)
if(WIN32)
    target_link_libraries(ccsocket PUBLIC ws2_32)
endif()

# ── ccalg (header-only, no compile) ──
add_library(ccalg INTERFACE)
target_include_directories(ccalg INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/deps/ccalg/include>)

# ── ccev library ──
set(CCEV_SOURCES
    src/ccev.c
    src/ccev_timer.c
    src/ccev_conn.c
    src/ccev_dns.c
    src/ccev_icmp.c
    src/ccev_signal.c
)
add_library(ccev STATIC ${CCEV_SOURCES})
target_include_directories(ccev PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(ccev PRIVATE deps/ccalg/include deps/epoll/include deps/ccsocket)
target_link_libraries(ccev PUBLIC epoll_static ccsocket ccalg)

# Linux timerfd requires _GNU_SOURCE for CLOCK_MONOTONIC, TFD_NONBLOCK
if(CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
    target_compile_definitions(ccev PRIVATE _GNU_SOURCE)
endif()

# MSVC: C11 mode for proper bool support + suppress deprecation warnings
if(MSVC)
    target_compile_options(ccev PRIVATE /std:c11 /wd4996)
    target_compile_definitions(ccev PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Suppress SOCKET redefinition warning (epoll.h and ccsocket.h both define it compatibly)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ccev PRIVATE -Wno-typedef-redefinition)
endif()

# ── Install (PUBLIC headers use $<INSTALL_INTERFACE> for relocatability) ──
include(GNUInstallDirs)
install(TARGETS ccev epoll epoll_static ccsocket ccalg
    EXPORT ccev-targets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES src/ccev.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
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
