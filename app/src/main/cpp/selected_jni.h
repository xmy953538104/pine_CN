// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <jni.h>

namespace skyline::kernel { class OS; }
namespace skyline {
    // Borrows the launch FD. The selected path duplicates it before inspection;
    // the common emulation JNI lifecycle remains its caller/owner.
    void ExecuteSelectedFromJava(JNIEnv *env, kernel::OS &os, int borrowedFd,
                                 jobjectArray fields, jintArray indices);
}
