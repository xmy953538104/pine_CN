// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import java.util.UUID

/** A launch survives logging-session changes. Disabled launches allocate no random ID. */
internal class LaunchIdentity {
    var id: String = ""; private set
    var active: Boolean = false; private set
    var owner: Long = 0; private set
    var startObserved: Boolean = false; private set
    fun begin(enabled: Boolean, owner: Long = 0): String {
        this.owner = owner
        active = true
        startObserved = enabled
        id = if (enabled) UUID.randomUUID().toString() else ""
        return id
    }
    fun observe(enabled: Boolean): String {
        if (enabled && active && id.isEmpty()) id = UUID.randomUUID().toString()
        return id
    }
    fun belongsTo(expected: Long): Boolean = expected != 0L && owner == expected
    fun end(expected: Long? = null): Boolean {
        if (expected != null && (!active || !belongsTo(expected))) return false
        active = false; id = ""; startObserved = false
        return true
    }
}
