// SPDX-License-Identifier: MPL-2.0
// Transport/collection tests only. No result here is Android execution evidence.
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const { runOnce, execute } = require('../android-bionic/run-once.js');
const source = '55b3fab139e0891d3278715406ed1f843686b687';
const names = ['pine-boundary', 'pine-lifecycle', 'pine-registers', 'libc++_shared.so'];
const digest = value => crypto.createHash('sha256').update(value).digest('hex');
test('real host process preserves argv/metacharacters, stderr and exit37 without a shell', async () => {
  const args = ['path with spaces', 'literal"quote', 'semi;colon', '中文', '$(not-a-command)'];
  const result = await execute(process.execPath, ['-e', 'console.log(JSON.stringify(process.argv.slice(1)));console.error("owned-process");process.exit(37)', ...args], 10000);
  assert.equal(result.code, 37); assert.deepEqual(JSON.parse(result.stdout), args); assert.match(result.stderr, /owned-process/);
});
test('real host process timeout remains uncertain/nonzero and is not retried', async () => {
  const result = await execute(process.execPath, ['-e', 'setTimeout(()=>{},10000)'], 100);
  assert.equal(result.code, null); assert.equal(result.timedOut, true);
});
async function fixture(mode = 'pass', devices = 'SERIAL device\n') {
  const bundle = await fs.mkdtemp(path.join(os.tmpdir(), 'pine bionic collector '));
  await fs.mkdir(path.join(bundle, 'device'));
  const files = [];
  for (const name of names) { await fs.writeFile(path.join(bundle, 'device', name), name); files.push({ name: 'device/' + name, sha256: digest(name) }); }
  await fs.writeFile(path.join(bundle, 'manifest.json'), JSON.stringify({ schema: 1, source_commit: source, source_dirty: false, files }));
  const calls = []; let remote, runId;
  const transport = async (adb, args) => {
    calls.push(args);
    let stdout = '';
    if (args[0] === 'version') stdout = 'Android Debug Bridge version 1.0.41';
    else if (args[0] === 'devices') stdout = 'List of devices attached\n' + devices;
    else if (args[2] === 'push') { if (mode === 'push-fail') return { code: 1, stdout: '', stderr: 'transport lost' }; }
    else if (args[2] === 'pull') {
      if (mode === 'pull-fail') return { code: 1, stdout: '', stderr: 'offline' };
      if (mode !== 'missing-result') {
        await fs.mkdir(args[4], { recursive: true });
        const code = mode === 'crash' ? 134 : 0;
        const result = { schema: 1, run_id: runId, source_commit: mode === 'wrong-commit' ? 'a'.repeat(40) : source, android_execution: mode !== 'linux-domain', scope: 'isolated_fixture_not_pine_app_or_horizon', status: code ? 'FAIL' : 'PASS', exit_code: code, tests: ['lifecycle', 'registers'].map((name, index) => ({ component: name, exit_code: code && index === 0 ? code : 0, signal: code && index === 0 ? 6 : 0, result: { status: 'PASS', source_commit: source, android_execution: true } })) };
        await fs.writeFile(path.join(args[4], 'android-result.json'), JSON.stringify(result));
      }
    } else if (args[2] === 'shell') {
      const command = args[3];
      if (command === 'getprop ro.build.version.sdk') stdout = mode === 'old-api' ? '28' : '35';
      else if (command === 'getprop ro.product.cpu.abilist') stdout = mode === 'wrong-abi' ? 'armeabi-v7a' : 'arm64-v8a,armeabi-v7a';
      else if (command === 'uname -m') stdout = 'aarch64';
      else if (command.startsWith('mkdir -m 700 ')) { remote = command.slice('mkdir -m 700 '.length); runId = remote.slice('/data/local/tmp/pine-bionic-'.length); }
      else if (command.startsWith('sha256sum ')) stdout = names.map(name => (mode === 'remote-hash' ? '0'.repeat(64) : digest(name)) + '  ' + remote + '/' + name).join('\n');
      else if (command.includes('./pine-boundary ')) {
        if (mode === 'timeout') return { code: null, timedOut: true, stdout: '', stderr: 'timeout' };
        if (mode === 'crash') return { code: 134, stdout: '', stderr: 'owned child aborted' };
      }
    }
    return { code: 0, stdout, stderr: '' };
  };
  const execute = options => runOnce({ bundle, transport, ...options });
  return { bundle, calls, execute };
}
test('single authorized run validates identity and collects once; only owned directory touched', async () => {
  const f = await fixture(); const report = await f.execute();
  assert.equal(report.status, 'ANDROID_BIONIC_EXECUTED_PASS'); assert.equal(report.exit_code, 0);
  assert.equal(report.native_execution_attempts, 1);
  assert.equal(f.calls.filter(args => args[2] === 'shell' && args[3].includes('./pine-boundary ')).length, 1);
  assert.equal(f.calls.filter(args => args[2] === 'pull').length, 1);
  assert.match(report.remote_directory, /^\/data\/local\/tmp\/pine-bionic-[a-f0-9]{32}$/);
  for (const args of f.calls) assert(!args.some(value => /logcat|\/sdcard|\/data\/data|setprop|reboot|kill-server|\bsu\b/.test(value)));
  assert.equal(JSON.parse(await fs.readFile(path.join(report.output_directory, 'result.json'))).status, report.status);
});
for (const [mode, expected, attempted] of [
  ['crash', 'ANDROID_EXECUTION_FAILED', 1], ['timeout', 'ANDROID_EXECUTION_UNCERTAIN', 1],
  ['missing-result', 'ANDROID_EXECUTION_UNCERTAIN', 1], ['pull-fail', 'ANDROID_EXECUTION_UNCERTAIN', 1],
  ['wrong-commit', 'ANDROID_EXECUTION_UNCERTAIN', 1], ['linux-domain', 'ANDROID_EXECUTION_UNCERTAIN', 1],
  ['old-api', 'ANDROID_EXECUTION_PENDING', 0], ['wrong-abi', 'ANDROID_EXECUTION_PENDING', 0],
  ['remote-hash', 'ANDROID_EXECUTION_PENDING', 0], ['push-fail', 'ANDROID_EXECUTION_PENDING', 0],
]) test(mode + ' retains structured evidence without another execution', async () => {
  const f = await fixture(mode); const report = await f.execute();
  assert.equal(report.status, expected); assert.equal(report.native_execution_attempts, attempted); assert.notEqual(report.exit_code, 0);
  if (mode === 'crash') { assert.equal(report.exit_code, 134); assert.equal(report.adb_execution_exit_code, 134); assert.equal(report.native_exit_code, 134); }
  assert(f.calls.filter(args => args[2] === 'shell' && args[3].includes('./pine-boundary ')).length <= 1);
  assert.equal(JSON.parse(await fs.readFile(path.join(report.output_directory, 'result.json'))).status, expected);
});
for (const devices of ['SERIAL unauthorized\n', 'SERIAL offline\n', '', 'ONE device\nTWO device\n']) test('authorization/selection preflight stops: ' + JSON.stringify(devices), async () => {
  const f = await fixture('pass', devices); const report = await f.execute();
  assert.equal(report.status, 'ANDROID_EXECUTION_PENDING'); assert.equal(report.native_execution_attempts, 0);
  assert(!f.calls.some(args => args[2] === 'push' || args[2] === 'shell'));
});
test('explicit multi-device selection never silently chooses another target', async () => {
  const f = await fixture('pass', 'ONE device\nTWO device\n'); const report = await f.execute({ serial: 'TWO' });
  assert.equal(report.selected_serial, 'TWO'); assert.equal(report.status, 'ANDROID_BIONIC_EXECUTED_PASS');
  assert(f.calls.filter(args => args[0] === '-s').every(args => args[1] === 'TWO'));
});
test('local payload mismatch stops before ADB; failure file survives', async () => {
  const f = await fixture(); await fs.writeFile(path.join(f.bundle, 'device/pine-lifecycle'), 'changed');
  const report = await f.execute(); assert.equal(report.status, 'ANDROID_EXECUTION_PENDING'); assert.equal(f.calls.length, 0); assert(report.error.message.includes('hash mismatch'));
});
