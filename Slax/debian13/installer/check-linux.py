from pathlib import Path
import json
import os
import subprocess
import sys
import gi

gi.require_version('UMockdev','1.0')
from gi.repository import UMockdev

binary=str(Path(sys.argv[1]).resolve())
bed=UMockdev.Testbed.new()
usb=bed.add_device('usb','installer-usb',None,[],['DEVTYPE','usb_device'])

# Build isolated kernel/udev fixtures without attaching or writing any physical disk.
def disk(name,parent=usb,readonly=False,kind='disk',cdrom=False,number='240:0'):
    parent_path=parent.removeprefix('/sys') if parent else '/devices'
    path=parent_path+'/'+name
    major,minor=number.split(':')
    data=f'P: {path}\nN: {name}\nE: SUBSYSTEM=block\nE: DEVTYPE={kind}\nE: DEVNAME=/dev/{name}\nE: MAJOR={major}\nE: MINOR={minor}\nE: ID_MODEL=Fixture_東京_İstanbul\nE: ID_SERIAL={name}\nE: ID_CDROM={int(cdrom)}\nE: USEC_INITIALIZED=123456\nA: size=16777216\nA: ro={int(readonly)}\nA: removable=1\nA: dev={number}\n'
    if kind=='partition': data+='A: partition=1\nA: start=2048\n'
    bed.add_from_string(data)
    link=Path(bed.get_root_dir())/'sys/dev/block'/number
    link.parent.mkdir(parents=True,exist_ok=True)
    link.symlink_to('../../'+path.removeprefix('/'))
    return '/sys'+path


# Read discovery through the same command and native implementation used by the GUI.
def probe(source=None):
    return json.loads(subprocess.check_output([binary,'--list-devices']+([source] if source else [])))


normal=disk('fixture-usb')
readonly=disk('fixture-readonly',readonly=True,number='240:1')
internal=disk('fixture-internal',parent=None,number='240:2')
partition=disk('fixture-partition',parent=normal,kind='partition',number='240:3')
optical=disk('fixture-optical',cdrom=True,number='240:4')
rows=probe()
assert {row['path'] for row in rows}=={'/dev/fixture-readonly','/dev/fixture-internal','/dev/fixture-partition'}
assert [row['path'] for row in rows]==sorted(row['path'] for row in rows)
assert next(row for row in rows if row['path']=='/dev/fixture-readonly')['blocked']=='This disk is read-only.'
assert next(row for row in rows if row['path']=='/dev/fixture-partition')['name'].startswith('Fixture 東京 İstanbul')

# Protect a synthetic system mount, including its block parent.
mounts=Path(bed.get_root_dir())/'proc/self/mountinfo'
mounts.parent.mkdir(parents=True)
mounts.write_text('1 0 240:3 / / rw - ext4 /dev/fixture-partition rw\n')
assert 'running system' in next(row for row in probe() if row['path']=='/dev/fixture-partition')['eraseBlocked']
mounts.write_text('')

# Match an ordinary source file's filesystem to a mocked USB block identity.
source=sys.argv[2]
dev=os.stat(source).st_dev
origin=disk('fixture-source',number=f'{os.major(dev)}:{os.minor(dev)}')
row=next(row for row in probe(source) if row['path']=='/dev/fixture-source')
assert 'ISO image' in row['eraseBlocked']
bed.remove_device(origin)

# Removal and replacement never reuse the old selection identity.
old=next(row['id'] for row in probe() if row['path']=='/dev/fixture-partition')
bed.remove_device(partition)
bed.remove_device(normal)
assert all(row['path']!='/dev/fixture-usb' for row in probe())
# UMockdev leaves its old block-class link behind when a recorded device is removed.
(Path(bed.get_root_dir())/'sys/block/fixture-usb').unlink(missing_ok=True)
(Path(bed.get_root_dir())/'sys/block/fixture-partition').unlink(missing_ok=True)
normal=disk('fixture-usb',number='240:5')
bed.set_property(normal,'ID_SERIAL','replacement')
partition=disk('fixture-partition',parent=normal,kind='partition',number='240:6')
assert next(row['id'] for row in probe() if row['path']=='/dev/fixture-partition')!=old
print('Linux disk and partition discovery checks passed.')
