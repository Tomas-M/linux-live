from pathlib import Path
import ctypes
import hashlib
import time
import paramiko

folder=Path(__file__).resolve().parent
remote='/var/tmp/slax-build-trixie'
output=folder/'slax-build-trixie'
previous=''
# Keep the host awake while the VM compiles and transfers the final files.
ctypes.windll.kernel32.SetThreadExecutionState(0x80000001)

# Reconnect through the kernel reboot and wait for the one-shot build result.
while True:
    client=paramiko.SSHClient()
    client.load_host_keys(str(folder/'known_hosts'))
    try:
        client.connect('10.0.0.48',port=22,username='root',key_filename=str(folder/'id_ed25519'),timeout=30,banner_timeout=60,auth_timeout=60,allow_agent=False,look_for_keys=False)
        _,stdout,_=client.exec_command('systemctl show slax-autobuild.service -p ActiveState -p SubState -p Result; test ! -s '+remote+'/SHA256SUMS || echo BUILD_COMPLETE',timeout=30)
        status=stdout.read().decode()
        if status!=previous:
            print(time.strftime('%Y-%m-%d %H:%M:%S'),status.strip(),flush=True)
            previous=status
        if 'BUILD_COMPLETE' in status:
            break
        if 'ActiveState=failed' in status:
            _,stdout,_=client.exec_command('tail -n 60 /root/slax-autobuild.log',timeout=30)
            raise RuntimeError(stdout.read().decode())
    except (OSError,paramiko.SSHException,EOFError) as error:
        print('Waiting for the work VM:',error,flush=True)
    finally:
        if 'BUILD_COMPLETE' not in locals().get('status',''):
            client.close()
    time.sleep(20)

# Download only the ISO and publish it after verifying its remote checksum.
output.mkdir(exist_ok=True)
part=output/'slax-amd64.iso.part'
with client.open_sftp() as transfer:
    with transfer.open(remote+'/SHA256SUMS') as file:
        expected=file.read().decode().split()[0]
    transfer.get(remote+'/slax-amd64.iso',str(part))
client.close()
with part.open('rb') as file:
    actual=hashlib.file_digest(file,'sha256').hexdigest()
if actual!=expected:
    raise RuntimeError('Downloaded ISO checksum mismatch.')
part.replace(output/'slax-amd64.iso')
print('Slax build downloaded and verified:',output,flush=True)
