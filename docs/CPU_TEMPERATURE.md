# Server CPU temperature

The CPU chip shows the load and CPU temperature of the computer running FlexWeb.
It is separate from the Flex radio and amplifier temperatures. Tap the chip for
sensor/source details, CPU usage and process memory. FUNC > SERVER CPU hides it.
Temperature is sampled approximately every two seconds and sent with load updates.
A missing, invalid or stale reading is shown as `TEMP —`, never as zero degrees.

## Windows (including the Skull Canyon NUC)

1. Obtain LibreHardwareMonitor from its official project:
   https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases
2. Run it on the FlexWeb server computer. Allow it to read the CPU sensors using
   whatever permissions your installation requires. FlexWeb does not request
   elevation or install a hardware-access driver.
3. Confirm that LibreHardwareMonitor shows a live CPU Package temperature.
   For versions that provide WMI, it must expose `root\LibreHardwareMonitor:Sensor`.
   Alternatively, enable the monitor’s local JSON/web-server endpoint on port
   8085 and verify `http://127.0.0.1:8085/data.json` on the same PC. Restrict the
   monitor’s listener/firewall to localhost; do not expose it to the Internet.
4. Start FlexWeb. The CPU chip should show the package temperature and identify
   the selected sensor when tapped. If no package/die sensor is exposed, the
   hottest valid CPU core sensor is used and named in the details.

OpenHardwareMonitor's `root\OpenHardwareMonitor` provider is a fallback. GPU,
motherboard and generic ACPI thermal zones are not substituted for CPU temperature.
Versions without working WMI can use the local JSON endpoint. Seeing a value
in the monitor’s own UI does not by itself prove that either interface is available.

FlexWeb uses one read-only, noninteractive Windows PowerShell helper. It queries
local sensor data only. WMI is tried first; an already-enabled local JSON server
is a fallback. FlexWeb never enables or changes a network listener. If the helper stops responding, readings expire after 15 s;
it is restarted with backoff. Closing the hardware monitor removes the temperature
readout without interrupting radio/audio operation.

## Linux

Uses `coretemp`, `k10temp` or `zenpower` CPU sensors from `/sys/class/hwmon`.
Package/die readings are preferred over individual cores; faulted readings are
excluded. No privileged helper is launched.

## macOS

No supported temperature provider is included yet. CPU load and process memory
remain available; temperature is explicitly marked unavailable.

## Station check

On the Intel Skull Canyon NUC, compare the package reading with the same named
sensor in LibreHardwareMonitor, then close the monitor and confirm `TEMP —`.
Check that receive audio, tuning and TX controls continue working during both
conditions. Hardware validation remains necessary on Windows.
