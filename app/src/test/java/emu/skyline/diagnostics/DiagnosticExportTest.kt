// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.RandomAccessFile
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import java.util.zip.ZipInputStream

class DiagnosticExportTest {
    @Test fun resourceWindowsUseFixedScopesAndRedactBeforeZipExport() = sandbox { logs, textLog, cache, _ ->
        listOf("main/", "emulation/", "").forEachIndexed { index, scope ->
            log(logs, "${scope}resource-window.jsonl", """{"schema":1,"session_id":"sample-$index","seq":1,"event":"HOST_MEMORY_SAMPLE","fields":{"rss_kib":"123","path":"/data/user/0/private/memory","password":"hidden-value"}}""" + "\n")
        }
        log(logs, "foreign/resource-window.jsonl", "{}\n")
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val entries = archive(snapshot)
            for (scope in listOf("main/", "emulation/", "")) {
                val row = Json.parseToJsonElement(entries.getValue("${scope}resource-window.jsonl").trim()).jsonObject
                assertEquals("123", row["fields"]!!.jsonObject["rss_kib"]!!.jsonPrimitive.content)
            }
            assertFalse(entries.containsKey("foreign/resource-window.jsonl"))
            assertFalse(entries.values.joinToString().contains("hidden-value"))
            assertFalse(entries.values.joinToString().contains("/data/user"))
        }
    }
    @Test fun oversizedResourceWindowIsOmittedRatherThanTreatedAsEightMiBLog() = sandbox { logs, textLog, cache, _ ->
        log(logs, "main/resource-window.jsonl", "x".repeat(128 * 1024 + 1))
        assertFalse(DiagnosticExport.files(logs, textLog).any { it.name == "resource-window.jsonl" })
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val entries = archive(snapshot)
            assertFalse(entries.containsKey("main/resource-window.jsonl"))
            assertTrue(entries.getValue("session-metadata.txt").contains("main/resource-window.jsonl: exceeds 131072 bytes"))
        }
    }

    @Test fun productionGuestDfx1RawTextAndDecodedPayloadSurviveRealZipExport() = sandbox { logs, textLog, cache, _ ->
        val source = File("../tools/tests/fixtures/native-sink.jsonl").readText()
        val original = source.lineSequence().filter { it.isNotBlank() }.map { Json.parseToJsonElement(it).jsonObject }.toList()
        val probe = original.first { it["event"]?.jsonPrimitive?.content == "DFX_PROBE" }
        val wire = probe["fields"]!!.jsonObject["message"]!!.jsonPrimitive.content
        log(logs, "main/diagnostics-0.jsonl", source)
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val text = archive(snapshot).getValue("main/diagnostics-0.jsonl")
            val rows = text.lineSequence().filter { it.isNotBlank() }.map { Json.parseToJsonElement(it).jsonObject }.toList()
            val exported = rows.first { it["event"]?.jsonPrimitive?.content == "DFX_PROBE" }["fields"]!!.jsonObject
            assertEquals(wire, exported["message"]!!.jsonPrimitive.content)
            assertEquals("中文 | %", exported["game_message"]!!.jsonPrimitive.content)
            assertEquals("DFX1", exported["bridge_protocol"]!!.jsonPrimitive.content)
            assertTrue(rows.any { it["event"]?.jsonPrimitive?.content == "GUEST_DEBUG_MESSAGE" && it["fields"]?.jsonObject?.get("message")?.jsonPrimitive?.content == wire })
        }
    }

    @Test fun dfxPercentEscapesCannotBypassDefaultPrivatePathAndCredentialRedaction() {
        val path = "DFX1|event=DFX_LUA_ERROR|probe_id=privacy-test|file=%2Fdata%2Fuser%2F0%2FAlice%20Smith%2Fprivate.lua|message=error"
        val secret = "DFX1|event=DFX_PROBE|probe_id=privacy-test|message=password%3Dprivate-value"
        for (wire in listOf(path, secret)) {
            val result = DiagnosticExport.redact(wire)
            assertFalse(result.contains("Alice")); assertFalse(result.contains("private-value"))
            assertFalse(result.contains("%2Fdata")); assertTrue(result.contains("REDACTED"))
        }
        assertEquals("DFX1|[REDACTED_MALFORMED_PROTOCOL]", DiagnosticExport.redact("DFX1|event=DFX_PROBE|message=%GG"))
    }

    @Test fun realNativeLoggerPrefixCannotExportEncodedPrivateDfxPayload() = sandbox { logs, textLog, cache, _ ->
        // AsyncLogger::WriteFile: "{:7} | {:>10} | {:^15} | {}\n", then SVC function prefix.
        val prefix = "INFO    |     123456 |      HOS-1      | OutputDebugString: "
        textLog.writeText(prefix + "DFX1|event=DFX_LUA_ERROR|probe_id=privacy|file=%2Fdata%2Fuser%2F0%2FAlice%2Fprivate.lua|message=error\n" +
            prefix + "DFX1|event=DFX_PROBE|probe_id=privacy|message=password%3Dprivate-value\n")
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val entries = archive(snapshot)
            val all = entries.values.joinToString("\n")
            for (privateValue in listOf("Alice", "private.lua", "private-value", "%2Fdata", "password%3D")) assertFalse(privateValue, all.contains(privateValue))
            assertTrue(entries.getValue("emulation.log").contains("REDACTED_PATH"))
            assertTrue(entries.getValue("emulation.log").contains("REDACTED_CREDENTIAL_LINE"))
            assertTrue(entries.getValue("emulation.log").contains("OutputDebugString:"))
        }
    }

    @Test fun dfxFragmentCannotReviveAWholeLineCredentialOrOmittedPrivatePathTail() = sandbox { logs, textLog, cache, _ ->
        textLog.writeText("password=prefix DFX1|event=DFX_PROBE|probe_id=x|message=private-credential-tail\n" +
            "open /data/user/0/Alice/private DFX1|event=DFX_PROBE|probe_id=x|message=private-path-tail\n")
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val entries = archive(snapshot)
            val all = entries.values.joinToString("\n")
            for (secret in listOf("private-credential-tail", "private-path-tail", "Alice", "password=prefix")) assertFalse(secret, all.contains(secret))
            assertTrue(entries.getValue("emulation.log").contains("REDACTED_CREDENTIAL_LINE"))
            assertTrue(entries.getValue("emulation.log").contains("REDACTED_PATH"))
        }
    }

    @Test fun combinedDfxEncodingAndPlainPrivacyRulesComposeInBothFieldOrders() = sandbox { logs, textLog, cache, _ ->
        val prefixes = listOf("INFO    |     123456 |      HOS-1      | OutputDebugString: ", "password=prefix ", "open /data/user/0/Alice/prefix ")
        val payloads = listOf(
            "message=password%3Dprivate-encoded|file=/data/user/0/Alice/private.lua|traceback=private-omitted-tail",
            "file=/data/user/0/Alice/private.lua|message=password%3Dprivate-encoded|traceback=private-omitted-tail",
            "message=password%3Dprivate-encoded|file=%2Fdata%2Fuser%2F0%2FAlice%2Fprivate.lua",
            "file=%2Fdata%2Fuser%2F0%2FAlice%2Fprivate.lua|message=password%3Dprivate-encoded",
            "message=password=private-plaintext|file=%2Fdata%2Fuser%2F0%2FAlice%2Fprivate.lua",
            "file=%2Fdata%2Fuser%2F0%2FAlice%2Fprivate.lua|message=password=private-plaintext")
        textLog.writeText(prefixes.flatMap { prefix -> payloads.map { payload ->
            prefix + "DFX1|event=DFX_LUA_ERROR|probe_id=privacy|" + payload
        } }.joinToString("\n", postfix = "\n"))
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val all = archive(snapshot).values.joinToString("\n")
            for (secret in listOf("Alice", "private.lua", "%2Fdata", "private-encoded", "private-plaintext", "private-omitted-tail", "password%3D"))
                assertFalse(secret, all.contains(secret))
        }
    }
    @Test fun criticalStateSharesSequenceWithoutReplayingAnErrorAndRemainsAllowlisted() = sandbox { logs, textLog, cache, _ ->
        val original = Json.parseToJsonElement(record("same", 7, "exception", "NATIVE_EXCEPTION", "ERROR")).jsonObject
        val state = JsonObject(original + mapOf("record_kind" to JsonPrimitive("critical_snapshot"),
            "event" to JsonPrimitive("CRITICAL_STATE_SNAPSHOT"), "level" to JsonPrimitive("INFO"),
            "fields" to buildJsonObject { put("observed_event", "NATIVE_EXCEPTION"); put("detail", "path:/data/private/owner") }))
        log(logs, "main/critical-state.jsonl", state.toString() + "\n")
        log(logs, "main/diagnostics-0.jsonl", original.toString() + "\n")
        log(logs, "main/critical-state.tmp", "NEVER_EXPORT_TEMP")
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val result = archive(snapshot)
            assertTrue(result.containsKey("main/critical-state.jsonl"))
            assertFalse(result.keys.any { it.endsWith(".tmp") })
            assertFalse(result.values.any { it.contains("private/owner") })
            val metadata = result.getValue("session-metadata.txt")
            assertTrue(metadata.contains("exported_records=1"))
            assertTrue(metadata.contains("exported_critical_state_records=1"))
            assertTrue(metadata.contains("observed_error_count=1"))
        }
    }

    @Test fun repeatedHistoricalImportAcrossSessionsDoesNotInventNewCrashes() = sandbox { logs, textLog, cache, _ ->
        fun historical(session: String) = buildJsonObject {
            put("session_id", session); put("sequence", 3); put("category", "lifecycle")
            put("event", "PREVIOUS_PROCESS_CRASH"); put("level", "ERROR")
            putJsonObject("fields") { put("historical_record", "true"); put("exit_record_id", "a".repeat(64)) }
        }.toString() + "\n"
        log(logs, "main/diagnostics-0.jsonl", historical("one"))
        log(logs, "emulation/diagnostics-0.jsonl", historical("two"))
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val metadata = archive(snapshot).getValue("session-metadata.txt")
            assertTrue(metadata.contains("historical_crash_observations=1"))
            assertTrue(metadata.contains("omitted_duplicate_historical_records=1"))
            assertTrue(metadata.contains("observed_error_count=0"))
        }
    }

    @Test fun realNativeTwentyTwoFieldTraceSnapshotRetainsStackAfterRotationAndExport() = sandbox { logs, textLog, cache, _ ->
        val fixture = File("../tools/tests/fixtures/native-trace-snapshot.jsonl").readText()
        log(logs, "main/critical-state.jsonl", fixture)
        // Native fixture generator asserts the original trace is gone from both ordinary files.
        log(logs, "main/diagnostics-0.jsonl", record("later", 500, "storage", "DLC_DATA_READ"))
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val exported = archive(snapshot)
            val state = Json.parseToJsonElement(exported.getValue("main/critical-state.jsonl").trim()).jsonObject
            val fields = state.getValue("fields").jsonObject
            assertEquals(16, fields.getValue("frames").jsonPrimitive.content.split(";").size)
            assertEquals("16", fields.getValue("frames_seen").jsonPrimitive.content)
            assertEquals("true", fields.getValue("trace_complete").jsonPrimitive.content)
            assertEquals("393216", fields.getValue("trace_bytes_read").jsonPrimitive.content)
            assertEquals("complete", fields.getValue("trace_capture_status").jsonPrimitive.content)
            assertEquals("3", fields.getValue("trace_attempt_limit").jsonPrimitive.content)
            assertTrue(fields.getValue("frames").jsonPrimitive.content.contains("libskyline.so+0x1234@" + "a".repeat(40)))
            assertEquals(0, state.getValue("snapshot_metadata").jsonObject.getValue("fields_omitted").jsonPrimitive.int)
            assertTrue(exported.getValue("session-metadata.txt").contains("exported_critical_state_records=1"))
            assertTrue(exported.getValue("session-metadata.txt").contains("observed_error_count=0"))
        }
    }

    @Test fun malformedStagePayloadCannotBreakSafeExportOrInventNormalEnd() = sandbox { logs, textLog, cache, _ ->
        log(logs, "main/diagnostics-0.jsonl", """{"session_id":"synthetic","sequence":1,"event":"STAGE_COMPLETED","fields":{"stage":["EMULATION_ENDED"]}}""" + "\n")
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val metadata = archive(snapshot).getValue("session-metadata.txt")
            assertTrue(metadata.contains("exported_records=1"))
            assertTrue(metadata.contains("\"emulation_end_observed\":false"))
        }
    }

    @Test fun historicalDropCountersRemainVisibleWhenCurrentSessionHasZero() = sandbox { logs, textLog, cache, _ ->
        fun row(session: String, sequence: Int, dropped: Int, event: String) = buildJsonObject {
            put("schema", 1); put("session_id", session); put("sequence", sequence)
            put("category", "lifecycle"); put("event", event); put("level", "INFO")
            put("dropped_count", dropped)
            putJsonObject("fields") { put("stage", "SYNTHETIC_GPU_MEMBER") }
        }.toString() + "\n"
        log(logs, "main/diagnostics-0.jsonl", row("current", 1, 0, "SESSION_METADATA"))
        log(logs, "main/diagnostics-1.jsonl", row("historical", 1, 1, "SESSION_METADATA") +
            row("historical", 2, 2, "STAGE_STARTED") + row("historical", 3, 2, "SESSION_SUMMARY"))
        DiagnosticExport.capture(logs, textLog, cache, "status_session_id=current\nwritten=1; dropped=0; disk_errors=0").use { snapshot ->
            val metadata = archive(snapshot).getValue("session-metadata.txt")
            assertTrue(metadata.contains("observed_dropped_count_lower_bound=2"))
            val summaries = metadata.lineSequence().mapNotNull {
                runCatching { Json.parseToJsonElement(it).jsonObject }.getOrNull()
            }.filter { "session_summary_observed" in it }.associateBy { it.getValue("session_id").jsonPrimitive.content }
            assertEquals(2, summaries.size)
            assertEquals(2, summaries.getValue("historical").getValue("observed_dropped_count_lower_bound").jsonPrimitive.int)
            assertTrue(summaries.getValue("historical").getValue("session_summary_observed").jsonPrimitive.boolean)
            assertFalse(summaries.getValue("current").getValue("session_summary_observed").jsonPrimitive.boolean)
            assertEquals("SYNTHETIC_GPU_MEMBER", summaries.getValue("historical").getValue("last_started_stage").jsonPrimitive.content)
            assertTrue(metadata.contains("zero does not imply no crash"))
            assertTrue(metadata.contains("does not distinguish crash, kill, live session"))
        }
    }

    @Test fun observedDropCountersUseMaximumInSequenceIndependentSnapshotOrder() = sandbox { logs, textLog, cache, _ ->
        fun row(sequence: Int, dropped: Long) = """{"session_id":"synthetic","sequence":$sequence,"dropped_count":$dropped,"fields":{}}""" + "\n"
        log(logs, "main/diagnostics-0.jsonl", row(3, 4))
        log(logs, "main/diagnostics-1.jsonl", row(1, 1) + row(2, -1))
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            assertTrue(archive(snapshot).getValue("session-metadata.txt").contains("observed_dropped_count_lower_bound=4"))
        }
    }

    private fun sandbox(test: (logs: File, textLog: File, cache: File, root: File) -> Unit) {
        val root = Files.createTempDirectory("pine-diag-test").toFile()
        try {
            test(File(root, "diagnostics").apply { mkdir() }, File(root, "emulation.log"), File(root, "cache"), root)
        } finally { root.deleteRecursively() }
    }

    private fun log(logs: File, name: String, text: String) = File(logs, name).apply {
        parentFile!!.mkdirs(); writeText(text, Charsets.UTF_8)
    }

    private fun record(session: String, sequence: Int, category: String = "content", event: String = "BASE_SELECTED", level: String = "INFO", detail: String = "title_id=01000000") = buildJsonObject {
        put("schema", 1); put("session_id", session); put("sequence", sequence)
        put("category", category); put("event", event); put("level", level)
        put("last_stage", "content_ready")
        putJsonObject("fields") { put("detail", detail) }
    }.toString() + "\n"

    private fun archive(snapshot: DiagnosticExport.Snapshot): Map<String, String> {
        val output = ByteArrayOutputStream()
        DiagnosticExport.write(output, snapshot)
        val entries = linkedMapOf<String, String>()
        ZipInputStream(output.toByteArray().inputStream()).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                assertNull("Duplicate ZIP name", entries.put(entry.name, zip.readBytes().toString(Charsets.UTF_8)))
            }
        }
        assertEquals(snapshot.entries.map { it.name }.toSet(), entries.keys)
        snapshot.entries.forEach { assertEquals(it.bytes, entries.getValue(it.name).toByteArray(Charsets.UTF_8).size.toLong()) }
        return entries
    }

    @Test fun redactionPreservesJsonStructureTypesEscapesAndUtf8() {
        val source = Json.parseToJsonElement("""{"schema":1,"message":"世界 CONTENT:\/\/private\/rom?q=1","password":"quoted \"secret\"","fields":{"token":{"nested":"do-not-leak"},"path":"/data/private/rom","windows":"C:\\Users\\Private\\game.nsp","valid":true,"count":12,"missing":null,"array":["https://private/rom",2,false]},"event":"GUEST_DEBUG_MESSAGE"}""").jsonObject
        val safe = DiagnosticExport.redactJson(source)
        val reparsed = Json.parseToJsonElement(safe.toString()).jsonObject
        assertEquals(source.keys, reparsed.keys)
        assertEquals(1, reparsed.getValue("schema").jsonPrimitive.int)
        assertEquals("GUEST_DEBUG_MESSAGE", reparsed.getValue("event").jsonPrimitive.content)
        assertTrue(reparsed.getValue("message").jsonPrimitive.content.contains("世界"))
        assertEquals(source.getValue("fields").jsonObject.keys, reparsed.getValue("fields").jsonObject.keys)
        val fields = reparsed.getValue("fields").jsonObject
        assertTrue(fields.getValue("valid").jsonPrimitive.boolean)
        assertEquals(12, fields.getValue("count").jsonPrimitive.int)
        assertEquals(JsonNull, fields["missing"])
        assertEquals(2, fields.getValue("array").jsonArray[1].jsonPrimitive.int)
        assertFalse(safe.toString().contains("private", ignoreCase = true))
        assertFalse(safe.toString().contains("do-not-leak"))
        assertFalse(safe.toString().contains("quoted"))
    }

    @Test fun textRedactsCredentialLinesAndKnownLocations() {
        val safe = DiagnosticExport.redact("before 世界\nROM content://authority/game.nsp /storage/emulated/0/private\npassword=DO_NOT_EXPORT rest-of-line\nAuthorization: Bearer DO_NOT_EXPORT\nprod.keys DO_NOT_EXPORT\nafter")
        assertTrue(safe.contains("before 世界")); assertTrue(safe.endsWith("after"))
        assertFalse(safe.contains("authority")); assertFalse(safe.contains("game.nsp"))
        assertFalse(safe.contains("emulated")); assertFalse(safe.contains("DO_NOT_EXPORT"))
        assertFalse(safe.contains("rest-of-line"))
    }

    @Test fun redactsWholeLocationTailWithSpacesQuotesAndUtf8AtRealBoundaries() {
        val locations = listOf(
            "/storage/emulated/0/Alice Smith/秘密 \"save\".nsp" to "[REDACTED_PATH]",
            "/data/user/0/app/Private Name/O'Connor 秘密.nsp" to "[REDACTED_PATH]",
            "/cache/Private Name/秘密.nsp" to "[REDACTED_PATH]",
            "C:\\Users\\Alice Smith\\秘密 \"save\".nsp" to "[REDACTED_PATH]",
            "D:/Private Name/O'Connor 秘密.nsp" to "[REDACTED_PATH]",
            "\\\\server\\Private Share\\秘密 file.nsp" to "[REDACTED_PATH]",
            "content://provider/Private Name/秘密 \"save\".nsp" to "[REDACTED_URI]",
            "file:///storage/emulated/0/Private Name/O'Connor 秘密.nsp" to "[REDACTED_URI]",
            "file:/storage/emulated/0/Private Name/秘密 file.nsp" to "[REDACTED_URI]",
            "file:C:\\Users\\Private Name\\秘密 file.nsp" to "[REDACTED_URI]"
        )
        for ((location, marker) in locations) {
            for (prefix in listOf("open failed: ", "读取失败：", "path=(", "path=\"", "path='", "path:", "std::filesystem::path:", "ns::")) {
                assertEquals(prefix + marker, DiagnosticExport.redact(prefix + location + "'\" trailing detail"))
            }
            if (!location.startsWith("/")) assertEquals("正在打开" + marker, DiagnosticExport.redact("正在打开" + location))
        }
        val relative = "scripts/data/a.lua ./data/a.lua ../data/a.lua 脚本/data/a.lua scripts/storage/a.lua /database/a.lua /datafile/a.lua rom:/data/a.lua romfs:/storage/a.lua sdmc:/data/a.lua ns::rom:/data/a.lua"
        assertEquals(relative, DiagnosticExport.redact(relative))
    }

    @Test(timeout = 5000) fun longGuestLinesStayFastAndKeepFirstLocationSemantics() = sandbox { logs, textLog, cache, _ ->
        val plain = "a".repeat(65536)
        assertEquals(plain, DiagnosticExport.redact(plain))
        val noSchemeLetter = "123+.-".repeat(10000) + "://"
        assertEquals(noSchemeLetter, DiagnosticExport.redact(noSchemeLetter))
        val mounts = "rom:/data/a.lua sdmc:/data/b.lua ".repeat(1600)
        assertEquals(mounts, DiagnosticExport.redact(mounts))
        val late = plain.take(60000) + " 正在打开content://provider/Alice Smith/秘密 file"
        val lateExpected = plain.take(60000) + " 正在打开[REDACTED_URI]"
        assertEquals(lateExpected, DiagnosticExport.redact(late))
        assertEquals("[REDACTED_URI]", DiagnosticExport.redact(plain + "https://provider/private"))
        assertEquals("123+.-[REDACTED_URI]", DiagnosticExport.redact("123+.-content://provider/private"))
        assertEquals("open [REDACTED_PATH]", DiagnosticExport.redact("open /data/private then https://provider/private"))
        assertEquals("open [REDACTED_URI]", DiagnosticExport.redact("open https://provider/private then /data/private"))
        val messages = listOf(plain.take(60000), late)
        log(logs, "main/diagnostics-0.jsonl", messages.joinToString("\n", postfix = "\n") { buildJsonObject { put("message", it) }.toString() })
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val exported = archive(snapshot).getValue("main/diagnostics-0.jsonl").lineSequence().filter { it.isNotBlank() }
                .map { Json.parseToJsonElement(it).jsonObject.getValue("message").jsonPrimitive.content }.toList()
            assertEquals(listOf(messages[0], lateExpected), exported)
        }
    }

    @Test fun archiveRedactsSpacedLocationsWithoutChangingOtherJsonFields() = sandbox { logs, textLog, cache, _ ->
        val relative = "世界 scripts/data/a.lua rom:/data/a.lua"
        val messages = listOf(
            "open failed: /storage/emulated/0/Alice Smith/秘密 \"save\".nsp",
            "open failed: 'C:\\Users\\Alice Smith\\秘密 O'Connor.nsp'",
            "open failed: content://provider/Alice Smith/秘密 \"save\".nsp"
        )
        val records = messages.mapIndexed { index, message -> buildJsonObject {
            put("schema", 1); put("session_id", "spaced-paths"); put("sequence", index + 1)
            put("category", "content"); put("event", "DLC_RUNTIME_ERROR"); put("level", "ERROR")
            putJsonObject("fields") { put("message", message); put("relative", relative); put("count", 7); put("known", true) }
        } }
        log(logs, "main/diagnostics-0.jsonl", records.joinToString("\n", postfix = "\n"))
        textLog.writeText(messages.joinToString("\n", postfix = "\n"))
        DiagnosticExport.capture(logs, textLog, cache, "schema=1\nerror=/data/user/0/Alice Smith/秘密 \"save\"").use { snapshot ->
            val entries = archive(snapshot)
            assertFalse(entries.values.any { text -> listOf("Alice", "Smith", "秘密", "Connor", "save").any { it in text } })
            val exported = entries.getValue("main/diagnostics-0.jsonl").lineSequence().filter { it.isNotBlank() }.map { Json.parseToJsonElement(it).jsonObject }.toList()
            assertEquals(records.size, exported.size)
            exported.forEachIndexed { index, value ->
                assertEquals(records[index].keys, value.keys)
                val fields = value.getValue("fields").jsonObject
                assertEquals(relative, fields.getValue("relative").jsonPrimitive.content)
                assertEquals(7, fields.getValue("count").jsonPrimitive.int)
                assertTrue(fields.getValue("known").jsonPrimitive.boolean)
                assertTrue(fields.getValue("message").jsonPrimitive.content.startsWith("open failed: "))
            }
            assertTrue(entries.getValue("session-metadata.txt").contains("DLC_RUNTIME_ERROR"))
        }
    }

    @Test fun exportUsesOnlyFixedNamesAcrossBothProcessesAndLegacy() = sandbox { logs, textLog, cache, _ ->
        log(logs, "diagnostics-0.jsonl", record("legacy", 1))
        log(logs, "main/diagnostics-0.jsonl", record("main", 1, detail = "世界 content://private/game"))
        log(logs, "emulation/diagnostics-0.jsonl", record("emu", 1))
        log(logs, "prod.keys", "DO_NOT_EXPORT")
        log(logs, "subdirectory/diagnostics-1.jsonl", "DO_NOT_EXPORT")
        log(logs, "main/diagnostics-8.jsonl", "DO_NOT_EXPORT")
        log(logs, "main/session-id.txt", "DO_NOT_EXPORT")
        textLog.writeText("content://private/game.nsp\nhello 世界\n")
        DiagnosticExport.capture(logs, textLog, cache, "schema=1\ncommit=0123456789abcdef").use { snapshot ->
            val entries = archive(snapshot)
            assertEquals(setOf("session-metadata.txt", "diagnostics-0.jsonl", "main/diagnostics-0.jsonl", "emulation/diagnostics-0.jsonl", "emulation.log"), entries.keys)
            assertFalse(entries.values.any { it.contains("private") || it.contains("DO_NOT_EXPORT") })
            assertTrue(entries.getValue("main/diagnostics-0.jsonl").contains("世界"))
            assertTrue(entries.getValue("session-metadata.txt").contains("commit=0123456789abcdef"))
        }
    }

    @Test fun rejectsSymlinkFilesSymlinkParentsAndOversizedLogs() = sandbox { logs, textLog, cache, root ->
        val outside = File(root, "outside").apply { mkdir() }
        val secret = File(outside, "secret").apply { writeText("DO_NOT_EXPORT") }
        Files.createSymbolicLink(File(logs, "diagnostics-0.jsonl").toPath(), secret.toPath())
        File(outside, "diagnostics-0.jsonl").writeText(record("DO_NOT_EXPORT", 1))
        Files.createSymbolicLink(File(logs, "main").toPath(), outside.toPath())
        Files.createSymbolicLink(textLog.toPath(), secret.toPath())
        RandomAccessFile(File(logs, "diagnostics-1.jsonl"), "rw").use { it.setLength(DiagnosticExport.MAX_FILE_BYTES.toLong() + 1) }
        assertTrue(DiagnosticExport.files(logs, textLog).isEmpty())
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val entries = archive(snapshot)
            assertEquals(setOf("session-metadata.txt"), entries.keys)
            assertTrue(entries.values.none { it.contains("DO_NOT_EXPORT") })
            assertTrue(entries.getValue("session-metadata.txt").contains("exceeds ${DiagnosticExport.MAX_FILE_BYTES}"))
        }
    }

    @Test fun previewIsStableAfterAppendRenameDeletionAndActivityRestore() = sandbox { logs, textLog, cache, _ ->
        val source = log(logs, "main/diagnostics-0.jsonl", record("before", 1))
        textLog.writeText("before\n")
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val preview = snapshot.entries
            source.appendText(record("AFTER", 2))
            Files.move(source.toPath(), File(logs, "main/diagnostics-1.jsonl").toPath())
            source.writeText(record("AFTER", 3))
            textLog.delete()
            val restored = DiagnosticExport.restore(cache, snapshot.id)!!
            assertEquals(preview, restored.entries)
            val entries = archive(restored)
            assertFalse(entries.values.any { it.contains("AFTER") })
            assertEquals("before\n", entries["emulation.log"])
            assertNull(DiagnosticExport.restore(cache, "../${snapshot.id}"))
        }
    }

    @Test fun omitsInvalidPartialAndOversizedRecordsWithoutLeakingRawText() = sandbox { logs, textLog, cache, _ ->
        log(logs, "main/diagnostics-0.jsonl", record("valid", 1) +
            "{broken DO_NOT_EXPORT}\n" + "x".repeat(65537) + "DO_NOT_EXPORT\n" + "{\"message\":\"DO_NOT_EXPORT")
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val entries = archive(snapshot)
            val lines = entries.getValue("main/diagnostics-0.jsonl").lineSequence().filter { it.isNotBlank() }.toList()
            assertEquals(1, lines.size)
            assertEquals("valid", Json.parseToJsonElement(lines.single()).jsonObject.getValue("session_id").jsonPrimitive.content)
            val metadata = entries.getValue("session-metadata.txt")
            assertTrue(metadata.contains("omitted_malformed_records=1"))
            assertTrue(metadata.contains("omitted_incomplete_records=1"))
            assertTrue(metadata.contains("omitted_oversized_lines=1"))
            assertFalse(entries.values.any { it.contains("DO_NOT_EXPORT") })
        }
    }

    @Test fun summaryKeepsDistinctSessionsAndDeduplicatesRotatedRecords() = sandbox { logs, textLog, cache, _ ->
        val first = record("main", 1, event = "CONTENT_ASSOCIATION", detail = "title_id=01000000 update_count=2 dlc_count=3")
        log(logs, "main/diagnostics-0.jsonl", first)
        log(logs, "main/diagnostics-1.jsonl", first)
        log(logs, "emulation/diagnostics-0.jsonl", record("emulation", 1, event = "DLC_RUNTIME_ERROR", level = "ERROR", detail = "title_id=01000000 error=short_read /data/private/rom"))
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val entries = archive(snapshot)
            val records = entries.filterKeys { it.endsWith(".jsonl") }.values.sumOf { text -> text.lineSequence().count { it.isNotBlank() } }
            assertEquals(2, records)
            val metadata = entries.getValue("session-metadata.txt")
            assertTrue(metadata.contains("omitted_duplicate_records=1"))
            assertTrue(metadata.contains("observed_error_count=1"))
            assertTrue(metadata.contains("CONTENT_ASSOCIATION")); assertTrue(metadata.contains("DLC_RUNTIME_ERROR"))
            assertTrue(metadata.contains("01000000")); assertTrue(metadata.contains("update_count=2 dlc_count=3"))
            assertTrue(metadata.contains("content_ready")); assertFalse(metadata.contains("private"))
        }
    }

    @Test fun metadataDistinguishesCapturedSessionBuildsFromCurrentExporter() = sandbox { logs, textLog, cache, _ ->
        log(logs, "main/diagnostics-0.jsonl", record("old-session", 1, category = "lifecycle", event = "SESSION_METADATA", detail = "version=old commit=11111111 process=main") + record("old-session", 2))
        log(logs, "emulation/diagnostics-0.jsonl", record("new-session", 1, category = "lifecycle", event = "SESSION_METADATA", detail = "version=new commit=22222222 process=emulation") + record("unknown-build-session", 1))
        DiagnosticExport.capture(logs, textLog, cache, "schema=1\nexporter_commit=33333333").use { snapshot ->
            val metadata = archive(snapshot).getValue("session-metadata.txt")
            assertTrue(metadata.contains("exporter_commit=33333333"))
            assertTrue(metadata.contains("version=old commit=11111111 process=main"))
            assertTrue(metadata.contains("version=new commit=22222222 process=emulation"))
            assertTrue(metadata.contains("observed_sessions_without_build_metadata=unknown-build-session"))
        }
    }

    @Test fun abandonedPrivateSnapshotsHaveBoundedRetention() = sandbox { logs, textLog, cache, _ ->
        val snapshots = (0..3).map {
            DiagnosticExport.capture(logs, textLog, cache, "schema=1")
        }
        try {
            assertEquals(2, cache.listFiles()!!.count { it.isDirectory })
            assertNotNull(DiagnosticExport.restore(cache, snapshots.last().id))
        } finally { snapshots.forEach { it.close() } }
    }

    @Test fun prunesInterruptedSnapshotWithoutMetadata() = sandbox { logs, textLog, cache, _ ->
        val incomplete = File(cache, "snapshot-11111111-1111-1111-1111-111111111111")
        log(incomplete, "emulation/diagnostics-0.jsonl", "partial export")
        assertTrue(incomplete.setLastModified(1))
        assertNull(DiagnosticExport.restore(cache, incomplete.name))
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use {
            assertFalse("Interrupted snapshot was not pruned", incomplete.exists())
        }
    }

    @Test fun snapshotCleanupUnlinksScopeSymlinkWithoutDeletingOutsideLogs() = sandbox { logs, textLog, cache, root ->
        log(logs, "main/diagnostics-0.jsonl", record("main", 1))
        val outside = File(root, "outside").apply { mkdir() }
        val victim = File(outside, "diagnostics-0.jsonl").apply { writeText("DO_NOT_DELETE") }
        val snapshot = DiagnosticExport.capture(logs, textLog, cache, "schema=1")
        val scope = File(cache, "${snapshot.id}/main")
        assertTrue(File(scope, "diagnostics-0.jsonl").delete())
        assertTrue(scope.delete())
        Files.createSymbolicLink(scope.toPath(), outside.toPath())
        snapshot.close()
        assertEquals("DO_NOT_DELETE", victim.readText())
        assertFalse(Files.exists(scope.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS))
    }

    @Test fun aggregateBudgetPrioritizesEmulationThenMainOverLegacy() = sandbox { logs, textLog, cache, _ ->
        val emulation = record("emu", 1)
        val main = record("main", 1)
        log(logs, "emulation/diagnostics-0.jsonl", emulation)
        log(logs, "main/diagnostics-0.jsonl", main)
        log(logs, "diagnostics-0.jsonl", record("legacy", 1))
        textLog.writeText("legacy text\n")
        val budget = emulation.toByteArray(Charsets.UTF_8).size + main.toByteArray(Charsets.UTF_8).size
        DiagnosticExport.captureBounded(logs, textLog, cache, "schema=1", budget).use { snapshot ->
            val entries = archive(snapshot)
            assertEquals(emulation, entries["emulation/diagnostics-0.jsonl"])
            assertEquals(main, entries["main/diagnostics-0.jsonl"])
            assertFalse(entries.containsKey("diagnostics-0.jsonl"))
            assertFalse(entries.containsKey("emulation.log"))
            val metadata = entries.getValue("session-metadata.txt")
            assertTrue(metadata.contains("snapshot_exported_log_bytes=$budget"))
            assertTrue(metadata.contains("omitted_file=diagnostics-0.jsonl: snapshot byte limit reached"))
            assertTrue(snapshot.entries.filter { it.name != "session-metadata.txt" }.sumOf { it.bytes } <= budget)
        }
    }

    @Test fun redactionExpansionStillObeysByteLimitAtCompleteJsonBoundaries() = sandbox { logs, textLog, cache, _ ->
        val line = buildJsonObject { putJsonArray("paths") { repeat(6000) { add("/data/x") } } }.toString() + "\n"
        val source = log(logs, "main/diagnostics-0.jsonl", line.repeat(130))
        assertTrue(source.length() <= DiagnosticExport.MAX_FILE_BYTES)
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val entries = archive(snapshot)
            val text = entries.getValue("main/diagnostics-0.jsonl")
            assertTrue(text.toByteArray(Charsets.UTF_8).size <= DiagnosticExport.MAX_FILE_BYTES)
            text.lineSequence().filter { it.isNotEmpty() }.forEach { Json.parseToJsonElement(it).jsonObject }
            val omitted = Regex("omitted_lines_at_export_byte_limit=(\\d+)").find(entries.getValue("session-metadata.txt"))!!
            assertTrue(omitted.groupValues[1].toLong() > 0)
        }
    }

    @Test fun previousSessionCriticalStatesUseFixedNamesAndKeepSourceIdentity() = sandbox { logs, textLog, cache, _ ->
        val state = buildJsonObject {
            put("schema", 1); put("session_id", "prior-session"); put("sequence", 3)
            put("record_kind", "critical_snapshot"); put("event", "CRITICAL_STATE_SNAPSHOT")
            put("launch_id", "prior-launch"); put("base_program_nca", "base-program")
            put("effective_exefs_program_nca", "update-program"); put("effective_exefs_source", "update")
            putJsonObject("snapshot_metadata") { put("observed_event", "LAUNCH_ENDED") }
            putJsonObject("fields") { put("end_reason", "execute_returned"); put("host_stop_requested", "true") }
        }.toString() + "\n"
        log(logs, "main/critical-state-1.jsonl", state)
        log(logs, "emulation/critical-state-7.jsonl", state.replace("prior-session", "another-prior-session"))
        log(logs, "main/critical-state-8.jsonl", state)
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val entries = archive(snapshot)
            assertEquals(state.trim(), entries.getValue("main/critical-state-1.jsonl").trim())
            assertTrue(entries.containsKey("emulation/critical-state-7.jsonl"))
            assertFalse(entries.containsKey("main/critical-state-8.jsonl"))
            assertTrue(entries.getValue("main/critical-state-1.jsonl").contains("prior-launch"))
        }
    }

    @Test fun priorCriticalStateHasTheSame64KiBBoundAsCurrentState() = sandbox { logs, textLog, cache, _ ->
        log(logs, "main/critical-state-1.jsonl", "x".repeat(65537))
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            assertFalse(archive(snapshot).containsKey("main/critical-state-1.jsonl"))
        }
    }

    @Test fun snapshotsRemainValidWhileSourceRotatesAndAppendsPartialLines() = sandbox { logs, textLog, cache, _ ->
        val source = log(logs, "main/diagnostics-0.jsonl", record("concurrent", 1))
        val stop = AtomicBoolean(false)
        val failure = AtomicReference<Throwable?>(null)
        val started = CountDownLatch(1)
        val writer = Thread {
            try {
                var sequence = 2
                started.countDown()
                while (!stop.get()) {
                    source.outputStream().use { output ->
                        val line = record("concurrent", sequence++, detail = "世界 content://private/game").toByteArray(Charsets.UTF_8)
                        val split = line.size / 2
                        output.write(line, 0, split)
                        Thread.yield()
                        output.write(line, split, line.size - split)
                    }
                    Files.move(source.toPath(), File(logs, "main/diagnostics-1.jsonl").toPath(), StandardCopyOption.REPLACE_EXISTING)
                }
            } catch (error: Throwable) { failure.set(error) }
        }
        writer.start(); started.await()
        try {
            repeat(4) {
                DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
                    val entries = archive(snapshot)
                    entries.filterKeys { it.endsWith(".jsonl") }.values.forEach { text ->
                        text.lineSequence().filter { it.isNotBlank() }.forEach { Json.parseToJsonElement(it).jsonObject }
                        assertFalse(text.contains("private"))
                    }
                }
            }
        } finally { stop.set(true); writer.join(5000) }
        assertFalse("Writer did not stop", writer.isAlive)
        failure.get()?.let { throw AssertionError("Source writer failed", it) }
    }
}
