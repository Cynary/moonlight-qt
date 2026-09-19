# DualSense Bluetooth waveform validation

Linux with SDL 2.24 or newer:

```sh
mkdir -p build/tests-haptics
cd build/tests-haptics
qmake6 ../../tests/haptics/haptics.pro
make -j10
./tst_dualsensehaptics
```

Use the same `PKG_CONFIG_PATH` and runtime `LD_LIBRARY_PATH` as the application
if it links a local SDL build. The test uses a local datagram socket in place of
hidraw and does not open a physical controller. It exercises the production
worker and resampler, checks both channels and the Bluetooth CRC, bounded queue
behavior, idle silence and controller removal.

For real playback, use the coordinated Vibeshine and Moonlight builds. Pair the
DualSense/Edge to the Linux **client**, select DS5 (or automatic PlayStation
emulation) on the host, then reconnect the stream. The client log must contain
`DualSense Bluetooth waveform backend ready`. Ordinary USB connections and
unsupported clients keep conventional rumble. The game must send native haptic
PCM to the host virtual controller's audio endpoint.

Check distinct left/right native effects while moving both sticks and using
adaptive triggers; stop the effect, pause/end the stream, disconnect/reconnect
Bluetooth and repeat. Verify an older client still receives rumble. A working
backend log or passing socket test alone is not physical haptics validation.

`moonlight --haptics-license` prints the embedded source and notices; see
[`PROVENANCE.md`](../../third-party/saxense/PROVENANCE.md) for the pinned source,
licenses and distribution requirements.
