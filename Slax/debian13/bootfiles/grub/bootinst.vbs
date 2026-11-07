' Install bundled GRUB and USB EFI files using Windows XP-era scripting APIs.
Option Explicit
Dim files, shell, wmi
On Error Resume Next
Install
If Err.Number <> 0 Then
    WScript.Echo Err.Description
    WScript.Quit 1
End If
WScript.Quit 0

' Quote paths passed directly to Windows programs, without a command shell.
Function Quote(value)
    Quote=Chr(34) & value & Chr(34)
End Function

' Refuse unsafe targets or report an installation failure.
Sub Fail(message)
    Err.Raise vbObjectError+1,"bootinst",message
End Sub

' Resolve a drive letter through WMI's actual volume-to-partition association.
Function PartitionForDrive(drive)
    Dim partitions, partition
    Set partitions=wmi.ExecQuery("ASSOCIATORS OF {Win32_LogicalDisk.DeviceID='" & drive & "'} WHERE AssocClass=Win32_LogicalDiskToPartition")
    If partitions.Count <> 1 Then Fail "Cannot resolve one physical partition for " & drive & "."
    For Each partition In partitions
        Set PartitionForDrive=partition
    Next
End Function

' Install BIOS boot code and copy EFI files without changing partition types.
Sub Install
    Dim boot, drive, filesystem, automatic, elevated, arguments
    Dim partition, hostPartition, disk, model, systemDisk, os, version, modern, registry, allowed
    Dim storage, disks, biosStatus
    Set files=CreateObject("Scripting.FileSystemObject")
    Set shell=CreateObject("WScript.Shell")
    Set wmi=GetObject("winmgmts:{impersonationLevel=impersonate}!\\.\root\cimv2")
    boot=files.GetParentFolderName(files.GetParentFolderName(WScript.ScriptFullName))
    drive=UCase(files.GetDriveName(boot))
    If Len(drive) <> 2 Then Fail "Run this installer from a local USB drive."
    filesystem=files.GetDrive(drive).FileSystem
    If filesystem <> "FAT" And filesystem <> "FAT32" Then Fail "UEFI boot requires FAT12, FAT16 or FAT32. No changes were made."
    automatic=False
    elevated=False
    If WScript.Arguments.Count > 0 Then automatic=(LCase(WScript.Arguments(0))="auto")
    If WScript.Arguments.Count > 1 Then elevated=(WScript.Arguments(1)="elevated")

    Set partition=PartitionForDrive(drive)
    Set hostPartition=PartitionForDrive(files.GetDriveName(files.GetSpecialFolder(0)))
    If partition.DiskIndex=hostPartition.DiskIndex Then Fail "Refusing to install on the disk containing Windows."
    For Each disk In wmi.ExecQuery("SELECT Model FROM Win32_DiskDrive WHERE Index=" & partition.DiskIndex)
        model=disk.Model
    Next
    For Each os In wmi.ExecQuery("SELECT Version FROM Win32_OperatingSystem")
        version=Split(os.Version,".")
    Next
    modern=(CInt(version(0)) > 6 Or (CInt(version(0))=6 And CInt(version(1)) >= 2))

    ' Check access without writing a probe directory into the Windows installation.
    Set registry=GetObject("winmgmts:{impersonationLevel=impersonate}!\\.\root\default:StdRegProv")
    registry.CheckAccess &H80000002,"SYSTEM\CurrentControlSet\Control",2,allowed
    If Not allowed Then
        If elevated Then Fail "Administrator rights are required. Run bootinst.bat as an administrator."
        WScript.Echo "Requesting administrator rights..."
        If automatic Then arguments="auto elevated" Else arguments="interactive elevated"
        CreateObject("Shell.Application").ShellExecute boot & "\bootinst.bat",arguments,boot,"runas",1
        WScript.Quit 740
    End If

    If modern Then
        Set storage=GetObject("winmgmts:{impersonationLevel=impersonate}!\\.\root\Microsoft\Windows\Storage")
        Set disks=storage.ExecQuery("SELECT IsBoot,IsSystem FROM MSFT_Disk WHERE Number=" & partition.DiskIndex)
        If disks.Count <> 1 Then Fail "Cannot verify the target disk's system status."
        For Each systemDisk In disks
            If systemDisk.IsBoot Or systemDisk.IsSystem Then Fail "Refusing to install on a Windows system or boot disk."
        Next
    End If

    WScript.Echo "Install GRUB 2 on " & drive & " (disk " & partition.DiskIndex & ": " & model & ", " & filesystem & ")."
    If Not automatic Then
        WScript.StdOut.Write "Press Enter to install, or close this window to cancel: "
        WScript.StdIn.ReadLine
    End If
    biosStatus=shell.Run(Quote(boot & "\grub\windows\grub-install.exe") & " --target=i386-pc --directory=" & Quote(boot & "\grub\windows\i386-pc") & _
        " --boot-directory=" & Quote(boot) & " --recheck --locales= --fonts= --themes= " & Quote("\\?\PhysicalDrive" & partition.DiskIndex),1,True)
    If Not files.FolderExists(drive & "\EFI") Then files.CreateFolder drive & "\EFI"
    If Not files.FolderExists(drive & "\EFI\BOOT") Then files.CreateFolder drive & "\EFI\BOOT"
    files.CopyFolder boot & "\EFI\*",drive & "\EFI\",True
    WScript.Echo "UEFI boot files installed in " & drive & "\EFI\BOOT."
    If biosStatus <> 0 Then Fail "BIOS GRUB installation failed (exit " & biosStatus & ")."
    WScript.Echo "BIOS boot installation finished."
End Sub
