// SPDX-License-Identifier: MPL-2.0
'use strict';
const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const { execFile } = require('node:child_process');
const readline = require('node:readline/promises');

const payloadNames = ['pine-boundary', 'pine-lifecycle', 'pine-registers', 'libc++_shared.so'];
async function hashFile(file) { return crypto.createHash('sha256').update(await fs.readFile(file)).digest('hex'); }
async function writeJson(file, value) { await fs.writeFile(file + '.tmp', JSON.stringify(value, null, 2) + '\n'); await fs.rename(file + '.tmp', file); }
function execute(file, args, timeout) {
  return new Promise(resolve => execFile(file, args, { timeout, maxBuffer: 4 * 1024 * 1024, windowsHide: true, encoding: 'utf8' }, (error, stdout, stderr) => {
    resolve({ code: error ? (Number.isInteger(error.code) ? error.code : null) : 0, stdout, stderr, timedOut: Boolean(error?.killed), signal: error?.signal || null, error: error?.message || null });
  }));
}
function devicesFrom(text) {
  return text.split(/\r?\n/).flatMap(line => { const found = /^(\S+)\s+(device|unauthorized|offline|recovery|sideload|bootloader|authorizing|connecting|no permissions)(?:\s|$)/.exec(line); return found ? [{ serial: found[1], state: found[2] }] : []; });
}
async function runOnce(options) {
  const bundle = path.resolve(options.bundle);
  const root = path.resolve(options.outputRoot || path.join(bundle, 'results'));
  await fs.mkdir(root, { recursive: true });
  const output = await fs.mkdtemp(path.join(root, 'pine-bionic-'));
  const runId = crypto.randomBytes(16).toString('hex');
  const remote = '/data/local/tmp/pine-bionic-' + runId;
  const report = { schema: 1, status: 'ANDROID_EXECUTION_PENDING', run_id: runId, source_commit: null, remote_directory: remote, output_directory: output, native_execution_attempts: 0, native_exit_code: null, adb_execution_exit_code: null, collection: 'not_started', observer_installation: 'disabled', runtime_connected: false, real_game_records: 0, actions: [], error: null };
  let adb = options.adb || path.join(bundle, 'platform-tools', process.platform === 'win32' ? 'adb.exe' : 'adb');
  let selected;
  const transport = options.transport || execute; // test injection is module-only, never a CLI option
  const persist = () => writeJson(path.join(output, 'result.json'), report);
  const call = async (args, timeout = 15000) => {
    const result = await transport(adb, args, timeout);
    const entry = { argv: args, code: result.code, timed_out: result.timedOut || false, signal: result.signal || null, stdout: result.stdout || '', stderr: result.stderr || '', error: result.error || null };
    report.actions.push(entry); await persist(); return result;
  };
  const success = (result, stage) => { if (result.code !== 0) throw Object.assign(new Error(stage + ': ' + (result.stderr || result.error || 'ADB failed')), { stage }); return result.stdout.trim(); };
  let created = false;
  try {
    const manifest = JSON.parse(await fs.readFile(path.join(bundle, 'manifest.json'), 'utf8'));
    if (manifest.schema !== 1 || manifest.source_dirty !== false || !/^[a-f0-9]{40}$/.test(manifest.source_commit)) throw new Error('Invalid/development bundle manifest; exact clean source required');
    report.source_commit = manifest.source_commit;
    const knownFiles = new Set();
    for (const file of manifest.files) {
      if (!/^[A-Za-z0-9_./+-]+$/.test(file.name) || file.name.split('/').includes('..') || path.isAbsolute(file.name)) throw new Error('Unsafe manifest filename');
      if (!/^[a-f0-9]{64}$/.test(file.sha256) || await hashFile(path.join(bundle, file.name)) !== file.sha256) throw new Error('Bundle hash mismatch: ' + file.name);
      knownFiles.add(file.name);
    }
    for (const name of payloadNames) if (!knownFiles.has('device/' + name)) throw new Error('Missing required payload: ' + name);
    report.bundle_hashes_verified = true;
    if (!options.adb) { try { await fs.access(adb); } catch { adb = 'adb'; } }
    success(await call(['version']), 'adb_version');
    const devices = devicesFrom(success(await call(['devices', '-l']), 'adb_discovery'));
    report.discovered_devices = devices;
    if (options.serial) selected = devices.find(device => device.serial === options.serial);
    else if (devices.length === 1) selected = devices[0];
    else if (devices.length > 1 && options.chooseDevice) {
      const serial = await options.chooseDevice(devices); selected = devices.find(device => device.serial === serial);
    }
    if (!selected) throw Object.assign(new Error(devices.length ? 'Multiple devices: select exactly one with -Serial / --serial.' : 'No ADB device found.'), { stage: 'device_selection' });
    if (selected.state !== 'device') throw Object.assign(new Error('Device state is ' + selected.state + '. Authorization, if needed, must be approved by the Owner; no test was run.'), { stage: 'device_authorization' });
    report.selected_serial = selected.serial;
    const args = command => ['-s', selected.serial, ...command];
    const api = success(await call(args(['shell', 'getprop ro.build.version.sdk'])), 'android_api');
    const abi = success(await call(args(['shell', 'getprop ro.product.cpu.abilist'])), 'android_abi');
    const machine = success(await call(args(['shell', 'uname -m'])), 'android_machine');
    if (!/^\d+$/.test(api) || Number(api) < 29 || !abi.split(',').includes('arm64-v8a') || machine !== 'aarch64') throw Object.assign(new Error('Requires Android API29+, arm64-v8a and aarch64 kernel; native test was not started.'), { stage: 'device_capability' });
    report.device = { api: Number(api), abi_list: abi, machine };
    success(await call(args(['shell', 'mkdir -m 700 ' + remote])), 'create_unique_directory'); created = true;
    for (const name of payloadNames) success(await call(args(['push', path.join(bundle, 'device', name), remote + '/' + name]), 60000), 'push_' + name);
    success(await call(args(['shell', 'chmod 700 ' + payloadNames.slice(0, 3).map(name => remote + '/' + name).join(' ')])), 'payload_permissions');
    const hashes = success(await call(args(['shell', 'sha256sum ' + payloadNames.map(name => remote + '/' + name).join(' ')])), 'device_payload_hashes');
    for (const name of payloadNames) {
      const expected = manifest.files.find(file => file.name === 'device/' + name).sha256;
      if (!hashes.split(/\r?\n/).some(line => line.trim().split(/\s+/)[0] === expected && line.trim().endsWith(remote + '/' + name))) throw new Error('Device payload hash mismatch: ' + name);
    }
    report.device_hashes_verified = true; report.status = 'ANDROID_EXECUTION_REQUESTED'; report.native_execution_attempts = 1; await persist();
    // The sole execution action. No retry, reboot, adb root, settings change,
    // Pine launch/data access, private logcat or game-file operation exists.
    const result = await call(args(['shell', 'cd ' + remote + ' && LD_LIBRARY_PATH=. ./pine-boundary ' + runId]), options.timeout || 240000);
    report.adb_execution_exit_code = result.code;
    report.execution_transport_timed_out = result.timedOut || false;
    report.status = result.timedOut || result.code === null ? 'ANDROID_EXECUTION_UNCERTAIN' : 'ANDROID_EXECUTION_FAILED';
    await persist();
  } catch (error) {
    report.error = { stage: error.stage || 'preflight_or_execution', message: error.message };
  } finally {
    if (created) {
      try {
        report.collection = 'attempted';
        const result = await call(['-s', selected.serial, 'pull', remote, path.join(output, 'device')], 60000);
        report.collection = result.code === 0 ? 'collected' : 'failed';
        if (result.code !== 0) report.collection_error = result.stderr || result.error;
      } catch (error) { report.collection = 'failed'; report.collection_error = error.message; }
    }
    if (report.native_execution_attempts) {
      try {
        let file = path.join(output, 'device', 'android-result.json');
        try { await fs.access(file); } catch { file = path.join(output, 'device', path.posix.basename(remote), 'android-result.json'); }
        const native = JSON.parse(await fs.readFile(file, 'utf8'));
        report.native_result = native;
        const identity = native.run_id === runId && native.source_commit === report.source_commit && native.android_execution === true && native.scope === 'isolated_fixture_not_pine_app_or_horizon';
        if (!identity) throw new Error('Native result identity/domain mismatch');
        const complete = native.tests?.length === 2 && native.tests.every((test, index) => test.component === ['lifecycle', 'registers'][index] && test.exit_code === 0 && test.result?.status === 'PASS' && test.result?.source_commit === report.source_commit && test.result?.android_execution === true);
        report.native_exit_code = native.exit_code;
        if (native.status === 'PASS' && complete && native.exit_code === 0 && report.adb_execution_exit_code === 0 && report.collection === 'collected') report.status = 'ANDROID_BIONIC_EXECUTED_PASS';
        else report.status = report.execution_transport_timed_out || native.status === 'RUNNING' || native.exit_code < 0 ? 'ANDROID_EXECUTION_UNCERTAIN' : 'ANDROID_EXECUTION_FAILED';
      } catch (error) { report.result_validation_error = error.message; report.status = 'ANDROID_EXECUTION_UNCERTAIN'; }
    }
    report.retry_policy = 'No automatic execution retry. Keep this run directory and evidence on every failure.';
    report.exit_code = report.status === 'ANDROID_BIONIC_EXECUTED_PASS' ? 0 : (Number.isInteger(report.native_exit_code) && report.native_exit_code > 0 ? report.native_exit_code : Number.isInteger(report.adb_execution_exit_code) && report.adb_execution_exit_code > 0 ? report.adb_execution_exit_code : 1);
    await persist();
  }
  return report;
}
async function main() {
  const options = {};
  const mapping = { '--bundle': 'bundle', '--adb': 'adb', '--serial': 'serial', '--output': 'outputRoot' };
  for (let index = 2; index < process.argv.length; index += 2) {
    const key = mapping[process.argv[index]]; if (!key || !process.argv[index + 1]) throw new Error('Use --bundle PATH [--adb PATH] [--serial SERIAL] [--output PATH]');
    options[key] = process.argv[index + 1];
  }
  options.bundle ||= path.resolve(__dirname, '..');
  if (process.stdin.isTTY) options.chooseDevice = async devices => {
    devices.forEach((device, index) => console.log(`${index + 1}: ${device.serial} (${device.state})`));
    const prompt = readline.createInterface({ input: process.stdin, output: process.stdout });
    try { return devices[Number(await prompt.question('Choose one device number: ')) - 1]?.serial; } finally { prompt.close(); }
  };
  console.log('Isolated program signal tests only; no Pine/game launch, root, settings change or logcat. The native suite runs once.');
  const report = await runOnce(options);
  console.log(`${report.status}\nResult: ${path.join(report.output_directory, 'result.json')}\nExit code: ${report.exit_code}`);
  process.exitCode = report.exit_code;
}
if (require.main === module) main().catch(error => { console.error(error.message); process.exitCode = 1; });
module.exports = { runOnce, devicesFrom, execute };
