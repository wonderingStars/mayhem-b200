Using a USB SDR with mayhem-b200 (RTL-SDR, HackRF, Airspy, ...)
==============================================================

mayhem-b200 can drive non-USRP radios through SoapySDR (bundled here). Everything
software-side is set up for you. The ONE manual step Windows requires is assigning
a USB driver to the radio, because Windows will not let any application open a raw
USB SDR until it has the WinUSB driver. This is a one-time thing per radio.

Do this once, per radio
-----------------------
 1. Plug the SDR into a USB port.
 2. Run  zadig-2.5.exe  (in this folder). Click Yes at the UAC prompt.
 3. Menu:  Options -> List All Devices  (tick it).
 4. In the dropdown, pick your radio. Typical names:
       RTL-SDR .......... "Bulk-In, Interface (Interface 0)" or "RTL2838UHIDIR"
       HackRF ........... "HackRF One"
       Airspy ........... "Airspy" / "AIRSPY"
       (Pluto/Lime/BladeRF usually ship their own driver and need no Zadig step.)
    If unsure, unplug/replug and watch which entry appears — that is your radio.
 5. To the right of the green arrow, choose  WinUSB  as the target driver.
 6. Click  "Install Driver"  (or "Replace Driver"). Wait for success.

That's it. Unplug and replug the radio once, then start
"Mayhem B200 (other SDRs)" from the Start Menu.

Notes
-----
* For an RTL-SDR this replaces the Windows DVB-T/TV driver with WinUSB. That is
  expected and only affects using it as an SDR.
* To go back to using the dongle as a TV tuner, uninstall the WinUSB driver in
  Device Manager and let Windows reinstall the original.
* The bundled SoapySDR device modules cover: RTL-SDR, HackRF, Airspy, Airspy HF+,
  BladeRF, LimeSDR (LMS7), PlutoSDR, RedPitaya, SDRplay, and network/remote SDRs.
* The app talks to a local sdrlink server on 127.0.0.1:5960 — the launcher starts
  it for you. To point at a server on another machine, run mayhem-b200.exe with
  --driver=sdrlink --args=<host>:5960 instead.
