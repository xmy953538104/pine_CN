// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import java.util.UUID

/** A launch survives logging-session changes. Disabled launches allocate no random ID. */
internal class LaunchIdentity {
    var id: String = ""; private set
    var active: Boolean = false; private set
    var startObserved: Boolean = false; private set
    fun begin(enabled: Boolean): String {
        active = true
        startObserved = enabled
        id = if (enabled) UUID.randomUUID().toString() else ""
        return id
    }
    fun observe(enabled: Boolean): String {
        if (enabled && active && id.isEmpty()) id = UUID.randomUUID().toString()
        return id
    }
    fun end() { active = false; id = ""; startObserved = false }
}
