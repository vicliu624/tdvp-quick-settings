# TDVP K230 integration

This directory contains the pieces that make `tdvp-quick-settings` a default
enhancement on a TDVP K230 image. They are intentionally not installed by the
generic Buildroot package.

The firmware integration must:

1. Install the autostart entry only after Labwc, NetworkManager, PulseAudio,
   and `vpl-hardwared` are available.
2. Keep the existing wf-panel-pi configuration independent. The Quick Settings
   package is additive and must not replace `wf-panel-pi.ini` or Labwc's
   session scripts.
3. Publish the documented `dock_nrf9151_sku_state`, `gps_available`,
   `gps_state`, `lora_available`, `lora_enabled`, and `radio_profile` state.
4. Expose the narrow control socket described in
   `docs/backend-protocol.md` with access only for the authenticated desktop
   account.
5. Build the image with an immutable version/tag of this repository, never a
   mutable branch checkout.

The generic application only renders a GPS card when
`dock_nrf9151_sku_state=lte-present`. On a no-LTE keyboard, the fourth primary
card becomes the on-screen keyboard shortcut instead.

