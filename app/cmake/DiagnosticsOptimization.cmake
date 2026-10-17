# SPDX-License-Identifier: MPL-2.0
set(PINE_NATIVE_DEBUG_OPTIMIZATION "0" CACHE STRING "Debug native optimization: baseline 0 or controlled candidate 2")
set_property(CACHE PINE_NATIVE_DEBUG_OPTIMIZATION PROPERTY STRINGS 0 2)
if(NOT PINE_NATIVE_DEBUG_OPTIMIZATION MATCHES "^[02]$")
    message(FATAL_ERROR "PINE_NATIVE_DEBUG_OPTIMIZATION must be 0 or 2")
endif()
# Retain debug information, frame pointers, assertions and existing protection flags.
# Release's -Ofast/-flto/-DNDEBUG/-fno-stack-protector are not used here.
set(CMAKE_CXX_FLAGS_DEBUG "-O${PINE_NATIVE_DEBUG_OPTIMIZATION} -g3 -glldb -gdwarf-5 -fno-omit-frame-pointer")
