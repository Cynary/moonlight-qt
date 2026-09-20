# VAAPI single-map validation

Tested on September 20, 2026 with an Intel Arc 130V (GMKtec K17), Bazzite,
Gamescope, and an LG VRR TV. The test stream was HEVC 4:2:0, 4K HDR, 60 FPS
on a 120 Hz output. This does not validate the separate 4:4:4 import problem.

The candidate is based on v6.1.0-vrr14 to isolate the mapping change from newer
pacing changes. Installed version: 6.1.0-vrr14-vaapi1. Executable SHA-256:
`74827f7759c478ed8aefa4069f42c6b61636174a33ac439337190998e12eed1b`.

The Flatpak build completed. All four required VRR test programs and
`vrrreplay --help` returned zero when checked individually. The outer
flatpak-builder `--run` process returned one even when its commands completed;
individual exits were checked again explicitly. These existing tests cover the
shared pacing logic, not VAAPI mapping on hardware.

During 6,915 presented gameplay frames after excluding startup:

- No pacer drops.
- Every preparation-time decode synchronization duration was zero.
- Rendering p99 was 1.450 ms, maximum 2.165 ms. Previously observed 10–11 ms
  second-sync/import stalls were absent.
- Total preparation p99 was 1.704 ms, maximum 2.732 ms.
- Gamescope samples showed 60–61 paints/second and zero repeated base frames.

The user still observed mostly 50–70 Hz variation and rare excursions to about
118 Hz. This change does not eliminate all timing variation. In the first
2,403 steady frames, submission intervals followed host RTP intervals with a
median absolute difference of 20 microseconds and p99 of 71 microseconds.
Frequent source intervals were 14.59, 16.67, and 18.74 ms. Two later catch-up
intervals of 8.564 and 8.449 ms followed host timestamp gaps of 31.255 and
29.167 ms plus initial decode waits of about 20.9 ms. Those initial waits are
still present; this patch avoids repeated synchronization during preparation.

The host reported a 116 FPS RTSS limit, a 60 FPS stream, and a 480 Hz WGC
capture admission rate. No host settings were changed for this comparison.
Hardware before/after captures were separate live sessions, not a deterministic
replay of identical gameplay. Suspend/resume, export-failure fallback, other
VAAPI drivers, and other codecs still need live validation.

Implementation and testing were performed with OpenAI Codex; the user assessed
the TV's behavior. Source inspection and these measurements do not replace
maintainer review or testing on other systems.
