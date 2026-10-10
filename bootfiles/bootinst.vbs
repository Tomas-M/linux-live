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

' Keep BIOS installation before EFI partition changes, including elevation restarts.
Sub Install
    Dim boot, drive, filesystem, automatic, elevated, arguments
    Dim partition, hostPartition, disk, model, systemDisk, os, version, modern, gpt, registry, allowed
    Dim storage, disks, items, item, parameters, response, biosStatus
    Set files=CreateObject("Scripting.FileSystemObject")
    Set shell=CreateObject("WScript.Shell")
    Set wmi=GetObject("winmgmts:{impersonationLevel=impersonate}!\\.\root\cimv2")
    boot=files.GetParentFolderName(WScript.ScriptFullName)
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
    gpt=(Left(partition.Type,4)="GPT:")
    If gpt And Not modern Then
        If CInt(version(0)) < 6 Or CInt(version(1)) < 1 Then Fail "Installing to GPT needs Windows 7 or later. Use an MBR USB drive on XP or Vista."
    End If

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
    files.CopyFile boot & "\EFI\BOOT\*.EFI",drive & "\EFI\BOOT\",True
    ' MBR retains its FAT type for XP; GPT needs the EFI System Partition GUID.
    If gpt Then
        If modern Then
            Set items=storage.ExecQuery("SELECT * FROM MSFT_Partition WHERE DiskNumber=" & partition.DiskIndex & " AND Offset=" & partition.StartingOffset)
            If items.Count <> 1 Then Fail "Cannot identify the FAT partition for EFI setup."
            For Each item In items
                Set parameters=item.Methods_("SetAttributes").InParameters.SpawnInstance_
                parameters.GptType="{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}"
                Set response=item.ExecMethod_("SetAttributes",parameters)
                If response.ReturnValue <> 0 Then Fail "Cannot mark the EFI partition (error " & response.ReturnValue & ")."
                If Not files.FolderExists(drive & "\") Then
                    Set parameters=item.Methods_("AddAccessPath").InParameters.SpawnInstance_
                    parameters.AccessPath=drive & "\"
                    Set response=item.ExecMethod_("AddAccessPath",parameters)
                    If response.ReturnValue <> 0 Then Fail "Cannot restore the USB drive letter (error " & response.ReturnValue & ")."
                End If
            Next
        Else
            MarkLegacyGpt drive
        End If
    End If
    WScript.Echo "UEFI boot files installed in " & drive & "\EFI\BOOT."
    If biosStatus <> 0 Then Fail "BIOS GRUB installation failed (exit " & biosStatus & ")."
    WScript.Echo "BIOS boot installation finished."
End Sub

' Windows 7 has DiskPart but lacks the newer Storage WMI provider.
Sub MarkLegacyGpt(drive)
    Dim path, commands, status, partition
    path=files.BuildPath(files.GetSpecialFolder(2),files.GetTempName)
    Set commands=files.CreateTextFile(path,False)
    commands.WriteLine "select volume " & Left(drive,1)
    commands.WriteLine "set id=C12A7328-F81F-11D2-BA4B-00A0C93EC93B override"
    commands.WriteLine "assign letter=" & Left(drive,1)
    commands.WriteLine "exit"
    commands.Close
    status=shell.Run("diskpart.exe /s " & Quote(path),1,True)
    files.DeleteFile path
    If status <> 0 Then Fail "Cannot mark the EFI partition (DiskPart exit " & status & ")."
    If Not files.FolderExists(drive & "\") Then Fail "Cannot restore the USB drive letter."
    Set partition=PartitionForDrive(drive)
    If partition.Type <> "GPT: System" Then Fail "DiskPart did not mark the USB partition as an EFI System Partition."
End Sub
