// SPDX-License-Identifier: MPL-2.0
package emu.skyline.loader

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonPrimitive
import java.util.Locale

const val SelectedContentIntentTag = "emu.skyline.SELECTED_CONTENT_V1"

enum class SelectedContentProfile(val wire: String) {
    Modern40("modern-40"), Legacy48("legacy-48");
    companion object {
        fun parse(value: String) = values().singleOrNull { it.wire == value }
            ?: throw IllegalArgumentException("selected_profile_unsupported")
    }
}

enum class SelectedContentMode(val wire: String) {
    Application("application"), StandalonePatch("standalone_patch");
    companion object {
        fun parse(value: String) = values().singleOrNull { it.wire == value }
            ?: throw IllegalArgumentException("selected_mode_unsupported")
    }
}

internal fun selectedHex(value: String, length: Int): String {
    require(value.length == length && value.all { it in '0'..'9' || it in 'a'..'f' || it in 'A'..'F' }) { "selected_invalid_hex" }
    return value.lowercase(Locale.ROOT)
}

internal fun selectedGroupId(value: String): String {
    require(value.startsWith("cnmt-sha256:")) { "selected_invalid_group_id" }
    return "cnmt-sha256:" + selectedHex(value.removePrefix("cnmt-sha256:"), 64)
}

data class SelectedProgramChoice(val groupId: String, val contentId: String, val programId: String, val index: Int) {
    internal fun checked() = SelectedProgramChoice(selectedGroupId(groupId), selectedHex(contentId, 32).also {
        require(it.any { c -> c != '0' }) { "selected_content_id_missing" }
    },
        selectedHex(programId, 16).also { require(it.any { c -> c != '0' }) { "selected_program_id_missing" } },
        index.also { require(it in 0..255) { "selected_program_index_invalid" } })
}

data class SelectedNativeWire(val fields: Array<String>, val programIndices: IntArray)

/** User intent only. Native preparation must re-establish every content binding. */
class SelectedContentSelection private constructor(
    val mode: SelectedContentMode,
    val profile: SelectedContentProfile,
    val applicationId: String,
    val primary: SelectedProgramChoice,
    val update: SelectedProgramChoice?,
    addOnGroupIds: List<String>
) {
    val addOnGroupIds: List<String> = addOnGroupIds.toList()

    fun toNativeWire(): SelectedNativeWire {
        // Check again at the boundary; no caller-owned array/list is reused.
        val checked = create(mode, profile, applicationId, primary, update, addOnGroupIds)
        return SelectedNativeWire(arrayOf("1", checked.mode.wire, checked.profile.wire, checked.applicationId,
            checked.primary.groupId, checked.primary.contentId, checked.primary.programId,
            checked.update?.groupId ?: "", checked.update?.contentId ?: "", checked.update?.programId ?: "") +
            checked.addOnGroupIds.toTypedArray(), intArrayOf(checked.primary.index, checked.update?.index ?: -1))
    }

    /** Fixed two-array JSON shape has no object keys or duplicate-key ambiguity. */
    fun encode(): String = toNativeWire().let { wire ->
        JsonArray(listOf(JsonArray(wire.fields.map(::JsonPrimitive)),
            JsonArray(wire.programIndices.map(::JsonPrimitive)))).toString()
    }

    companion object {
        const val MaxAddOns = 32
        const val MaxIntentChars = 8192

        fun create(mode: SelectedContentMode, profile: SelectedContentProfile, applicationId: String,
                   primary: SelectedProgramChoice, update: SelectedProgramChoice? = null,
                   addOnGroupIds: List<String> = emptyList()): SelectedContentSelection {
            val app = selectedHex(applicationId, 16)
            require(app.any { it != '0' }) { "selected_application_id_missing" }
            val base = primary.checked()
            val patch = update?.checked()
            require(mode != SelectedContentMode.StandalonePatch || patch == null) { "selected_standalone_update_forbidden" }
            require(addOnGroupIds.size <= MaxAddOns) { "selected_aoc_count_exceeded" }
            val aocs = addOnGroupIds.map(::selectedGroupId)
            val groups = listOf(base.groupId) + listOfNotNull(patch?.groupId) + aocs
            require(groups.distinct().size == groups.size) { "selected_group_reused" }
            return SelectedContentSelection(mode, profile, app, base, patch, aocs)
        }

        fun fromNativeWire(fields: Array<String>, indices: IntArray): SelectedContentSelection {
            require(fields.size in 10..(10 + MaxAddOns) && indices.size == 2 && fields.all { it.length <= 76 }) { "selected_wire_shape" }
            require(fields[0] == "1") { "selected_version_unsupported" }
            val mode = SelectedContentMode.parse(fields[1])
            val profile = SelectedContentProfile.parse(fields[2])
            val hasUpdate = fields.slice(7..9).any { it.isNotEmpty() }
            require(if (hasUpdate) fields.slice(7..9).all { it.isNotEmpty() } && indices[1] in 0..255 else indices[1] == -1) { "selected_update_shape" }
            return create(mode, profile, fields[3], SelectedProgramChoice(fields[4], fields[5], fields[6], indices[0]),
                if (hasUpdate) SelectedProgramChoice(fields[7], fields[8], fields[9], indices[1]) else null,
                fields.drop(10))
        }

        fun decode(value: String): SelectedContentSelection {
            checkJsonEnvelope(value, MaxIntentChars, 2)
            try {
                val root = Json.parseToJsonElement(value) as? JsonArray ?: error("shape")
                require(root.size == 2)
                val fields = root[0] as? JsonArray ?: error("fields")
                val indices = root[1] as? JsonArray ?: error("indices")
                require(fields.size in 10..(10 + MaxAddOns) && indices.size == 2)
                return fromNativeWire(fields.map {
                    val text = it as? JsonPrimitive ?: error("text")
                    require(text.isString); text.content
                }.toTypedArray(), indices.map {
                    val number = it as? JsonPrimitive ?: error("number")
                    require(!number.isString)
                    number.content.toIntOrNull() ?: error("integer")
                }.toIntArray())
            } catch (error: Exception) {
                throw IllegalArgumentException("selected_payload_invalid")
            }
        }
    }
}

/** Bound parser recursion before parsing any supplied JSON. Escaped strings are skipped. */
internal fun checkJsonEnvelope(value: String, maximum: Int, maxDepth: Int) {
    require(value.length in 1..maximum) { "selected_json_budget" }
    var quoted = false
    var escaped = false
    var depth = 0
    value.forEach { c ->
        if (quoted) {
            when { escaped -> escaped = false; c == '\\' -> escaped = true; c == '"' -> quoted = false }
        } else when (c) {
            '"' -> quoted = true
            '[', '{' -> { depth++; require(depth <= maxDepth) { "selected_json_depth" } }
            ']', '}' -> { depth--; require(depth >= 0) { "selected_json_shape" } }
        }
    }
    require(!quoted && depth == 0) { "selected_json_shape" }
}
