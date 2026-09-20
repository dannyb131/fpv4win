# WiFi Broadcast FPV client for Windows platform.

fpv4win is an app for Windows that packages multiple components together to decode an H264/H265 video feed broadcasted by wfb-ng over the air.

This fork updates the receiver for current OpenIPC FPV firmware and adds:

- H.264/H.265 detection using the OpenIPC RTP payload types.
- Jumbo WFB packet support and correct optional-FCS handling.
- D3D11 hardware video decoding with safe NV12-to-YUV conversion.
- A built-in low-latency HTTP MPEG-TS server for VLC and other players.
- Selectable output logs plus an automatic `fpv4win.log` file.
- Bidirectional MAVLink telemetry for Mission Planner.
- Project-local Windows build dependencies and removal scripts.


- [devourer](https://github.com/openipc/devourer): A userspace rtl8812au driver initially created by [buldo](https://github.com/buldo) and converted to C by [josephnef](https://github.com/josephnef) .
- [wfb-ng](https://github.com/svpcom/wfb-ng): A library that allows broadcasting the video feed over the air.

Supported rtl8812au WiFi adapter only.

It is recommended to use with [OpenIPC](https://github.com/OpenIPC) FPV

![img.png](img/img.png)

### Usage
- 1. Download [Zadig](https://github.com/pbatard/libwdi/releases/download/v1.5.0/zadig-2.8.exe)
- 2. Repair the libusb driver (you may need to enable [Options] -> [List All Devices] to show your adapter).

    ![img.png](img/img1.png)

- 3. Install [vcredist_x64.exe](https://aka.ms/vs/17/release/vc_redist.x64.exe)
- 4. Select your 8812au adapter.
- 5. Select your WFB key.
- 6. Select your drone channel.
- 7. Enjoy!

### Stream to a server

1. Start the receiver and wait for the video to appear.
2. For a built-in, single-viewer HTTP server, use `http://0.0.0.0:8080/stream.ts`.
3. Select **START STREAM**.

Open `http://<computer-LAN-IP>:8080/stream.ts` in VLC on another device. Start the server in fpv4win first, then open the URL in VLC. HTTP mode supports one viewer at a time. Windows Firewall may ask you to allow the app on private networks.

Publishing to an existing RTMP/RTMPS, RTSP/RTSPS, SRT, or MPEG-TS-over-UDP endpoint remains supported. The encoded camera packets are remuxed rather than re-encoded, keeping latency and CPU usage low. MP4 recording and server streaming can be used at the same time.

### Mission Planner telemetry

fpv4win bridges the standard OpenIPC MAVLink WFB streams in both directions:

- Air unit to PC: WFB radio port 16 is decrypted and forwarded to `127.0.0.1:14550`.
- PC to air unit: UDP packets received on port `14551` are encrypted and transmitted on WFB radio port 144 with 1/2 FEC.

The normal `gs.key`, matching the key installed on the air unit, is used for both directions. The bridge uses the default OpenIPC link domain (`link_id` 7669206), matching the video receiver. Mission Planner and fpv4win must currently run on the same PC.

#### 1. Configure the flight controller

Use the ArduPilot serial port physically connected to the camera. For example, when using `SERIAL2`:

```text
SERIAL2_PROTOCOL = 2     # MAVLink2
SERIAL2_BAUD = 115       # 115200 baud
SERIAL2_OPTIONS = 0      # normal bidirectional UART
```

Reboot the flight controller after changing the protocol. The FC TX line connects to camera RX, FC RX connects to camera TX, and both devices need a common ground and 3.3 V UART signalling.

MSP DisplayPort and MAVLink cannot share one UART. DisplayPort provides OSD drawing commands, not the heartbeats, parameters, missions, and command acknowledgements Mission Planner needs. To retain DisplayPort OSD as well as Mission Planner, use separate UARTs.

#### 2. Configure the OpenIPC air unit

SSH into the camera. If DisplayPort previously worked on `/dev/ttyS2`, keep that serial device and change the router to `mavfwd`:

```sh
cp /etc/wfb.yaml /etc/wfb.yaml.bak
wifibroadcast cli -s .telemetry.router mavfwd
wifibroadcast cli -s .telemetry.serial ttyS2
reboot
```

After reconnecting, verify it started:

```sh
wifibroadcast cli -g .telemetry.router
wifibroadcast cli -g .telemetry.serial
pidof mavfwd
ps | grep '[m]avfwd'
```

`ttyS2` is the camera's Linux UART name; it is not automatically the same as ArduPilot's logical `SERIAL2`. Use the camera UART wired to the selected FC telemetry port.

To validate a camera UART directly, stop the background copy and run `mavfwd` in verbose mode without synthetic temperature messages:

```sh
killall mavfwd
mavfwd -v -b 115200 -a 1 -m /dev/ttyS2 \
  -o 127.0.0.1:14551 -i 127.0.0.1:14550
```

A working connection reports MAVLink message `0`, the flight-controller heartbeat. Press `Ctrl+C`, then reboot the camera to restore normal operation.

#### 3. Connect Mission Planner

1. Start fpv4win with the adapter, WFB key, channel, and channel width used by the air unit.
2. Wait for the video and telemetry session to start.
3. In Mission Planner select **UDP**, click **Connect**, and enter port **14550**.
4. Watch **MAVLink (Air->MP / MP->Air)** in fpv4win. The left counter is downlink traffic and the right counter is Mission Planner traffic sent back to the aircraft.

The log should show `MAVLink message 0` followed by `Valid-looking MAVLink heartbeat detected`. If telemetry packets arrive but no heartbeat is found, check the FC serial protocol, baud rate, reboot state, camera UART selection, and crossed RX/TX wiring. OpenIPC `mavfwd` can generate camera-temperature messages even when no FC data is present, so a rising packet counter alone does not prove that the UART is receiving the flight controller.

### Delay test

![img.png](img/delay.png)

### Todo
- OSD
- ~~Hardware acceleration decoding~~
- ~~Record MP4 file~~
- ~~Capture frame to JPG~~
- ~~Stream to RTMP/RTSP/SRT server~~ (WHIP is not yet supported)
- Receive multiple video streams using a single adapter
- ONVIF/GB28181/SIP client

### How to build

Clone with submodules, then run the project-local setup and build scripts from PowerShell:

```powershell
git clone --recurse-submodules https://github.com/dannyb131/fpv4win.git
cd fpv4win
./scripts/setup-dependencies.ps1
./scripts/build-windows.ps1
```

The executable is written to `build/Release/fpv4win.exe`.

Remove the downloaded compiler libraries and other project-local dependencies later with:

```powershell
./scripts/remove-dependencies.ps1
```

The dependencies live only in `.deps`, so they do not modify system-wide library installations.

- Take a look at
[GithubAction](https://github.com/openipc/fpv4win/blob/main/.github/workflows/msbuild.yml)
