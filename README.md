# ESP32 Tailscale Wake-on-LAN Bridge

A guide to installing, configuring, building, flashing, and operating
the ESP32 Tailscale Wake-on-LAN Bridge on **Windows or Linux**.

## 1. Required Hardware

-   ESP32-WROOM-32 development board.
-   USB data cable.
-   Windows 10/11 or a supported Linux distribution.
-   Wi-Fi router.
-   Target PC connected to the router through Ethernet.
-   iPhone or another remote device with Tailscale installed.

The ESP32 must remain powered on to receive remote wake commands.

## 2. Download Required Software and Drivers

  ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
  Software                Windows                                                                           Linux
  ----------------------- --------------------------------------------------------------------------------- --------------------------------------------------------------------------------------------------------
  ESP-IDF 5.5.3           [Espressif download](https://dl.espressif.com/dl/esp-idf/)                        [ESP-IDF Linux
                                                                                                            setup](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32/get-started/linux-macos-setup.html)

  Git                     [Git for Windows](https://git-scm.com/download/win)                               Install through your package manager

  CP210x USB driver       [Silicon                                                                          Usually supported by the Linux kernel
                          Labs](https://www.silabs.com/software-and-tools/usb-to-uart-bridge-vcp-drivers)   

  Tailscale               [Windows download](https://tailscale.com/download/windows)                        [Linux installation](https://tailscale.com/download/linux)

  Tailscale for iPhone    [Tailscale downloads](https://tailscale.com/download)                             Not applicable
  ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

### Python

ESP-IDF requires Python and several Python packages.

-   **Windows:** The ESP-IDF installer provides a managed Python
    environment.
-   **Linux:** The ESP-IDF installation script creates a Python virtual
    environment.

Use the Python environment installed by ESP-IDF when running ESP-IDF
commands. Avoid mixing ESP-IDF dependencies with unrelated Python
installations.

## 3. Install the ESP32 USB Driver

### Windows

For an ESP32 board using CP2102:

1.  Download the CP210x Windows VCP driver.
2.  Extract the ZIP file if necessary.
3.  Run the Windows 64-bit installer.
4.  Complete the installation.
5.  Connect the ESP32 using a USB data cable.
6.  Open Device Manager.
7.  Expand **Ports (COM & LPT)**.
8.  Find **Silicon Labs CP210x USB to UART Bridge**.
9.  Record the assigned COM port, for example `COM3`.

If the board uses CH340 instead of CP2102, install the corresponding WCH
driver.

### Linux

Most modern Linux distributions include drivers for CP210x and CH340
USB-to-serial adapters.

Connect the ESP32 and check the serial devices:

``` bash
ls /dev/ttyUSB*
ls /dev/ttyACM*
```

The ESP32 may appear as `/dev/ttyUSB0` or `/dev/ttyACM0`.

Check the USB device:

``` bash
lsusb
```

If the serial device exists but permission is denied, add your user to
the `dialout` group:

``` bash
sudo usermod -aG dialout "$USER"
```

Log out and log back in, or restart the computer. Check group membership
with:

``` bash
groups
```

Do not use `sudo idf.py` as a routine workaround for serial permissions.

## 4. Install ESP-IDF 5.5.3

Use ESP-IDF v5.5.3 to match the tested project environment.

### Windows installation

1.  Download the Windows ESP-IDF installer from Espressif.
2.  Choose ESP-IDF version 5.5.3.
3.  Select the ESP32 target and required toolchain.
4.  Install the managed Python environment.
5.  Install Git, CMake, Ninja, and the required compiler tools.
6.  Allow the installer to complete.

An example installation directory is:

``` text
C:\esp
```

Open the generated ESP-IDF PowerShell environment shortcut and verify:

``` powershell
idf.py --version
```

Expected version:

``` text
ESP-IDF v5.5.3
```

### Linux installation

The following example uses Ubuntu or another Debian-based distribution.

Install dependencies:

``` bash
sudo apt update

sudo apt install -y \
  git \
  wget \
  flex \
  bison \
  gperf \
  python3 \
  python3-pip \
  python3-venv \
  cmake \
  ninja-build \
  ccache \
  libffi-dev \
  libssl-dev \
  dfu-util \
  libusb-1.0-0
```

Create the ESP-IDF directory:

``` bash
mkdir -p ~/esp
cd ~/esp
```

Clone ESP-IDF v5.5.3:

``` bash
git clone --recursive \
  --branch v5.5.3 \
  https://github.com/espressif/esp-idf.git
```

Install the ESP32 toolchain and Python environment:

``` bash
cd ~/esp/esp-idf
./install.sh esp32
```

Activate the environment:

``` bash
. ./export.sh
```

Verify:

``` bash
idf.py --version
```

Expected version:

``` text
ESP-IDF v5.5.3
```

Run `. ~/esp/esp-idf/export.sh` in every new terminal session before
using ESP-IDF commands.

## 5. Download the ESP32 Project

Open PowerShell on Windows or Terminal on Linux.

Clone the repository:

``` bash
git clone --recursive \
  https://github.com/kmscarpio/esp32-tailscale-wake-on-lan-bridge.git
```

Enter the project directory:

``` bash
cd esp32-tailscale-wake-on-lan-bridge
```

Alternatively, download the repository ZIP from GitHub and extract it.

Make sure the project contains its original files, including:

-   `CMakeLists.txt`
-   `main` directory
-   `sdkconfig.defaults`, if provided
-   Other project files and components

Do not remove existing project files.

## 6. Configure the ESP32

Open the ESP-IDF environment and navigate to the project directory.

Example Windows path:

``` powershell
cd C:\ESP32\esp32-tailscale-wake-on-lan-bridge
```

Example Linux path:

``` bash
cd ~/esp/esp32-tailscale-wake-on-lan-bridge
. ~/esp/esp-idf/export.sh
```

Set the target:

``` bash
idf.py set-target esp32
```

Open the configuration menu:

``` bash
idf.py menuconfig
```

Configure the options actually provided by the firmware.

### Wi-Fi

-   Wi-Fi SSID
-   Wi-Fi password

### Tailscale

-   Device name
-   Tailscale authentication configuration, if required by the firmware
-   Network settings

### Wake-on-LAN

-   Target PC MAC address
-   LAN broadcast address
-   WOL destination UDP port, normally 9

### UDP command service

-   Listening port, normally 9999
-   Authentication token
-   Authorized source Tailscale IP addresses

### Reliability

-   Reconnection intervals
-   Watchdog settings
-   Memory thresholds
-   Command rate limits

Do not enable unauthenticated UDP access. Do not publish Wi-Fi
passwords, authentication keys, or command tokens.

Save the configuration and exit `menuconfig`.

## 7. Build the Firmware

Run:

``` bash
idf.py build
```

A successful build should display:

``` text
Project build complete.
```

The generated firmware files will be placed inside the project's `build`
directory. Resolve build errors before flashing.

## 8. Flash the ESP32

Connect the ESP32 through USB and identify its serial port.

### Windows

``` powershell
idf.py -p COM3 flash
```

Replace `COM3` with the actual port.

### Linux

``` bash
idf.py -p /dev/ttyUSB0 flash
```

Replace `/dev/ttyUSB0` with the actual serial device.

If the ESP32 does not enter download mode automatically:

1.  Hold the BOOT button.
2.  Press and release EN or RESET.
3.  Release BOOT.
4.  Retry flashing.

Wait for the flash process to complete.

## 9. Open the Serial Monitor

### Windows

``` powershell
idf.py -p COM3 monitor
```

### Linux

``` bash
idf.py -p /dev/ttyUSB0 monitor
```

Check the startup logs and verify that:

-   ESP32 boots successfully.
-   Wi-Fi connects.
-   Tailscale initializes and connects.
-   UDP listener starts.
-   Firmware configuration is valid.

Exit the serial monitor with `Ctrl + ]`.

## 10. Configure the Target PC for Wake-on-LAN

The target PC must support Wake-on-LAN from its intended power state.

### BIOS/UEFI

Enter BIOS and check for settings such as:

-   Wake on LAN
-   Power On By PCI-E
-   Resume by LAN
-   ErP

Enable the relevant Wake-on-LAN option. Disable ErP if it removes
standby power from the Ethernet adapter. Save and restart.

### Windows target PC

1.  Open Device Manager.
2.  Expand **Network adapters**.
3.  Open the Ethernet adapter properties.
4.  Open **Power Management**.
5.  Enable **Allow this device to wake the computer**.
6.  Enable **Only allow a magic packet to wake the computer**.

Under **Advanced**, enable available settings corresponding to:

-   Wake on Magic Packet
-   Shutdown Wake-on-LAN, if supported
-   Wake from shutdown, if supported

For troubleshooting, disable Windows Fast Startup if shutdown WOL does
not work.

### Linux target PC

Install `ethtool` on Ubuntu or Debian:

``` bash
sudo apt update
sudo apt install ethtool
```

Find the Ethernet interface:

``` bash
ip -br link
```

Example interface: `enp3s0`.

Check Wake-on-LAN support:

``` bash
sudo ethtool enp3s0
```

Look for `Supports Wake-on` containing `g`. The `g` option indicates
magic-packet support.

Enable magic-packet wake:

``` bash
sudo ethtool -s enp3s0 wol g
```

Verify:

``` bash
sudo ethtool enp3s0
```

Look for:

``` text
Wake-on: g
```

Replace `enp3s0` with the actual Ethernet interface.

#### Make Linux Wake-on-LAN persistent

The `ethtool` setting may reset after a reboot or network-interface
reinitialization. On a systemd-based distribution, create a service:

``` bash
sudo nano /etc/systemd/system/wol-enable.service
```

Add:

``` ini
[Unit]
Description=Enable Wake-on-LAN
After=network.target

[Service]
Type=oneshot
ExecStart=/usr/sbin/ethtool -s enp3s0 wol g
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

Check the `ethtool` executable path:

``` bash
command -v ethtool
```

If it differs from `/usr/sbin/ethtool`, update `ExecStart`. Replace
`enp3s0` with the actual Ethernet interface.

Enable the service:

``` bash
sudo systemctl daemon-reload
sudo systemctl enable --now wol-enable.service
```

Check its status:

``` bash
systemctl status wol-enable.service
```

After restarting Linux, verify:

``` bash
sudo ethtool enp3s0
```

BIOS support, Ethernet adapter support, motherboard standby power, and
Linux driver behavior determine whether Wake-on-LAN works from sleep or
full shutdown. Test each power state separately.

## 11. Install and Configure Tailscale

Install Tailscale on:

-   The target PC.
-   The iPhone or other remote device.

Use the official Tailscale installation instructions for each operating
system. Sign in using the same Tailscale account or tailnet.

The ESP32 must also be authenticated and connected through the
firmware's Tailscale implementation.

Record the Tailscale IP addresses of the ESP32 and authorized remote
devices.

### Windows target PC: Run unattended

1.  Find the Tailscale icon in the system tray.
2.  Right-click the icon.
3.  Hover over **Preferences**.
4.  Select **Run unattended**.
5.  Select Yes when prompted.

A check mark should appear next to Run unattended. This enables
Tailscale to continue running as a Windows service without requiring a
user to sign in.

Verify by restarting Windows and checking whether the PC appears online
before signing in.

### Linux target PC

Enable the Tailscale service:

``` bash
sudo systemctl enable --now tailscaled
```

Connect the machine to your tailnet:

``` bash
sudo tailscale up
```

Complete authentication using the displayed login URL.

Check the service:

``` bash
systemctl status tailscaled
```

Check the Tailscale connection:

``` bash
tailscale status
```

Check the device's Tailscale IP:

``` bash
tailscale ip -4
```

The `tailscaled` service should start automatically after reboot. For
unattended operation, review the device's key-expiry settings in the
Tailscale admin console.

## 12. Test Local Wake-on-LAN

Before testing remote access, test the ESP32 while the remote device is
connected to the home Wi-Fi.

Open your UDP Sender application.

  Setting            Value
  ------------------ -----------------------------------------
  Protocol           UDP
  Destination        ESP32 Tailscale IP or configured LAN IP
  Destination port   9999
  Encoding           UTF-8
  Listening port     9999

Use the actual command syntax configured in the firmware.

Test:

``` text
status YOUR_TOKEN
```

The expected response should indicate that the ESP32, Wi-Fi, and
Tailscale are operational.

Then test:

``` text
wake YOUR_TOKEN
```

Verify that the target PC wakes.

## 13. Test Remote Wake-on-LAN

Leave the ESP32 connected to Wi-Fi and powered by a USB wall adapter.
The ESP32 does not need to remain connected to the development computer.

On your iPhone or remote device:

1.  Open Tailscale.
2.  Confirm the ESP32 is online.
3.  Open UDP Sender.
4.  Configure the ESP32's Tailscale IP.
5.  Set destination port to 9999.
6.  Set the listening port to 9999.
7.  Send the status command.

Example:

``` text
status YOUR_TOKEN
```

Verify that the ESP32 returns a response.

Then send:

``` text
wake YOUR_TOKEN
```

The ESP32 should transmit a WOL magic packet to the target PC. Wait for
the target operating system to boot and reconnect to Tailscale.

## 14. Final Operating Architecture

``` text
                 iPhone
                   |
                Tailscale
                   |
              ESP32-WROOM-32
                   |
                  Wi-Fi
                   |
               Home Router
                   |
              LAN Broadcast
                   |
                Target PC
                   |
              Windows / Linux
                   |
              Tailscale Service
                   |
              PC Online Again
```

## 15. Final Verification Checklist

### ESP32

-   [ ] Firmware builds successfully.
-   [ ] Firmware flashes successfully.
-   [ ] Wi-Fi connects automatically.
-   [ ] Tailscale connects automatically.
-   [ ] UDP listener starts.
-   [ ] Authentication works.
-   [ ] Status command returns diagnostics.
-   [ ] Wake command sends a magic packet.
-   [ ] ESP32 reconnects after a power interruption.

### Target PC

-   [ ] BIOS Wake-on-LAN enabled.
-   [ ] Ethernet adapter configured for magic packets.
-   [ ] Sleep WOL tested.
-   [ ] Full shutdown WOL tested.
-   [ ] Tailscale installed.
-   [ ] Automatic Tailscale startup configured.
-   [ ] Windows Run unattended enabled, if using Windows.
-   [ ] Linux `tailscaled` service enabled, if using Linux.
-   [ ] Tailscale reconnects after reboot.

### Remote Access

-   [ ] Remote device Tailscale connected.
-   [ ] ESP32 reachable remotely.
-   [ ] Remote status command works.
-   [ ] Remote wake command works.
-   [ ] Target PC powers on.
-   [ ] Target PC reconnects to Tailscale automatically.

## 16. Important Notes

-   Keep the ESP32 powered continuously.
-   Use a stable USB power supply.
-   Do not expose the UDP listener directly to the public internet.
-   Keep authentication enabled.
-   Keep the original project structure when applying patches.
-   A successful UDP response does not prove that the PC received the
    WOL packet.
-   A successful WOL transmission does not prove that the target
    operating system's Tailscale service has reconnected.
-   Test sleep and full shutdown separately.
-   Run unattended is a Windows Tailscale setting, not an ESP32 setting.
-   Linux uses its systemd service to keep Tailscale running
    automatically.
-   ESP-IDF build and flashing commands are substantially the same on
    Windows and Linux, but serial port names and environment activation
    differ.

## Official References

-   [ESP-IDF downloads](https://dl.espressif.com/dl/esp-idf/)
-   [ESP-IDF v5.5.3 Linux
    setup](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32/get-started/linux-macos-setup.html)
-   [ESP-IDF
    documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32/)
-   [Git for Windows](https://git-scm.com/download/win)
-   [Silicon Labs CP210x
    drivers](https://www.silabs.com/software-and-tools/usb-to-uart-bridge-vcp-drivers)
-   [Tailscale downloads](https://tailscale.com/download)
-   [Tailscale Linux installation](https://tailscale.com/download/linux)
