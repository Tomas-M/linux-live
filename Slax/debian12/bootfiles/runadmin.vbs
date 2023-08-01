Set UAC = CreateObject("Shell.Application")
Set args = WScript.Arguments
UAC.ShellExecute args.Item(0), "", "", "runas", 1
