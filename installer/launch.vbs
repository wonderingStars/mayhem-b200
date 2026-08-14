' mayhem-b200 launcher (installed build) — windowless.
'
' Starts the radio app and the web portal, waits for the portal to answer (a
' B200 takes ~15-20 s to initialise), then opens the browser on the portal.
' Run via wscript.exe (the default for .vbs), so there is NO console window.
'
' The app's own GUI window stays visible (that is the radio); only the portal
' server runs hidden. If everything is already running it just opens the browser.
'
' SPDX-License-Identifier: GPL-2.0-or-later
Option Explicit

Dim sh, fso, here
Set sh  = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
here = fso.GetParentFolderName(WScript.ScriptFullName)
sh.CurrentDirectory = here

Dim appExe, portalExe
appExe    = here & "\mayhem-b200.exe"
portalExe = here & "\mayhem-portal.exe"

' 1 = normal visible window (the app's radio GUI); 0 = hidden (portal server).
If Not IsRunning("mayhem-b200.exe") Then
    sh.Run """" & appExe & """ --driver=uhd --portal=8090", 1, False
End If
If Not IsRunning("mayhem-portal.exe") Then
    sh.Run """" & portalExe & """ -http 127.0.0.1:8081 -backend http://127.0.0.1:8090", 0, False
End If

' Wait for the portal to answer before opening the browser (avoids a dead tab
' while the B200 initialises). Give up after ~60 s and open it anyway.
Dim i
For i = 1 To 60
    If PortalUp() Then Exit For
    WScript.Sleep 1000
Next

CreateObject("Shell.Application").ShellExecute "http://127.0.0.1:8081"

Function PortalUp()
    On Error Resume Next
    Dim http
    Set http = CreateObject("WinHttp.WinHttpRequest.5.1")
    http.SetTimeouts 800, 800, 800, 800
    http.Open "GET", "http://127.0.0.1:8081/api/status", False
    http.Send
    PortalUp = (http.Status = 200)
    If Err.Number <> 0 Then PortalUp = False
    On Error GoTo 0
End Function

Function IsRunning(procName)
    On Error Resume Next
    Dim wmi, procs
    Set wmi = GetObject("winmgmts:\\.\root\cimv2")
    Set procs = wmi.ExecQuery("SELECT ProcessId FROM Win32_Process WHERE Name='" & procName & "'")
    IsRunning = (procs.Count > 0)
    If Err.Number <> 0 Then IsRunning = False
    On Error GoTo 0
End Function
