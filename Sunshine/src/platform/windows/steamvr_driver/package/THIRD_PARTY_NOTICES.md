# VipleStream SteamVR driver — third-party notices

## OpenVR SDK 2.15.6 (BSD-3-Clause)

`driver_viplestream.dll` is compiled against the OpenVR driver header
(`openvr_driver.h`, OpenVR SDK v2.15.6, commit
`0924064316de3effbcd1acf1e309182a2deb1c05`). The header's inline helpers are
part of the DLL. License text: `LICENSE-OpenVR.txt`.

No Valve sample code (d3drender, driverlog, threadtools) is included.

## ALVR

This version of the driver contains no code derived from ALVR: the direct-mode
component, the frame compositor, its shaders and the HMD device were written
independently. If a later version ports ALVR code, that version ships
`LICENSE-ALVR.txt` and lists the ported files here.
