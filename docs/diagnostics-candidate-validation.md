# Integrated Diagnostics candidate validation

This candidate integrates the independently reviewed startup repair, fresh UI
observations and bounded SAF navigation. Its APK, symbols and runtime evidence
must identify its own source commit. Earlier e434 APK runs do not validate this
changed binary. No game loading/storage selection, clocks, legacy logger policy
or commercial resources change. Default Debug remains O0; O2 is an explicit
Debug experiment with assertions, debug information and protection retained.

The startup repair reads one typed preference snapshot off-main and outside the
diagnostic/generation monitors. Configuration publication and beginLaunch happen
only within the accepted launch session, after an older owned drain. Newer user
settings win, cancelled launches do not publish, and a late read alone cannot
revive a finished session. See [the lifecycle contract](diagnostics-startup-preference-loading.md).
The independent regression control ran112 tests with exactly the two new owned
drain tests failing; corrected source5f5e ran116 with no failures/errors/skips.
The integrated APK verifier requires that startup suite and its two regressions;
the integrated source must rerun the full JVM suite.

The observer uses a unique absent path and positive success marker for every
UIAutomator dump and rejects errors in either output stream. No old XML fallback
is allowed. Installation requires a successful empty package-list query, including
uninstalled packages with retained data; unknown queries never authorize install.
Nothing is uninstalled or cleared. SAF selects a real clickable Downloads row
inside the system roots list and waits for enabled Save and the exact current
Session filename. Disabled Save is never tapped. ZIP allowlist/CRC/snapshot-byte
checks and actual native Stop remain mandatory. A narrowly identified pending
picker may be cancelled once normally without turning an export blocker into a
pass. Directory/mount probes alone do not prove document-provider readiness.

Retain these actual e434 outcomes separately:

| Attempt | Pass / blocked / failed | Evidence and limitation |
|---|---:|---|
| Original O2 Mac | 5 /13 /0 | [artifact12108292686](https://github.com/xmy953538104/pine_CN/actions/runs/39051726518/artifacts/12108292686): persistent Wellbeing ANR; old XML freshness defect. |
| O0 Linux | 5 /12 /1 | [artifact12108727134](https://github.com/xmy953538104/pine_CN/actions/runs/39051729605/artifacts/12108727134): real Pine SharedPreferences startup ANR, not exempted as platform failure. |
| O0 Mac | reported18 /0 /0 | [artifact12108449514](https://github.com/xmy953538104/pine_CN/actions/runs/39051729605/artifacts/12108449514): actual native logs and SAF ZIP, but old observer freshness limitation. |
| Explicit Wellbeing profile | 15 /3 /0 | [artifact12109904813](https://github.com/xmy953538104/pine_CN/actions/runs/39055917266/artifacts/12109904813): Settings/native logs/configuration/export snapshot passed; provider error disabled Save, ZIP and Stop blocked. |
| Corrected SAF observer | 5 /13 /0 | [artifact12112096195](https://github.com/xmy953538104/pine_CN/actions/runs/39059936995/artifacts/12112096195): actual FUSE gate passed, two ordinary system Wait actions, then real Launcher. Pine PID5874 failed to attach/start and was killed; no Pine ANR entry. No SAF action executed; do not attribute this case to preferences. |

The test matrix itself was unreliable. The retained Linux runner had4 logical
CPUs/16GiB and /dev/kvm root:kvm0660, but the emulator accelerator probe returned11
because the runner lacked access; it used software CPU emulation. Intel macOS
attempts also suffered severe system-app/start-attach failures despite the
Hypervisor.Framework probe. Repeating that matrix cannot establish reliability.
The selected local executor has no /dev/kvm, /dev/dri or exposed VMX/SVM. Inspected
official ATD configuration disabled the native bridge and advertised x86_64 only;
it is not an established ARM64 replacement. No image/security overrides are used.

CI now selects Linux and requires a successful official hardware-acceleration
probe before AVD creation. A failure writes an exact APK/source-bound report with
all18 APK checks blocked and no installation. A green evidence-collection job is
not an APK pass. No automatic chmod, ACL, group-database, sudo or security change
is performed. A separate explicit profile may disable only identified optional
Wellbeing on the fresh job-owned API35 AVD, after all existing ownership, package
absence and global error-visibility checks. Owner devices never get this profile.

A reliable next execution route is an authorized physical ARM64 device with the
official SDK ADB/UIAutomator, or a CI host with usable hardware virtualization.
Using an existing CI kvm group for only the same nonroot emulator child process
can be reviewed separately; it requires explicit authorization before any privilege
grant. No paid device/project, new account, unknown software or permission change
is provisioned to obtain a pass. Android UI/native-log/SAF testing does not validate
physical ARM64 NCE, legal-game content, simulation speed or audio. Those remain
separate acceptance tasks; presentation FPS and host microbenchmarks do not prove
the owner's reported game slowdown is resolved.
