// SPDX-License-Identifier: MPL-2.0
package emu.skyline.loader

/** Native inspection borrows the caller's descriptor for this synchronous call. */
object SelectedContentBridge {
    @JvmStatic external fun isAvailable(): Boolean
    @JvmStatic external fun inspect(romFd: Int, privateFilesPath: String, profile: String): String
    @JvmStatic external fun inspectObserved(romFd: Int, privateFilesPath: String, profile: String): String
}
