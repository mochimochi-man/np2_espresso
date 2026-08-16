# PC-9801 emulator np2 espresso
Copyright (c) 2026 Mochimochi-Man / Uh (X:@calorie0)

This project is an emulator that runs the classic retro PC 'PC-9801 series' on the
ESP32-S3 (or any ESP32 of equal or greater performance).

It is based on [np2kai](https://github.com/AZO234/NP2kai) by AZO234, with
modifications and customizations to run on the ESP32.

## Required hardware

- ESP32-S3 N16R8 Devkit
- LCD ST7789 TFT 240x320 module
- DAC MAX98357A I2S module
- microSD card reader (SPI) module
- microSD card, 32GB or less, formatted as FAT32
- Passive speaker, around 2W 8ohm (connected to the MAX98357A)
- A keyboard and a mouse, either of:
  - USB wired keyboard / USB wired mouse, plus a USB self-powered hub (one that can
    be powered from an AC adapter)
  - **Bluetooth keyboard / Bluetooth mouse** (see the limitations below)

## Required software

- ESP-IDF v5.5 series
- `BIOS.ROM` / `SOUND.ROM` / `FONT.ROM` extracted from real PC-9801 hardware.
  Place them in the root of the microSD card.
- Disk images such as `.NFD` or `.NHD`. Place them in the root of the microSD card.

## Wiring

### LCD ST7789 TFT 240x320 Module

| Signal | GPIO |
|--------|-----:|
| MOSI (SDA) | 11 |
| SCLK (SCL) | 12 |
| CS | 10 |
| DC | 9 |
| RST | unused |
| MISO | unused |
| BL | 3.3V |
| VCC | 3.3V |
| GND | GND |

### DAC MAX98357A I2S Module

| Signal | GPIO |
|--------|-----:|
| BCLK | 38 |
| LRCLK | 39 |
| DOUT | 40 |
| VCC | 5V |
| GND | GND |

Connect a speaker of about 2W 8ohm to the +/- terminals. (+RED, -BLACK)

### microSD CardReader SPI

| Signal | GPIO |
|--------|-----:|
| CS | 4 |
| SCK | 5 |
| MISO | 6 |
| MOSI | 7 |
| VCC | 3.3V |
| GND | GND |

## Build

Build:

```sh
./build.sh
```

Or activate ESP-IDF first, then run `idf.py`:

```sh
. $IDF_PATH/export.sh
idf.py build
```

This produces `build/np2_espresso.bin`. Flash and monitor:

```sh
./flash.sh [PORT]
```

Exit the monitor with `Ctrl-]`.

## Running

1. Insert a microSD card (with the required disk images) into the card reader.
2. For USB input, connect the self-powered hub to the ESP32-S3's USB-OTG port and
   plug the keyboard and mouse into the hub. Bluetooth devices need no wiring.
3. Connect power to the ESP32-S3's Serial USB port. If you hear the "pipo" chime
   after a few seconds and the memory count begins, it is working.

USB and Bluetooth work at the same time, and either one on its own is fine.

## Connecting a Bluetooth keyboard / mouse

The emulator looks for Bluetooth keyboards and mice on its own after power-on.
**Put the device into pairing mode** and it will be connected and usable; this can
take a few tens of seconds.

Pairing keys are stored on the board, so from then on the device reconnects
automatically at power-on.

Once both a keyboard and a mouse are connected the radio is switched off and the
search ends, to keep it away from the display and the microSD card. While only one
of them is connected the search continues, so a device switched on later still
gets in.

### If you do not use Bluetooth

Nothing to do. The search runs for about two minutes after power-on and then goes
nearly idle, checking briefly once a minute - so a Bluetooth device switched on
later still connects, and a USB-only setup is not affected.

Using Bluetooth for only the keyboard or only the mouse is fine too; plug the other
one in over USB.

### Limitations

- **Bluetooth LE (BLE / HOGP) devices only.** The ESP32-S3 has no Bluetooth Classic
  (BR/EDR) transceiver at all, so a Classic-only keyboard or mouse cannot be
  supported by any amount of software. If a device pairs with a phone or a tablet
  as a HID device, it is almost certainly BLE.
- Pairing is "Just Works". A device that insists on 6-digit passkey entry cannot be
  paired.
- Up to 8 pairings are stored. A full list blocks new pairings, so when that
  happens it is wiped automatically at the next boot (with a note on the console)
  and the devices have to be paired again.

## Controls

- Press **Pause/Break** to open the menu. **F11** and **F12** open it too, for the
  many keyboards (Bluetooth ones especially) that have no Pause key.
- From the menu you can mount FDD1/FDD2/HDD disk images, change the CPU priority,
  switch the downscaling algorithm, and set the **LCD SPI clock** and the
  **LCD colour depth**.
- **LCD SPI**: 80 / 40 / 20 MHz (40MHz by default). Lower it if the display is
  corrupted. ST7789 emulators such as LcdTap in particular can break up at 40MHz
  on busy screens (games) while looking fine on menus - choose 20MHz for those.
- **LCD color**: RGB444 (12-bit, the default) or RGB565 (16-bit). RGB444 sends
  three quarters of the bytes per pixel, so drawing is faster. Choose RGB565 if the
  reduced colour depth bothers you.
- Settings are stored on the board and survive a power cycle.
- Raising the CPU priority lowers the graphics-rendering priority. If graphics
  rendering is affected, try lowering the CPU priority.
- Press **ESC**, or select **RESET**, to save the current state and leave the menu.

## License

```
BSD 3-Clause License

Copyright (c) 2026 Mochimochi-Man / Uh (X:@calorie0)

This license applies to the np2_espresso ESP32-S3 porting code authored for this
project (primarily the files under main/ and the ESP-IDF build glue). Bundled
third-party components retain their own licenses; see NOTICE.en.md and the LICENSES/
folders within each component.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

For related third-party licenses, see the accompanying [NOTICE.en.md](NOTICE.en.md).
