# np2_espresso — サードパーティ・ライセンス表記 (NOTICE)

np2_espresso は PC-9801 (VM21 相当 / V30) エミュレータ NP2kai を ESP32-S3 に移植した
ものです。本配布物には以下の第三者コードが含まれます。各コードの著作権は各権利者に帰属し、
それぞれのライセンス条件に従って再頒布されます。

------------------------------------------------------------------------------

## 1. NP2kai / Neko Project II · 21/W (エミュレータ本体)

- 収録範囲: `components/np2kai/`
  (i286c コア, io, mem, vram, fdd, font, generic, lio, bios, cbus, codecnv,
   common, trap, diskimage, および sound/ の native 音源: opngen/OPNA, PSG, beep,
   ADPCM, PCM86, TMS3631, rhythm, CS4231, CT1741, OPL3 など)
- ライセンス: **修正BSDライセンス (3条項BSD)** が大半。
- 一部の周辺コンポーネント (別ライセンス) の詳細は
  `components/np2kai/np2kai/LICENSES/` を参照してください。
- 内蔵フォント (`font/fontdata`) は NP2 作者オリジナルの ANK 代替フォントであり、
  実機 (NEC/EPSON) の CGROM ダンプではありません。修正BSD。
- 上流: https://github.com/AZO234/NP2kai / https://simk98.github.io/np21w/

## 2. TFT_eSPI (ディスプレイ描画ライブラリ)

- 収録範囲: `components/TFT_eSPI/`
- 著作権: Copyright (c) Bodmer
- ライセンス: **FreeBSD License (2条項BSD相当)** — `components/TFT_eSPI/LICENSE`
- 上流: https://github.com/Bodmer/TFT_eSPI

## 3. np2_espresso ESP32 移植コード (本プロジェクト独自部分)

- 収録範囲: `main/`, 各 `CMakeLists.txt`, `sdkconfig.defaults`, ビルドスクリプト等
- ライセンス: ルートの `LICENSE` を参照。

------------------------------------------------------------------------------

## この配布物から除去したコンポーネント

配布・ライセンスを単純化するため、ビルドに使用しない以下のコードを物理的に削除しました。
（いずれも本エミュレータのビルド構成 (i286c コア + native 音源) では未使用）

- `sound/fmgen/` — cisc 製 FM 音源 (cisc 独自ライセンス)
- `sound/mame/` — MAME OPL (**GPL**)
- `sound/mamebsd/`, `sound/mamebsdsub/` — ymfm (3条項BSD)
- `sound/vermouth/` — GM/MIDI ソフトシンセ
- `i386c/` — IA-32 (386) コア一式。DOSBox 由来 FPU (**GPLv2**) および
  Berkeley SoftFloat (BSD) を含む
- `sdl/cmmidi.c` — MIDI 出力通信
- 上記に対応する `LICENSES/` 内の孤立ライセンス文書

> 結果として、ビルドされるバイナリおよび主要ソースは寛容ライセンス (BSD 系) のみで
> 構成されます。**GPL コードは配布物から除去済みです。**

## 補足 (未使用だが残置しているサブツリー)

`components/np2kai/np2kai/` 配下には、ESP32 ビルドでは未使用ながら NP2kai 由来の
以下のサブツリーが残っています (いずれもビルド対象外)。それぞれのライセンスは
`LICENSES/` 内の該当ファイルを参照してください。必要なら追加で削除できます。

- `wab/` (Cirrus GD54xx / TGUI9680 VGA), `windows/`, `wx/`, `x/` (各OS向けGUI),
  `network/` (LGY-98 等), `embed/`, `np2tool/`, `romimage/`, `textnorm/`,
  `sdl/` (cmmidi.c を除く)
