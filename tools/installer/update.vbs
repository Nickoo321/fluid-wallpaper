' Fluid Wallpaper self-update: runs update.ps1 (next to this file) with no console window.
' Arguments are handed on (the sign-in entry passes -DelaySec 90).
Dim fso, here, sh, extra, i
Set fso = CreateObject("Scripting.FileSystemObject")
here = fso.GetParentFolderName(WScript.ScriptFullName)
extra = ""
For i = 0 To WScript.Arguments.Count - 1
    extra = extra & " " & WScript.Arguments(i)
Next
Set sh = CreateObject("WScript.Shell")
sh.Run "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File """ & here & "\update.ps1""" & extra, 0, False
