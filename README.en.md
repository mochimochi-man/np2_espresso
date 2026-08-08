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
- USB wired keyboard
- USB wired mouse
- USB self-powered hub (one that can be powered from an AC adapter)

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
2. Connect the self-powered hub to the ESP32-S3's USB-OTG port, then connect the
   USB wired keyboard and USB wired mouse to the hub.
3. Connect power to the ESP32-S3's Serial USB port. If you hear the "pipo" chime
   after a few seconds and the memory count begins, it is working.

## Controls

- Press **Pause/Break** to open the menu.
- From the menu you can mount FDD1/FDD2/HDD disk images, change the CPU priority,
  and switch the downscaling algorithm.
- Raising the CPU priority lowers the graphics-rendering priority. If graphics
  rendering is affected, try lowering the CPU priority.
- Press **ESC**, or select **RESET**, to save the current state and leave the menu.

## License

```
BSD 3-Clause License

Copyright (c) 2026 Mochimochi-Man / Uh (X:@calorie0)

This license applies to the np2_espresso ESP32-S3 porting code authored for this
project (primarily the files under main/ and the ESP-IDF build glue). Bundled
third-party components retain their own licenses; see NOTICE.md and the LICENSES/
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
