# Start the Debian work VM with its visible VirtualBox console.
& 'C:/Program Files/Oracle/VirtualBox/VBoxManage.exe' startvm 'Slax Debian work' --type gui
if ($LASTEXITCODE) { exit $LASTEXITCODE }
# Collect the finished ISO after the automatic guest build and kernel reboot.
Start-Process -FilePath (Get-Command python).Source -ArgumentList '-u',"`"$PSScriptRoot/collect-slax-build.py`"" `
   -WorkingDirectory $PSScriptRoot -WindowStyle Hidden `
   -RedirectStandardOutput "$PSScriptRoot/slax-build-collector.log" `
   -RedirectStandardError "$PSScriptRoot/slax-build-collector.errors.log"
