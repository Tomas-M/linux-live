Set-Location $PSScriptRoot
# Keep the guest display visible and restrict SSH forwarding to this computer.
& "$PSScriptRoot/runtime/qemu-system-x86_64.exe" -name 'Debian work Linux' -machine q35 -accel tcg,thread=multi -cpu qemu64 -smp 4 -m 4096 -drive 'file=debian.qcow2,format=qcow2,if=virtio' -nic 'user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:2222-:22' -display gtk -vga virtio -boot c
