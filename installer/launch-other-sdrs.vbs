' mayhem-b200 launcher for NON-USRP radios (via SoapySDR / sdrlink) — windowless.
'
' Starts the sdrlink server (with the bundled SoapySDR worker + device modules),
' points the app at it over the network with --driver=sdrlink, brings up the web
' portal, and opens the browser once it answers.
'
' Before a USB SDR will work you must assign it the WinUSB driver once with
' Zadig — see ZADIG-README.txt in this folder.
'
' SPDX-License-Identifier: GPL-2.0-or-later
Option Explicit

Dim sh, fso, soapyDir, appDir
Set sh  = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
soapyDir = fso.GetParentFolderName(WScript.ScriptFullName)   ' <app>\soapy
appDir   = fso.GetParentFolderName(soapyDir)                 ' <app>

' 1) The sdrlink server + SoapySDR worker (hidden). The .cmd sets the SoapySDR
'    environment before launching the server.
If Not IsRunning("sdrlink-server.exe") Then
    sh.Run """" & soapyDir & "\start-sdrlink-server.cmd""", 0, False
End If

' 2) The app, pointed at the local server. Its sdrlink client auto-reconnects,
'    so it is fine if the server is still starting.
If Not IsRunning("mayhem-b200.exe") Then
    sh.Run """" & appDir & "\mayhem-b200.exe"" --driver=sdrlink --args=127.0.0.1:5960 --portal=8090", 1, False
End If

' 3) The web portal.
If Not IsRunning("mayhem-portal.exe") Then
    sh.Run """" & appDir & "\mayhem-portal.exe"" -http 127.0.0.1:8081 -backend http://127.0.0.1:8090", 0, False
End If

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
