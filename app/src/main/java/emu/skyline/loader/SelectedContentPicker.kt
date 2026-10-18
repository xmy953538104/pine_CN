// SPDX-License-Identifier: MPL-2.0
package emu.skyline.loader

import android.content.ContentResolver
import android.content.Context
import android.net.Uri
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.TextView
import android.widget.ListView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.Fragment
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import emu.skyline.R
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.FileNotFoundException

internal fun inspectSelectedContent(resolver: ContentResolver, uri: Uri, privateFilesPath: String,
    profile: SelectedContentProfile, inspect: (Int, String, String) -> String = SelectedContentBridge::inspect): SelectedContentCatalog {
    // inspect borrows fd. No detach/adopt or second Java ownership of it.
    return (resolver.openFileDescriptor(uri, "r") ?: throw FileNotFoundException("selected_input_unavailable")).use { descriptor ->
        SelectedContentCatalog.parse(inspect(descriptor.fd, privateFilesPath, profile.wire), profile)
    }
}

/** One explicit, lifecycle-owned chooser; cancellation never publishes a late scan. */
internal class SelectedContentPicker(private val fragment: Fragment, private val uri: Uri,
    private val launch: (SelectedContentSelection) -> Unit, private val finished: () -> Unit) : DefaultLifecycleObserver {
    private val owner = fragment.viewLifecycleOwner
    private val context: Context = fragment.requireContext()
    private var dialog: AlertDialog? = null
    private var scan: Job? = null
    private var active = true
    private var released = false
    private data class Row(val text: String, val rejection: SelectedRejection? = null)

    init { owner.lifecycle.addObserver(this) }

    fun show() {
        choose(context.getString(R.string.selected_profile_title), listOf(
            Row(context.getString(R.string.selected_profile_modern)), Row(context.getString(R.string.selected_profile_legacy)))) { index ->
            inspect(if (index == 0) SelectedContentProfile.Modern40 else SelectedContentProfile.Legacy48)
        }
    }

    override fun onDestroy(owner: LifecycleOwner) = cancel()

    private fun cancel() {
        active = false
        dialog?.dismiss(); dialog = null
        scan?.cancel()
        if (scan == null || scan?.isCompleted == true) release()
    }

    private fun release() {
        if (released) return
        released = true
        owner.lifecycle.removeObserver(this)
        finished()
    }

    private fun inspect(profile: SelectedContentProfile) {
        dialog?.dismiss()
        dialog = MaterialAlertDialogBuilder(context).setTitle(R.string.selected_scanning)
            .setMessage(R.string.selected_scan_caveat).setNegativeButton(android.R.string.cancel) { _, _ -> cancel() }
            .setOnCancelListener { cancel() }.show()
        val app = context.applicationContext
        scan = owner.lifecycleScope.launch {
            try {
                val catalog = withContext(Dispatchers.IO) {
                    inspectSelectedContent(app.contentResolver, uri, app.filesDir.canonicalPath + "/", profile)
                }
                if (active) { dialog?.dismiss(); primary(catalog) }
            } catch (error: CancellationException) { throw error }
            catch (error: Exception) { failure() }
            catch (error: LinkageError) { failure() }
            finally { if (!active) release() }
        }
    }

    private fun failure() {
        if (!active) return
        dialog?.dismiss()
        dialog = MaterialAlertDialogBuilder(context).setTitle(R.string.selected_content_title)
            .setMessage(R.string.selected_scan_failed).setPositiveButton(android.R.string.ok) { _, _ -> cancel() }
            .setOnCancelListener { cancel() }.show()
    }

    private fun role(group: SelectedCatalogGroup) = context.getString(when (group.type) {
        128 -> R.string.selected_role_application
        129 -> R.string.selected_role_patch
        else -> R.string.selected_role_aoc
    })

    private fun label(group: SelectedCatalogGroup) = "${role(group)}\n${group.titleId ?: "?"}  v${group.version ?: "?"}\n${group.id}"
    private fun label(candidate: SelectedProgramCandidate) = label(candidate.group) + "\n" +
        context.getString(R.string.selected_program_identity, candidate.program.choice?.programId ?: "?",
            candidate.program.choice?.index?.toString() ?: "?", candidate.program.contentId)

    private fun rejection(reason: SelectedRejection) = context.getString(when (reason) {
        SelectedRejection.BktrUnsupported -> R.string.selected_reason_bktr
        SelectedRejection.ParentMismatch, SelectedRejection.UpdateIdentityMismatch -> R.string.selected_reason_parent
        SelectedRejection.Ambiguous -> R.string.selected_reason_ambiguous
        SelectedRejection.IncompleteCatalog -> R.string.selected_reason_incomplete
        SelectedRejection.MissingContent, SelectedRejection.ControlRequired, SelectedRejection.PublicDataRequired -> R.string.selected_reason_missing
        SelectedRejection.ProgramIdentityUnknown -> R.string.selected_reason_program
        else -> R.string.selected_reason_unsupported
    })

    private fun choose(title: String, rows: List<Row>, selected: (Int) -> Unit) {
        if (!active) return
        if (rows.isEmpty()) {
            dialog = MaterialAlertDialogBuilder(context).setTitle(title).setMessage(R.string.selected_no_candidates)
                .setPositiveButton(android.R.string.ok) { _, _ -> cancel() }.setOnCancelListener { cancel() }.show()
            return
        }
        val labels = rows.map { it.text + (it.rejection?.let { reason -> "\n${rejection(reason)}" } ?: "") }
        val adapter = object : ArrayAdapter<String>(context, android.R.layout.simple_list_item_1, labels) {
            override fun areAllItemsEnabled() = false
            override fun isEnabled(position: Int) = rows[position].rejection == null
            override fun getView(position: Int, convertView: View?, parent: ViewGroup): View = super.getView(position, convertView, parent).apply {
                alpha = if (isEnabled(position)) 1f else .5f
                (this as TextView).apply { setSingleLine(false); maxLines = 10; textSize = 13f }
            }
        }
        dialog = MaterialAlertDialogBuilder(context).setTitle(title).setAdapter(adapter) { _, index ->
            if (rows[index].rejection == null && active) selected(index)
        }.setNegativeButton(android.R.string.cancel) { _, _ -> cancel() }.setOnCancelListener { cancel() }.show()
    }

    private fun primary(catalog: SelectedContentCatalog) {
        val groups = catalog.primaryGroups()
        choose(context.getString(R.string.selected_primary_title), groups.map {
            Row((if (it.group.type == 129) context.getString(R.string.selected_role_standalone_patch) + "\n" else "") + label(it.group), it.rejection)
        }) { index ->
            val programs = catalog.programs(groups[index].group.id)
            choose(context.getString(R.string.selected_program_title), programs.map { Row(label(it), it.rejection) }) { program ->
                update(catalog, requireNotNull(programs[program].program.choice))
            }
        }
    }

    private fun update(catalog: SelectedContentCatalog, primary: SelectedProgramChoice) {
        val group = catalog.groups.single { it.id == primary.groupId }
        if (group.type == 129) { addOns(catalog, primary, null); return }
        val updates = catalog.updates(primary)
        choose(context.getString(R.string.selected_update_title), listOf(Row(context.getString(R.string.selected_no_update))) +
            updates.map { Row(label(it), it.rejection) }) { index ->
            addOns(catalog, primary, if (index == 0) null else requireNotNull(updates[index - 1].program.choice))
        }
    }

    private fun addOns(catalog: SelectedContentCatalog, primary: SelectedProgramChoice, update: SelectedProgramChoice?) {
        val candidates = catalog.addOns(primary)
        val selected = linkedSetOf<String>()
        if (!active) return
        val labels = candidates.map { label(it.group) + (it.rejection?.let { reason -> "\n${rejection(reason)}" } ?: "") }.toTypedArray()
        val list = ListView(context).apply {
            choiceMode = ListView.CHOICE_MODE_MULTIPLE
            adapter = object : ArrayAdapter<String>(context, android.R.layout.simple_list_item_multiple_choice, labels) {
                override fun areAllItemsEnabled() = false
                override fun isEnabled(position: Int) = candidates[position].rejection == null
                override fun getView(position: Int, convertView: View?, parent: ViewGroup): View = super.getView(position, convertView, parent).apply {
                    alpha = if (isEnabled(position)) 1f else .5f
                    (this as TextView).apply { setSingleLine(false); maxLines = 10; textSize = 13f }
                }
            }
            setOnItemClickListener { _, _, index, _ ->
                val checked = isItemChecked(index)
                if (checked && selected.size >= SelectedContentSelection.MaxAddOns) {
                    setItemChecked(index, false)
                    Toast.makeText(context, R.string.selected_aoc_limit, Toast.LENGTH_SHORT).show()
                } else if (checked) selected.add(candidates[index].group.id) else selected.remove(candidates[index].group.id)
            }
        }
        val builder = MaterialAlertDialogBuilder(context).setTitle(R.string.selected_aoc_title)
        if (candidates.isEmpty()) builder.setMessage(R.string.selected_no_aocs) else builder.setView(list)
        dialog = builder
            .setPositiveButton(R.string.selected_review) { _, _ ->
                if (active) review(catalog.select(primary, update, selected.toList()))
            }.setNegativeButton(android.R.string.cancel) { _, _ -> cancel() }.setOnCancelListener { cancel() }.show()
    }

    private fun review(selection: SelectedContentSelection) {
        if (!active) return
        val primary = selection.primary
        val mode = context.getString(if (selection.mode == SelectedContentMode.Application) R.string.selected_role_application else R.string.selected_role_standalone_patch)
        val summary = "$mode\n${selection.applicationId}\n" +
            context.getString(R.string.selected_program_identity, primary.programId, primary.index.toString(), primary.contentId) +
            "\n${primary.groupId}\n" +
            context.getString(R.string.selected_review_update, selection.update?.let { "${it.groupId}\n${it.programId} / ${it.index}\n${it.contentId}" }
                ?: context.getString(R.string.selected_no_update)) + "\n" +
            context.getString(R.string.selected_review_aocs, selection.addOnGroupIds.size) +
            selection.addOnGroupIds.joinToString(separator = "\n", prefix = if (selection.addOnGroupIds.isEmpty()) "" else "\n") +
            "\n\n" + context.getString(R.string.selected_scan_caveat)
        dialog = MaterialAlertDialogBuilder(context).setTitle(R.string.selected_review).setMessage(summary)
            .setPositiveButton(R.string.selected_validate_launch) { _, _ ->
                if (active) { active = false; release(); launch(selection) }
            }.setNegativeButton(android.R.string.cancel) { _, _ -> cancel() }.setOnCancelListener { cancel() }.show()
    }
}
