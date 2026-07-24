cmake_minimum_required(VERSION 3.10)
project(ccev LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

#  ccache Supported
find_program(CCACHE_FOUND ccache)
if(CCACHE_FOUND)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache)
endif()

# ── C11 stdatomic.h detection — upgrade to C11 when available ──
# Use check_include_file (compiles a test source) rather than find_file
# (just checks disk) because MSVC ships a stub that #errors out.
include(CheckIncludeFile)
set(CMAKE_REQUIRED_FLAGS ${CMAKE_C11_STANDARD_COMPILE_OPTION})
check_include_file(stdatomic.h _have_atomic)
set(CMAKE_REQUIRED_FLAGS)
if(_have_atomic)
    set(CMAKE_C_STANDARD 11)
endif()

# ── Options ──
option(BUILD_TESTING  "Build tests"    ON)
option(BUILD_EXAMPLES "Build examples" ON)
option(CCEV_SANITIZE  "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(CCEV_TLS       "Build TLS support (requires OpenSSL)" OFF)

# ── Submodule check ──
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/deps/epoll/include/epoll/epoll.h" OR
   NOT EXISTS "${CMAKE_SOURCE_DIR}/deps/ccalg/include/ccheap.h" OR
   NOT EXISTS "${CMAKE_SOURCE_DIR}/deps/ccsocket/include/ccsocket.h")
    message(FATAL_ERROR
        "Git submodules not initialized.\n"
        "  Run:  git submodule update --init --recursive\n"
        "  Or:   scripts/init-deps.sh (POSIX) / init-deps.cmd (Windows)")
endif()

# ── Object library: epoll backend (1 per platform) ──────────
# kqueue: Apple, FreeBSD, DragonFly BSD, NetBSD, OpenBSD
#   epoll: Linux, Android
# wepoll: Windows
# uepoll: other POSIX (select/poll fallback)
if(WIN32)
    set(EPOLL_BACKEND deps/epoll/src/wepoll.c)
elseif(APPLE OR CMAKE_SYSTEM_NAME MATCHES "BSD|DragonFly")
    set(EPOLL_BACKEND deps/epoll/src/kepoll.c)
elseif(CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
    set(EPOLL_BACKEND deps/epoll/src/epoll.c)
else()
    set(EPOLL_BACKEND deps/epoll/src/uepoll.c)
endif()

add_library(ccev_epoll OBJECT ${EPOLL_BACKEND})
set_target_properties(ccev_epoll PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(ccev_epoll PRIVATE
    deps/epoll/include/epoll
    deps/epoll/include
)
# The kepoll and uepoll backends use atomic spinlocks.
# EPOLL_USE_ATOMIC / CCEV_USE_ATOMIC selects C11 <stdatomic.h> when available.
# if(_have_atomic)
#     target_compile_definitions(ccev_epoll PUBLIC EPOLL_USE_ATOMIC=1)
# endif()
target_compile_definitions(ccev_epoll PUBLIC EPOLL_NO_THREAD=1)

# EPOLL_USE_SHARED_WORKBUF — shared buffer on struct epoll_t (kepoll/pepoll).
# 0 (default): per-call stack + heap fallback, thread-safe.
# 1:           2^n lazy-realloc shared buffer, zero alloc after growth.
#               NOT safe for concurrent epoll_wait on same handle — ccev
#               is single-threaded per loop, so this is always safe.
target_compile_definitions(ccev_epoll PRIVATE EPOLL_USE_SHARED_WORKBUF=1)

# ── Object library: ccsocket (cross-platform socket API) ────
add_library(ccev_ccsocket OBJECT
    deps/ccsocket/src/ccsocket.c
    deps/ccsocket/src/ccdns.c
    deps/ccsocket/src/ccicmp.c
)
set_target_properties(ccev_ccsocket PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(ccev_ccsocket PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/deps/ccsocket/include>
)
target_compile_definitions(ccev_ccsocket PRIVATE
    CCICMP_MAX_PAYLOAD=65500
    CCICMP_RECV_BUFSZ=65535
)

# ── Static library: ccev ────────────────────────────────────
set(CCEV_SOURCES
    src/ccev.c
    src/ccev_mem.c
    src/ccev_timer.c
    src/ccev_sock.c
    src/ccev_stream.c
    src/ccev_dns.c
    src/ccev_icmp.c
    src/ccev_signal.c
    src/ccev_poll.c
)

if(CCEV_TLS)
    find_package(OpenSSL 1.1.1 QUIET)
    if(OpenSSL_FOUND)
        list(APPEND CCEV_SOURCES src/ccev_tls.c src/ccev_tls_ctx.c)
    else()
        message(STATUS "OpenSSL not found — TLS support disabled (CCEV_TLS=ON ignored)")
    endif()
endif()

add_library(ccev STATIC
    ${CCEV_SOURCES}
    $<TARGET_OBJECTS:ccev_epoll>
    $<TARGET_OBJECTS:ccev_ccsocket>
)

if(CCEV_TLS AND OpenSSL_FOUND)
    target_compile_definitions(ccev PRIVATE CCEV_HAVE_TLS=1)
endif()

set_target_properties(ccev PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(ccev PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/deps/ccsocket/include>
)
target_include_directories(ccev PRIVATE
    deps/ccalg/include
    deps/epoll/include/epoll
    deps/epoll/include
)
if(_have_atomic)
    target_compile_definitions(ccev PRIVATE CCEV_USE_ATOMIC=1)
endif()
unset(_have_atomic)

# ── Platform-specific ────────────────────────────────────────
if(CCEV_TLS AND OpenSSL_FOUND)
    if(TARGET OpenSSL::SSL_Static)
        target_link_libraries(ccev PUBLIC OpenSSL::SSL_Static OpenSSL::Crypto_Static)
    else()
        target_link_libraries(ccev PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    endif()
endif()

if(WIN32)
    target_link_libraries(ccev PUBLIC ws2_32)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
    target_link_libraries(ccev PUBLIC socket nsl sendfile)
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
    target_compile_definitions(ccev PRIVATE _GNU_SOURCE)
endif()

# ── Compiler-specific ────────────────────────────────────────
if(MSVC)
    target_compile_options(ccev PRIVATE /std:c11 /wd4996)
    target_compile_definitions(ccev PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ccev PRIVATE -Wno-typedef-redefinition)
endif()

# ── ThreadSanitizer (debug builds only, GCC/Clang, incompatible with CCEV_SANITIZE) ──
option(CCEV_TSAN "Enable ThreadSanitizer (replaces ASAN+UBSAN)" OFF)

# ── Sanitizer (debug builds only, GCC/Clang on POSIX) ──────
if(CCEV_SANITIZE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        if(NOT WIN32)
            target_compile_options(ccev PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            set(CMAKE_EXE_LINKER_FLAGS_DEBUG
                "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=address,undefined")
            if(APPLE)
                target_compile_definitions(ccev PRIVATE ASAN_OPTIONS=detect_leaks=0)
            endif()
        else()
            target_compile_options(ccev PRIVATE -fsanitize=address -fno-omit-frame-pointer)
            set(CMAKE_EXE_LINKER_FLAGS_DEBUG
                "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=address")
        endif()
    elseif(MSVC)
        target_compile_options(ccev PRIVATE /fsanitize=address)
    else()
        message(WARNING "CCEV_SANITIZE is set but compiler '${CMAKE_C_COMPILER_ID}' is not supported. Ignoring.")
    endif()
endif()

# ── ThreadSanitizer (mutually exclusive with CCEV_SANITIZE) ──
if(CCEV_TSAN AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        if(NOT WIN32)
            target_compile_options(ccev PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
            set(CMAKE_EXE_LINKER_FLAGS_DEBUG
                "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=thread")
            # TSAN requires position-independent code for shared libraries;
            # static libs are fine with PIC (already set above).
        else()
            target_compile_options(ccev PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
            set(CMAKE_EXE_LINKER_FLAGS_DEBUG
                "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=thread")
        endif()
    else()
        message(WARNING "CCEV_TSAN is set but compiler '${CMAKE_C_COMPILER_ID}' is not supported. Ignoring.")
    endif()
endif()

# ── Install ─────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS ccev
    EXPORT ccev-targets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
)
set(CCEV_INSTALL_TLS)
if(CCEV_TLS AND OpenSSL_FOUND)
    set(CCEV_INSTALL_TLS src/ccev_tls.h)
endif()
install(FILES
    src/ccev.h
    ${CCEV_INSTALL_TLS}
    deps/ccsocket/include/ccsocket.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(EXPORT ccev-targets
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ccev
    NAMESPACE ccev::
)

# ── Tests & examples ────────────────────────────────────────
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
