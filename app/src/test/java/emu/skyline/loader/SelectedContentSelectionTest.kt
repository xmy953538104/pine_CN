// SPDX-License-Identifier: MPL-2.0
package emu.skyline.loader

import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test
import java.security.MessageDigest

internal object SelectedPreviewFixture {
    const val Sha256 = "3aefa8e5c4c16f70af5aa05c081bb6c6965d38e4f692c767c8cc1bd67a9068d7"
    fun text() = requireNotNull(javaClass.getResourceAsStream("/selected-content/native-metadata-preview.json"))
        .bufferedReader().use { it.readText() }
    fun json() = Json.parseToJsonElement(text()).jsonObject
    fun catalog(json: JsonObject = json()) = SelectedContentCatalog.parse(json.toString(), SelectedContentProfile.Modern40)
    fun primary(catalog: SelectedContentCatalog) = catalog.programs(catalog.groups.single { it.type == 128 }.id)
        .single().program.choice!!
    fun group(json: JsonObject, role: Int, edit: (JsonObject) -> JsonObject): JsonObject = JsonObject(json +
        ("groups" to JsonArray(json.getValue("groups").jsonArray.map { if (it.jsonObject["type"]?.jsonPrimitive?.int == role) edit(it.jsonObject) else it })))
    fun fields(): Array<String> = catalog().let { c ->
        c.select(primary(c), null, emptyList()).toNativeWire().fields
    }
}

class SelectedContentSelectionTest {
    private fun rejects(block: () -> Unit) {
        try { block(); fail("Expected strict rejection") } catch (expected: IllegalArgumentException) { }
    }

    @Test fun nativeWireAndIntentRoundTripAreExactAndDoNotAutoSelectContents() {
        val catalog = SelectedPreviewFixture.catalog()
        val primary = SelectedPreviewFixture.primary(catalog)
        assertTrue(catalog.updates(primary).any { it.rejection == null })
        assertEquals(2, catalog.addOns(primary).count { it.rejection == null })
        val explicitNone = catalog.select(primary, null, emptyList())
        val wire = explicitNone.toNativeWire()
        assertEquals(10, wire.fields.size); assertArrayEquals(intArrayOf(0, -1), wire.programIndices)
        assertEquals(listOf("", "", ""), wire.fields.slice(7..9))
        assertEquals("1", wire.fields[0]); assertEquals("application", wire.fields[1]); assertEquals("modern-40", wire.fields[2])
        assertEquals(primary.groupId, wire.fields[4]); assertEquals(primary.contentId, wire.fields[5]); assertEquals(primary.programId, wire.fields[6])
        val decoded = SelectedContentSelection.decode(explicitNone.encode()).toNativeWire()
        assertArrayEquals(wire.fields, decoded.fields); assertArrayEquals(wire.programIndices, decoded.programIndices)
    }

    @Test fun exactUpdateAndAocsAreOnlyIncludedAfterExplicitChoices() {
        val catalog = SelectedPreviewFixture.catalog(); val primary = SelectedPreviewFixture.primary(catalog)
        val patch = catalog.updates(primary).single { it.rejection == null }.program.choice!!
        val aocs = catalog.addOns(primary).map { it.group.id }
        val selection = catalog.select(primary, patch, aocs)
        val wire = selection.toNativeWire()
        assertArrayEquals(arrayOf(patch.groupId, patch.contentId, patch.programId), wire.fields.sliceArray(7..9))
        assertEquals(aocs, wire.fields.drop(10)); assertArrayEquals(intArrayOf(primary.index, patch.index), wire.programIndices)
        val standalone = catalog.select(patch, null, aocs)
        assertEquals(SelectedContentMode.StandalonePatch, standalone.mode)
        assertEquals(catalog.groups.single { it.type == 129 }.applicationId, standalone.applicationId)
        assertEquals(patch.groupId, standalone.primary.groupId)
        rejects { catalog.select(patch, patch, emptyList()) }
    }

    @Test fun strictWireRejectsEnumsShapesIdsIndicesAndPartialUpdates() {
        val good = SelectedPreviewFixture.fields()
        for ((index, replacement) in listOf(0 to "2", 1 to "auto", 2 to "auto", 3 to "0".repeat(16),
            4 to "cnmt:bad", 5 to "0".repeat(32), 5 to "x".repeat(32), 6 to "0".repeat(16))) {
            rejects { SelectedContentSelection.fromNativeWire(good.copyOf().apply { this[index] = replacement }, intArrayOf(0, -1)) }
        }
        rejects { SelectedContentSelection.fromNativeWire(good.copyOfRange(0, 9), intArrayOf(0, -1)) }
        rejects { SelectedContentSelection.fromNativeWire(good, intArrayOf(0)) }
        rejects { SelectedContentSelection.fromNativeWire(good, intArrayOf(256, -1)) }
        rejects { SelectedContentSelection.fromNativeWire(good, intArrayOf(-1, -1)) }
        rejects { SelectedContentSelection.fromNativeWire(good, intArrayOf(0, 0)) }
        rejects { SelectedContentSelection.fromNativeWire(good.copyOf().apply { this[7] = good[4] }, intArrayOf(0, 0)) }
        val patch = good.copyOf().apply { this[7] = "cnmt-sha256:" + "b".repeat(64); this[8] = this[5]; this[9] = this[6] }
        rejects { SelectedContentSelection.fromNativeWire(patch, intArrayOf(0, -1)) }
        rejects { SelectedContentSelection.fromNativeWire(patch.copyOf().apply { this[1] = "standalone_patch" }, intArrayOf(0, 0)) }
    }

    @Test fun groupsAreDistinctAndBoundedAcrossAllRolesAndNormalization() {
        val good = SelectedPreviewFixture.fields()
        rejects { SelectedContentSelection.fromNativeWire(good + good[4], intArrayOf(0, -1)) }
        val aoc = "cnmt-sha256:" + "a".repeat(64)
        rejects { SelectedContentSelection.fromNativeWire(good + arrayOf(aoc, aoc.uppercase().replace("CNMT-SHA256:", "cnmt-sha256:")), intArrayOf(0, -1)) }
        val many = Array(33) { "cnmt-sha256:" + it.toString(16).padStart(64, '0') }
        rejects { SelectedContentSelection.fromNativeWire(good + many, intArrayOf(0, -1)) }
    }

    @Test fun intentJsonHasStrictTypesVersionAndBoundedNesting() {
        for (value in listOf("{}", "null", "[[1], [0,-1]]", "[[], [0,-1]]", "[[], [\"0\",-1]]",
            "[".repeat(100) + "0" + "]".repeat(100), " ".repeat(SelectedContentSelection.MaxIntentChars + 1))) {
            rejects { SelectedContentSelection.decode(value) }
        }
        val wire = SelectedPreviewFixture.fields()
        val payload = JsonArray(listOf(JsonArray(wire.map(::JsonPrimitive)), JsonArray(listOf(JsonPrimitive("0"), JsonPrimitive(-1))))).toString()
        rejects { SelectedContentSelection.decode(payload) }
    }

    @Test fun realNativeMetadataPreviewRemainsHashUnverified() {
        val text = SelectedPreviewFixture.text()
        val hash = MessageDigest.getInstance("SHA-256").digest(text.toByteArray()).joinToString("") { "%02x".format(it) }
        assertEquals(SelectedPreviewFixture.Sha256, hash)
        val raw = SelectedPreviewFixture.json()
        assertFalse(raw.getValue("verification").jsonObject.getValue("whole_nca_hash_requested").jsonPrimitive.boolean)
        assertEquals(0L, raw.getValue("verification").jsonObject.getValue("whole_nca_hash_bytes").jsonPrimitive.long)
        assertTrue(raw.getValue("ncas").jsonArray.all { it.jsonObject.getValue("actual_sha256") == JsonNull })
        val catalog = SelectedPreviewFixture.catalog()
        assertTrue(catalog.complete && catalog.nativeInspector); assertEquals(4, catalog.groups.size)
        assertTrue(catalog.groups.flatMap { it.references }.all { it.hashVerification == "unverified" })
        assertTrue(raw.getValue("groups").jsonArray.flatMap { it.jsonObject.getValue("contents").jsonArray }
            .all { it.jsonObject.getValue("resolution").jsonPrimitive.content == "filename_hint_only" })
        rejects { SelectedContentCatalog.parse(text, SelectedContentProfile.Legacy48) }
    }

    @Test fun bktrAndWrongExplicitParentsAreUnavailableWithoutGuesses() {
        val raw = SelectedPreviewFixture.json()
        val patch = raw.getValue("groups").jsonArray.single { it.jsonObject["type"]?.jsonPrimitive?.int == 129 }.jsonObject
        val member = patch.getValue("contents").jsonArray.first().jsonObject.getValue("member").jsonPrimitive.content
        val bktr = JsonObject(raw + ("ncas" to JsonArray(raw.getValue("ncas").jsonArray.map {
            if (it.jsonObject.getValue("id").jsonPrimitive.content != member) it else JsonObject(it.jsonObject +
                ("sections" to JsonArray(it.jsonObject.getValue("sections").jsonArray.map { section -> JsonObject(section.jsonObject + ("requires_base" to JsonPrimitive(true))) })))
        })))
        val blocked = SelectedPreviewFixture.catalog(bktr)
        assertEquals(SelectedRejection.BktrUnsupported, blocked.primaryGroups().single { it.group.type == 129 }.rejection)
        val wrongParent = SelectedPreviewFixture.group(raw, 129) { JsonObject(it + ("explicit_application_id" to JsonPrimitive("0100000000009000"))) }
        val catalog = SelectedPreviewFixture.catalog(wrongParent)
        assertEquals(SelectedRejection.ParentMismatch, catalog.updates(SelectedPreviewFixture.primary(catalog)).single().rejection)
    }

    @Test fun missingOrRenamedUnresolvedReferencesNeverBecomeImplicitHashMatches() {
        val missing = SelectedPreviewFixture.group(SelectedPreviewFixture.json(), 128) { group -> JsonObject(group +
            ("contents" to JsonArray(group.getValue("contents").jsonArray.map { ref ->
                if (ref.jsonObject.getValue("type").jsonPrimitive.int != 1) ref else JsonObject(ref.jsonObject + mapOf(
                    "member" to JsonNull, "candidate_members" to JsonArray(emptyList()), "actual_nca_type" to JsonNull,
                    "actual_nca_size" to JsonNull, "resolution" to JsonPrimitive("missing"), "reference_consistency" to JsonPrimitive("missing")))
            }))) }
        val catalog = SelectedPreviewFixture.catalog(missing)
        assertEquals(SelectedRejection.MissingContent, catalog.primaryGroups().single { it.group.type == 128 }.rejection)
        assertTrue(catalog.programs(catalog.groups.single { it.type == 128 }.id).all { it.rejection != null })
    }

    @Test fun publicDataAndProgramIndexEvidenceAreRequired() {
        val raw = SelectedPreviewFixture.json()
        val unknownIndex = SelectedPreviewFixture.group(raw, 128) { group -> JsonObject(group +
            ("programs" to JsonArray(group.getValue("programs").jsonArray.map { JsonObject(it.jsonObject + ("program_index" to JsonNull)) }))) }
        val unknown = SelectedPreviewFixture.catalog(unknownIndex)
        assertEquals(SelectedRejection.ProgramIdentityUnknown, unknown.programs(unknown.groups.single { it.type == 128 }.id).single().rejection)
        val data = SelectedPreviewFixture.group(raw, 130) { group -> JsonObject(group +
            ("contents" to JsonArray(group.getValue("contents").jsonArray.map { JsonObject(it.jsonObject + ("actual_nca_type" to JsonPrimitive(4))) }))) }
        val unsupported = SelectedPreviewFixture.catalog(data)
        assertTrue(unsupported.addOns(SelectedPreviewFixture.primary(unsupported)).all { it.rejection != null })
    }

    @Test fun zeroContentAndUnknownHashStatesCannotBecomeClickableProgramChoices() {
        val raw = SelectedPreviewFixture.json()
        val zeroId = "0".repeat(32)
        val zero = SelectedPreviewFixture.group(raw, 128) { group -> JsonObject(group + mapOf(
            "contents" to JsonArray(group.getValue("contents").jsonArray.map {
                if (it.jsonObject.getValue("type").jsonPrimitive.int != 1) it else JsonObject(it.jsonObject + ("content_id" to JsonPrimitive(zeroId)))
            }),
            "programs" to JsonArray(group.getValue("programs").jsonArray.map { JsonObject(it.jsonObject + ("content_id" to JsonPrimitive(zeroId))) }))) }
        val unavailable = SelectedPreviewFixture.catalog(zero)
        assertTrue(unavailable.programs(unavailable.groups.single { it.type == 128 }.id).all {
            it.rejection != null && it.program.choice == null
        })
        val unknown = SelectedPreviewFixture.group(raw, 128) { group -> JsonObject(group +
            ("contents" to JsonArray(group.getValue("contents").jsonArray.map { JsonObject(it.jsonObject +
                ("hash_verification" to JsonPrimitive("unknown_future_state"))) }))) }
        val unknownCatalog = SelectedPreviewFixture.catalog(unknown)
        assertEquals(SelectedRejection.UnsupportedContent, unknownCatalog.primaryGroups().single { it.group.type == 128 }.rejection)
        val valid = SelectedPreviewFixture.catalog()
        rejects { valid.select(SelectedPreviewFixture.primary(valid).copy(groupId = "cnmt-sha256:" + "f".repeat(64)), null, emptyList()) }
    }
}
