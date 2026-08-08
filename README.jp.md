# PC-9801 emulator np2 espresso
Copyright (c) 2026 もちもちまん / うっ (X:@calorie0)

本プロジェクトは、ESP32-S3 または同等以上の性能を有する ESP32 シリーズ上において、往年のレトロパソコン PC-9801 シリーズをエミュレートするエミュレーターです。

AZO234 氏が開発された [np2kai](https://github.com/AZO234/NP2kai) をベースに、ESP32 で動作するよう修正とカスタマイズを加えました。

## 必要なハードウェア

- ESP32-S3 N16R8 Devkit
- LCD ST7789 TFT 240x320 モジュール
- DAC MAX98357A I2S モジュール
- microSD カードリーダー SPI モジュール
- microSD カード 32GB 以下 ※FAT32 でフォーマット
- パッシブスピーカー 2W 8ohm 程度 ※MAX98357A に接続
- USB 有線キーボード
- USB 有線マウス
- USB セルフパワーハブ ※AC アダプタから給電できるもの

## 必要なソフトウェア

- ESP-IDF v5.5 系
- PC-9801 系実機から抽出した `BIOS.ROM` / `SOUND.ROM` / `FONT.ROM` ※microSD カードのルートに入れてください。
- `.NFD` や `.NHD` などのディスクイメージ ※microSD カードのルートに入れてください。

## 配線

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

※ +/- 端子に 2W 8ohm 程度のスピーカーを接続してください。(+RED, -BLACK)

### microSD CardReader SPI

| Signal | GPIO |
|--------|-----:|
| CS | 4 |
| SCK | 5 |
| MISO | 6 |
| MOSI | 7 |
| VCC | 3.3V |
| GND | GND |

## ビルド方法

ビルド:

```sh
./build.sh
```

または ESP-IDF を有効化してから `idf.py` を実行:

```sh
. $IDF_PATH/export.sh
idf.py build
```

`build/np2_espresso.bin` ができるので、書き込み＋モニタ:

```sh
./flash.sh [PORT]
```

モニタ終了は `Ctrl-]`。

## 実行

1. microSD カードリーダーに、所定のディスクイメージを入れた microSD カードを挿入します。
2. ESP32-S3 の USB-OTG 側 USB 端子にセルフパワーハブを接続し、ハブに USB 有線キーボードと USB 有線マウスを接続してください。
3. ESP32-S3 の Serial 側 USB 端子に電源を接続してください。数秒後に「ピポ」と音が鳴ってメモリカウントが始まれば成功です。

## 操作方法

- **Pause/Break** キーを押すとメニューを表示します。
- FDD1/FDD2/HDD のディスクマウント、CPU の優先度、縮小表示のアルゴリズム切替を行うことができます。
- CPU の優先度を上げるとグラフィックスの描画優先度が下がります。グラフィックスの描画に支障がある場合は、CPU の優先度を下げてみてください。
- **ESC** キーを押す、または **RESET** を選択すると、現在の状態を保存してメニューから抜けます。

## ライセンス

```
BSD 3-Clause License

Copyright (c) 2026 Mochimochi-Man / Uh (X:@calorie0)

This license applies to the np2_espresso ESP32-S3 porting code authored for this
project (primarily the files under main/ and the ESP-IDF build glue). Bundled
third-party components retain their own licenses; see NOTICE.jp.md and the LICENSES/
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

関連ライセンスは、別添 [NOTICE.jp.md](NOTICE.jp.md) をご覧ください。
