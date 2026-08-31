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
SET lora-power on|off
SET gnss-power on|off
SET radio-profile lora|nrf9151
SYSTEM lock
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

Wi-Fi and PulseAudio are unprivileged desktop-session concerns. The UI uses
their supported user/session APIs separately and does not proxy Wi-Fi
credentials through the board socket.
