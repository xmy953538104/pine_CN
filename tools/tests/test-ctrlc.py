import os,pty,fcntl,termios,subprocess,time,pathlib,select,tempfile,json
# POSIX PTY integration test; Windows cancellation is covered by pine-diag.Tests.ps1.
base=pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='pine-ctrlc-') as temp:
    root=pathlib.Path(temp)
    fake=root/'adb'
    fake.write_bytes((base/'tools/tests/fake-adb.py').read_bytes()); fake.chmod(0o755)
    master,slave=pty.openpty()
    def setup():
        os.setsid()
        fcntl.ioctl(slave,termios.TIOCSCTTY,0)
    env=dict(os.environ, XDG_CACHE_HOME=str(root/'cache'),XDG_CONFIG_HOME=str(root/'config'),XDG_DATA_HOME=str(root/'data'),PINE_FAKE_ADB_ARGS=str(root/'args'),PINE_FAKE_ADB_PID=str(root/'pid'),PINE_FAKE_ADB_MODE='normal',TERM='xterm')
    proc=subprocess.Popen([os.environ.get('PINE_PWSH', 'pwsh'),'-NoLogo','-NoProfile','-File',str(base/'tools/pine-diag.ps1'),'-AdbPath',str(fake),'-OutputDirectory',str(root/'out'),'-NonInteractive'],stdin=slave,stdout=slave,stderr=slave,preexec_fn=setup,env=env)
    os.close(slave)
    deadline=time.monotonic()+15
    output=b''
    while time.monotonic()<deadline:
        if select.select([master],[],[],.1)[0]: output+=os.read(master,8192)
        if b'message' in output: break
    assert b'message' in output,output
    child=int((root/'pid').read_text())
    os.write(master,b'\x03')
    try: proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill(); raise
    assert not pathlib.Path('/proc/'+str(child)).exists(),'fake ADB leaked after Ctrl+C'
    files=list((root/'out').glob('*.jsonl'))
    assert files and any(json.loads(l)['message']=='中文 日志' for f in files for l in f.read_text().splitlines())
    os.close(master)
    print('PASS: real PTY Ctrl+C interrupted pwsh, flushed UTF8 logs and reaped fake ADB; exit',proc.returncode)
