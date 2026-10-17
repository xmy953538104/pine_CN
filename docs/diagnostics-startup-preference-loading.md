# Independent startup preference repair

This branch is separate from the frozen e434 APK. JVM tests shadow JNI only;
they do not establish an Android ANR fix, native execution or game performance.
No APK build, signing credentials or release is part of its workflow.

Application startup schedules its diagnostics preference read on PineDiagPrefs.
The initial state is off. One typed SharedPreferences.all snapshot is read without
the Diagnostics or EmulationGeneration session monitor. A read, type or scheduling
failure remains off. This removes the added Application main-thread disk wait.

Configuration request order and session-stop order are separate. The latest
request owns one immutable preference snapshot. A completed read can cache that
latest snapshot after an old session ends, but its stale stop generation prevents
it from configuring or reviving that finished session. Older requests cannot
overwrite a newer user request, including one still waiting for its preference
read. A stale launch owner cannot invalidate the current request.

EmulationActivity uses prepareLaunch outside the generation session monitor.
Preparation reads and remembers preferences; it has no native configuration or
launch side effect. Only its returned action, inside the accepted
generations.session(token), publishes configuration and calls beginLaunch under
the Diagnostics monitor. An already-owned A drain completes before B can publish.
If B is cancelled or superseded while reading, the action never runs. Existing
descriptor cleanup and the post-action native-execution ownership check remain.

B restores its prepared configuration after A stops when it is still the latest
request. If a newer user configuration has completed, B uses that complete
snapshot instead. If the newer request is pending, B remains conservatively off
and attaches that request to its newly owned session, so its eventual completion
is neither lost nor replaced by B's older enabled snapshot. A late enable records
launch_start_observed=false and the existing LAUNCH_CONTEXT_OBSERVED event; it
does not claim that diagnostics observed B's start.

The first05bead67 repair moved the whole apply outside the generation session.
That ordering was unsafe: B could return early for A's same enabled signature,
or B's pending read could be invalidated by A's finish, after which B began with
diagnostics off. The test-firstfde5c4b1 commit preserves that behavior behind the
same prepare/owned-action wiring so two deterministic tests can demonstrate the
failure. The follow-up changes only the preference/publication/lifecycle wiring.

The regressions use real Diagnostics and EmulationGeneration with controlled
preference latches and JNI shadows. They hold A's already accepted drain, start
B, and release A; both a completed B preparation and a still-blocked B read must
produce B's enabled association, new Session, SESSION_METADATA and LAUNCH_BEGIN.
Additional cases cover both monitors remaining available, cancellation before
publication, newer completed or pending enable/disable settings, and no restart
from an explicit read after finish without an owned replacement. The original
startup/typed-snapshot/default-off/race/failure tests remain.

Native configuration and marker-file work still occur on the owned launch worker
as in the original lifecycle. This change does not claim to remove every possible
native or filesystem delay from that existing owned section.
