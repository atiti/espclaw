# OTA

ESPClaw now supports direct HTTP firmware OTA uploads on real firmware targets.

## Important Migration Note

Existing boards need one final serial flash to migrate onto the OTA-capable partition table.

The new layout uses:

- `ota_0`
- `ota_1`
- `otadata`
- `workspace`

After that one serial migration flash, future updates can be pushed from the admin UI over HTTP.

## Admin Flow

Open the admin UI on the device and use the `Firmware OTA` card:

1. Check OTA status.
2. Choose the built `espclaw_firmware.bin`.
3. Click `Upload and Reboot`.

The firmware streams the uploaded image into the inactive OTA slot, marks it for next boot, and schedules a reboot.

## Boot Validation

On the first boot into a newly uploaded image, ESPClaw confirms the running partition with ESP-IDF's rollback API. That means:

- a successfully booted image becomes the new confirmed slot
- a pending image is explicitly confirmed after the runtime and admin server come up

## Current Scope

The implemented OTA path is:

- local admin HTTP upload
- write to inactive OTA slot
- boot partition switch
- scheduled reboot
- boot confirmation on next startup

It does not yet include:

- signed image verification beyond normal ESP-IDF image validation
- remote manifest polling
- delta updates
