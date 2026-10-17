// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.app.ApplicationExitInfo
import org.junit.Assert.*
import org.junit.Test

class StartupDiagnosticsTest {
    @Test fun titleIdentityIsBoundedStructuredHexInsteadOfDetailParsing() {
        assertEquals("0100000000001000", Diagnostics.normalizedTitleId("0100000000001000"))
        assertEquals("010000000000ABCD", Diagnostics.normalizedTitleId("010000000000abcd"))
        for (invalid in listOf(null, "", "unknown", "title_id=0100000000001000", "../private", "01000000000010000"))
            assertEquals("", Diagnostics.normalizedTitleId(invalid))
    }

    @Test fun systemReportsCrashesAndAnrWithoutRelabelingNormalExitOrSignalKill() {
        for (reason in listOf(ApplicationExitInfo.REASON_CRASH, ApplicationExitInfo.REASON_CRASH_NATIVE, ApplicationExitInfo.REASON_ANR))
            assertEquals("PREVIOUS_PROCESS_CRASH", PreviousProcessExit.eventName(reason))
        for (reason in listOf(ApplicationExitInfo.REASON_EXIT_SELF, ApplicationExitInfo.REASON_SIGNALED,
            ApplicationExitInfo.REASON_USER_REQUESTED, ApplicationExitInfo.REASON_LOW_MEMORY, ApplicationExitInfo.REASON_UNKNOWN))
            assertEquals("PREVIOUS_PROCESS_EXIT", PreviousProcessExit.eventName(reason))
    }
}
