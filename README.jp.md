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
- キーボードとマウス（下記のいずれか）
  - USB 有線キーボード / USB 有線マウス ＋ USB セルフパワーハブ ※AC アダプタから給電できるもの
  - **Bluetooth キーボード / Bluetooth マウス** ※後述の制限あり

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
2. USB キーボード / マウスを使う場合は、ESP32-S3 の USB-OTG 側 USB 端子にセルフパワーハブを接続し、ハブに接続してください。Bluetooth キーボード / マウスを使う場合は配線不要です。
3. ESP32-S3 の Serial 側 USB 端子に電源を接続してください。数秒後に「ピポ」と音が鳴ってメモリカウントが始まれば成功です。

USB と Bluetooth は同時に使えます。片方だけでも構いません。

## Bluetooth キーボード / マウスの接続方法

電源投入後、自動的に周囲の Bluetooth キーボード / マウスを探します。**機器側をペアリングモードにしてください。** 接続されるとそのまま使えます。数十秒かかることがあります。

ペアリング情報は本体に保存されるので、次回以降は電源を入れるだけで自動的に再接続します。

キーボードとマウスの両方が接続されると、電波を止めて探索を終了します（描画と microSD アクセスへの影響を避けるため）。片方だけ接続されている状態では、あとから電源を入れた機器を拾えるよう探索を続けます。

### Bluetooth を使わない場合

何もしなくて構いません。起動から約 2 分のあいだ機器を探し、見つからなければ探索をほぼ停止します（以後は 1 分に 1 回だけ短く確認するので、あとから Bluetooth 機器の電源を入れても繋がります）。USB キーボード / マウスだけで使う場合も動作に影響はありません。

キーボードだけ、マウスだけを Bluetooth にすることもできます。足りないほうを USB で繋いでも構いません。

### 制限事項

- **Bluetooth LE（BLE / HOGP）の機器のみ対応します。** ESP32-S3 は Bluetooth Classic（BR/EDR）の送受信機を搭載していないため、Classic 専用のキーボード / マウスはソフトウェアでは対応できません。スマートフォンやタブレットとペアリングできる機器であれば、ほぼ BLE です。
- ペアリングは「Just Works」方式です。6 桁のパスキー入力を要求する機器は接続できません。
- ペアリング情報は 8 台まで保存されます。満杯になると新規登録ができなくなりますが、その場合は起動時に自動で全消去し、ログにその旨を表示します（登録し直してください）。

## 操作方法

- **Pause/Break** キーを押すとメニューを表示します。このキーを持たないキーボード（Bluetooth 機器に多い）のために、**F11** と **F12** でも開きます。
- FDD1/FDD2/HDD のディスクマウント、CPU の優先度、縮小表示のアルゴリズム、**LCD の SPI クロック**、**LCD の色数**の切替を行うことができます。
- **LCD SPI**: 80 / 40 / 20 MHz を選べます（既定 40MHz）。表示が乱れる LCD では下げてください。特に LcdTap のような ST7789 エミュレータでは、ゲーム画面など転送量の多い場面で 40MHz でも乱れることがあり、その場合は 20MHz を選択します。
- **LCD color**: RGB444（12bit、既定）と RGB565（16bit）を選べます。RGB444 は 1 画素あたりの転送量が 3/4 になるため描画が速くなります。色数の違いが気になる場合は RGB565 にしてください。
- 設定は本体に保存され、次回起動時も維持されます。
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
