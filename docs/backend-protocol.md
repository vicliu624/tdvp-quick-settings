# Backend protocol

The UI deliberately does not write K230 GPIO, pinctrl, ALSA route controls, or
NetworkManager connection profiles directly. It talks to the board provider
over a local, access-controlled Unix `SOCK_SEQPACKET` socket.

```
/run/vicliu-pocket-linux-hardware/quick-settings.sock
```

The provider grants the logged-in desktop user access to the socket and is the
only process that can perform privileged board actions. Requests and responses
are newline-delimited UTF-8 records. Every response includes an explicit
`result=ok` or `result=error` field; the UI must refresh authoritative state
after a successful action rather than infer success from a button press.

## Read state

```
request:  GET_STATE\n
response: result=ok
          dock_nrf9151_sku_state=lte-present
          gps_available=1
          gps_state=fix
          bluetooth_available=1
          bluetooth_control_available=1
          bluetooth_enabled=0
          lora_available=1
          lora_enabled=0
          radio_profile=nrf9151
          ...
```

## Write actions

The initial K230 provider action set is intentionally narrow:

```
SET display-brightness 0..100
SET keyboard-backlight 0..100
SET speaker-route external|internal
SET speaker-volume 0..100
SET speaker-mute mute|unmute|toggle
SET bluetooth-power on|off
SET lora-power on|off
SET gnss-power on|off
SET radio-profile lora|nrf9151
SYSTEM reboot
SYSTEM poweroff
```

`SET gnss-power on` must fail unless the keyboard-mounted nRF9151 was verified.
`SET lora-power on` and `SET gnss-power on` are mutually exclusive operations;
the provider, not the UI, enforces the safe pin/power sequence.

On a verified nRF9151 keyboard, `SET gnss-power on` selects the nRF9151 board
profile, waits for its AT proxy, then asks the modem to enter the documented
LTE-M + GNSS system mode and activate GNSS without activating LTE. The provider
reports `gps_state=starting` and then `searching`; it must not report `fix`
until a future GNSS/NMEA consumer has measured a real position. Turning GPS
off tears down the nRF9151 profile before the radio can be reassigned to LoRa.

Wi-Fi remains an unprivileged desktop-session concern. The UI uses
NetworkManager's supported `nmcli` interface and never sends Wi-Fi credentials
through the board socket. Audio output routing, volume and mute are board
controls: the provider owns the `K230_I2S_INNO` ALSA mixer and its `PCM`
playback control, so an unrelated PulseAudio virtual sink cannot make the UI
appear to change the speaker volume without changing the physical output.

Screen locking is also a desktop-session action: the K230 profile installs
`swaylock` with the existing PAM service and the UI launches it directly from
the logged-in Wayland session. The board provider deliberately does not
pretend that a root-side `loginctl` call can lock a `dbus-run-session` Labwc
desktop.
