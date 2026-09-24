# Native Steam Controller transport (experimental)

This branch forwards the 2026 Steam Controller's native input reports through
Moonlight's encrypted streaming connection. The paired Windows host creates an
emulated Valve HID device so Windows Steam Input can use the trackpads, rear
buttons, pressure, touch sensors and motion reports. Feature requests and haptic
output travel back through the same connection. The physical USB device remains
on Linux; this is not USB/IP and does not require an SSH data relay.

Requires the matching experimental Vibepollo and libvirtualgamepad branches.
Ordinary Moonlight/Sunshine hosts do not implement this extension. It is disabled
unless explicitly selected; only one native controller is supported per stream
and per host. The Xbox controller path is separate.

On Linux, the Input settings now include **Forward native Steam Controller
(experimental)**. It is off by default. When enabled, Moonlight finds the puck's
interface-2 hidraw node automatically. The helper must still be installed.

For development, set `MOONMACHINE_NATIVE_CONTROLLER_DEVICE` to the puck's
interface-2 hidraw node and `MOONMACHINE_NATIVE_CONTROLLER_HELPER` to the supplied
`app/deploy/linux/native-controller/moonmachine-native-controller` script. Keep
`imu_clock.py` and `controller_io.py` beside it. The user must have access to the hidraw node. The node
number is not stable across boots; check sysfs identity before using it. These
variables override discovery for development and transport simulation.

Vibepollo advertises support only with `MOONMACHINE_NATIVE_CONTROLLER=1`. The
client suppresses the selected controller's SDL Xbox representation to avoid
creating two Windows controllers. If native attachment fails, stop the stream
and resolve the error rather than expecting an automatic SDL fallback.

Input messages use a bounded, versioned report format over reliable encrypted
control traffic. No new network listener is added. The physical helper and
virtual driver both restrict feature/output commands; firmware flashing and
arbitrary USB requests are not provided. Pending feature requests expire, and
stream teardown removes the virtual controller.

The current firmware's raw quaternion remains constant. Windows Steam Input
calculates orientation from gyro/accelerometer data. Duplicate sensor timestamps
are adjusted by the existing bounded timestamp normalizer, preserving button
reports and preventing the observed Steam Input orientation resets.

## Validation

The client and Windows host compile. Protocol bounds/direction tests pass under
ASan/UBSan. The four IMU timestamp tests pass. An initial end-to-end synthetic run
received 4,920 reports in the Windows HID viewer with zero malformed reports.
Normal stream disconnect removed the virtual HID device. Physical testing then received 1,344 real reports in five seconds with zero
malformed reports. Windows Steam Input recognized type 17 (Steam Controller 2026)
and returned changing orientation. Both left/right haptic requests reached the
physical device and were felt by the tester. Normal disconnect removed the HID;
reconnect received 4,721 real reports with zero malformed reports without pairing
or unplugging. Local Steam input isolation and sleep recovery remain unverified.
The initial rumble test did not specify identical pulse effects for the two
sides. A second test sent three identical finite pulse bursts to each side; the
tester confirmed matching timing and count.

`tests/native-controller/simulated-helper.py` exercises input delivery without
hardware. Never select this helper for normal play. It intentionally refuses
feature commands; use the real device helper for capabilities and haptics tests.

The tester also confirmed local Steam menu navigation immediately after ending
the stream, without pairing or reconnecting. Concurrent local input isolation
while streaming, sleep recovery, and the complete guided control checklist
have not been fully validated.

## Command scheduling

Input reading and physical USB commands run independently. Slow USB feature I/O
must not delay button-release or trackpad reports. Commands execute in arrival
order on one worker; no input report is replayed to fill a timing gap.

The physical-command queue holds at most 32 requests. Nonzero rumble/pulse
commands waiting there for over 50 ms are rejected rather than played late.
Stop commands and feature transactions are not treated as expiring haptic effects.
This is a local queue-age limit, not a measurement of network transit time.
Sensor timestamps remain in the input reports; independent computer clocks are
not subtracted or used to reorder button edges.

Steam Input can expose the puck as a second, generic Steam Virtual Gamepad.
Both startup enumeration and hotplug exclude that duplicate using Steam's
per-slot physical VID/PID metadata. Other slots, including Xbox controllers,
remain on the normal gamepad path. The actual MoonDeck/Overcooked launch was
verified with a native HID device and no duplicate Xbox 360 device on Windows.

Run `python3 -m unittest discover -s app/deploy/linux/native-controller
-p test_controller_io.py` for command-order, backpressure, expiry, stop-command,
and failure-recovery tests (join the command onto one line).
