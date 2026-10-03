# Run the sun252iw2 BSP on the EVB (xfel, no flash)

The BSP uses the RT-Thread device model: the board is described by `dts/sun252iw2-evb.dts` (+ `sun252iw2.dtsi`),
compiled by `board/SConscript` with `dtc` (set `DTC=<path of dtc>` when it is not in PATH) and linked into the
image. Drivers are platform drivers bound by the compatible strings of the nodes; pins, clocks, resets, DMA
channels and interrupts come from the device tree and nothing else.

Build (from `bsp/allwinner-hal/sun252iw2`):

    export RTT_EXEC_PATH=<xuantie toolchain>/bin     # riscv64-unknown-elf-*
    scons -j8                                        # -> rtt.bin, linked at 0x40000000
    rm rtthread.elf                                  # after a change of link.lds

Download and start (the board must be power cycled before every download, FEL is gone after `exec`):

    xfel ddr f101-s3
    xfel write 0x40000000 rtt.bin
    xfel exec 0x40000000

Console: UART3 (PE08/PE09), 115200 8N1 (`chosen/stdout-path`). Expected: RT-Thread banner, `msh >`.

## Drivers on the device model (all verified on the EVB with their test command)

| driver (drivers/...) | compatible | test |
|---|---|---|
| interrupt_controller/pic-plic.c (PLIC as a PIC) | sifive,plic-1.0.0 | every interrupt based test |
| clock_control/ccu-sun252i.c (clock + reset provider) | allwinner,sun252i-ccu | every driver |
| pinctrl/pinctrl-sun252i.c (pin groups + GPIO + pin interrupts) | allwinner,sun252i-pio | `test_gpio` |
| serial: the generic DW 8250 driver of the kernel | snps,dw-apb-uart | console, `test_kernel` |
| dma/dma-sun252i.c (RT dma controller API, channels 8..11 polled) | allwinner,sun252i-dma | `test_dma`, `test_dma_hi` |
| watchdog/watchdog-sun252i.c | allwinner,sun252i-wdt | `test_wdt` |
| i2c/i2c-sun252i.c | allwinner,sun252i-i2c | `test_i2c` (no slave reachable on the EVB) |
| spi/spi-sun252i.c (SPI NOR on spi0) | allwinner,sun252i-spi | `test_spi` |
| pwm/pwm-sun252i.c | allwinner,sun252i-pwm | `test_pwm` |
| adc/adc-sun252i.c | allwinner,sun252i-gpadc | `test_adc` |
| sdio/sdio-sun252i.c (SD card, `cd-gpios`) | allwinner,sun252i-mmc | `test_sd` |
| mbus/mbus-sun252i.c | allwinner,sun252i-mbus | `test_mbus` |
| audio/codec-sun252i.c (RT audio device `audio0`, DAC + ADC with DMA rings) | allwinner,sun252i-codec | `test_audio`, `test_mic` |
| audio/i2s-sun252i.c (I2S0, internal loopback only: no pins on the EVB) | allwinner,sun252i-i2s | `test_i2s` |
| audio/owa-sun252i.c (S/PDIF transmitter; the receiver DMA gets no request) | allwinner,sun252i-owa | `test_owa` |
| pwm/pwm-bl-sun252i.c (panel backlight, device `pwm_bl0`) | allwinner,sun252i-pwm-bl | `test_backlight` |
| display (graph built from the nodes at boot: `ofw_graph.c`; DE, TCON, RGB/LVDS/DSI, D-PHY, panel, backlight) | allwinner,sun252iw2-display-engine, ..., panel-simple, pwm-backlight | `test_display` (look at the panel) |
| g2d/g2d-sun252i.c (`g2d/g2d.h`) | allwinner,sunxi-g2d | `test_g2d` |
| vdec/ (video engine, prebuilt decoder archive in `vdec/lib`, `vdec/vdec.h`) | allwinner,sunxi-ve | `test_vdec` |
| mipi_dbi/mipi-dbi-sun252i.c | allwinner,sunxi-dbi | `test_dbi` (no panel behind it) |
| usb/phy/usb-phy-sun252i.c (shared UTMI PHY, VBUS gpio) | allwinner,sunxi-usb-phy | with the two below |
| usb/cherryusb/usb-otg-sun252i.c (CherryUSB MUSB device) | allwinner,sunxi-musb | `usb_device_start`, `usb_device_send` (tests: a COM port on the host PC) |
| usb/cherryusb/usb-hci-sun252i.c + usb_hc_ohci.c (CherryUSB EHCI/OHCI host) | allwinner,sunxi-ehci | `usbh_start` (not run: the single port is the download cable) |

`test_all` runs the self contained ones in turn (the tests and the benchmarks are not built unless BSP_USING_TESTS / BSP_USING_BENCH is defined in `rtconfig.h`, see "Tests and benchmarks"). The device tree is the only place that names pins, bases, interrupts,
clocks and resets; `drivers/clock_control/ccu-sun252i.c` is the one place that knows the clock registers.

## Tests and benchmarks (applications/tests, applications/bench)

Both are off by default. `#define BSP_USING_TESTS` in `rtconfig.h` builds the `test_*` commands and `test_all`;
`#define BSP_USING_BENCH` builds the speed measurements: `g2d_bench`, `g2d_bench_clk`, `h264_bench`, `bench_stop`,
`usb_bench_start`, `usb_bench_stat` and the other `usb_bench_*` commands. The bus tools `mbus_stats`, `mbus_masters`,
`mbus_set` and `mbus_find` are part of the MBUS driver.

The board is chosen with the environment variable `SUN252I_BOARD` when building: `evb` (default, SD card slot), `evb-jtag` (the
pins of the slot as JTAG port), `evb-h264` (display, G2D, DBI, USB, audio and PWM off: the 16 MB PSRAM is left to the video
engine, the 1024x600 frame buffer alone is 2.4 MB) and `yuzukineko`.

### G2D (`g2d_bench`, `g2d_bench_clk`)

1280x720 ARGB8888, hardware time only (command list start to end interrupt, no cache maintenance, no queueing), 20 runs each,
module clock 300 MHz:

| operation | hardware time | memory traffic |
|---|---|---|
| fill | 3.86 ms | 952 MB/s |
| copy | 10.4 ms | 710 MB/s |
| RGB565 to ARGB8888 | 7.6 ms | 725 MB/s |
| NV12 to ARGB8888 | 7.2 ms | 707 MB/s |
| scale 640x360 to 720p | 6.3 ms | 735 MB/s |
| rotate 90 | 19.1 ms | 385 MB/s |
| flip horizontal | 23.4 ms | 315 MB/s |
| blend src-over | 17.0 ms | 651 MB/s |

Only the fill depends on the module clock below 300 MHz (150 MHz 6.15 ms, 200 MHz 4.62 ms); everything else is the same from 150 to
600 MHz, the memory is the limit. 1200 MHz (PLL_PERI_2X undivided) does not work. The G2D is bus master 13; with that master at
priority 3 and no limit, blend drops to 13.2 ms, rotate to 17.4 ms and NV12 conversion to 6.8 ms, fill and copy do not change.

### H.264 decode (`h264_bench <file> [ve_mhz [max_frames [no_cache]]]`)

Decodes a raw Annex B file of the SD card as fast as it goes (no sound, no picture, no pacing), cuts it into pictures, times the
decode of each with the 24 MHz counter and prints fps, the decode time of I, P (and B) pictures and the memory traffic of the video
engine (MBUS counter `ve`); `bench_stop` ends a run early and prints the summary. Run it on the `evb-h264` board (the default
board has too little free heap for 720p).

1280x720 stream (11 IDR pictures of 73 KB, P pictures of 6 KB):

| | |
|---|---|
| frame rate | 33 fps (30-35 over every 5 s window) |
| I picture decode | 26.8 ms |
| P picture decode | 28.4 ms (18 ms at best) |
| video engine memory traffic | 435 MB/s, 13.1 MB per picture |

What does not help: the video engine clock (200 MHz 32.1 ms, 300 MHz 30.1, 400 MHz default 29.2, 600 MHz 28.4 per P picture),
MBUS priority 3 and no limit for the video engine (master 4: `mbus_set 4 3 0`; `mbus_find 4` finds a master while a load runs), skipping the
cache maintenance of the output (`no_cache` / `vdec_stream_config.no_cache_ops`, 2%). The decode is limited by the memory.

## MP4 player (applications/apps/mp4)

`mp4_play <file>` plays a file of the SD card (FAT, mounted on `/` on first use), `mp4_stop` stops it. The H.264 track
is decoded by the video engine and shown on the video plane of the display (`lcd_show_yuv`), the AAC track is decoded in
software (`applications/apps/mp4/aac`) and played by the `audio0` codec; the DAC sample counter is the clock, late pictures
are dropped. Measured with a 960x540 30 fps clip: 30.0 fps, 0 dropped, A/V offset within 8 ms. 48000 Hz and 44100 Hz
sound is played, other rates only show the picture. The heap is 12 MiB (`RT_HW_HEAP_BEGIN`) because the decoder takes
its frame buffers from it.

## USB device performance (applications/apps/usb_cdc)

The bulk endpoints (all but EP0) can be moved by the OTG's internal DMA (`CONFIG_USB_MUSB_DMA`, on by default (`usb_config.h`); buffers that are not 64 byte aligned stay on the CPU, a short packet is confirmed after the DMA has been still for 50 us; whole packets by DMA,
short packets and tails by the CPU, the DMA ends with an interrupt on the shared OTG line). VEND0 bit 0 stays set in
both modes, the CPU/DMA choice is made in the endpoint CSR. Measured on the CDC ACM port with 16 KiB transfers and
a pattern check of both directions (0 errors, odd write sizes included):

| | PIO | DMA |
|---|---|---|
| OUT (host to board) | 14.2 MB/s, 49% CPU | 26.3 MB/s, 7% CPU |
| IN (board to host) | 22 MB/s, 39% CPU | 23.9 MB/s, 6% CPU |

The rates are limited by the Windows serial driver. The measurement is in `applications/bench` (BSP_USING_BENCH): `usb_bench_start`
brings the CDC port up, then `usb_bench_cpu` (idle rate, run it first and with no traffic), `usb_bench_src 1|0` (IN stream),
`usb_bench_check 1|0` (verify the OUT pattern, costs CPU: turn it off for the CPU figure), `usb_bench_load 1|0` (memory
traffic in the background) and `usb_bench_stat` (rates and load over the last burst); host side `usbbench.ps1 -Mode out|in
-Seconds N [-Odd 1000]` on the PC. The tests' `usb_regs` also dumps the DMA channels.

## USB second screen (applications/apps/usb_display)

`usb_display_start` makes the OTG port enumerate as a virtual display (VID 303A, PID 2987, product string
`sun252iw2_R1024x600_Ejpg6_Fps30_Bl500`: name, resolution, JPEG quality 6, 30 fps, 500 KB frame buffer limit). Install the
Windows driver `modules/lib/cherryusb/tools/display/xfz1986_usb_graphic_250224_rc_sign.exe` on the PC (administrator, the
driver is test signed); Windows then shows a second monitor that the board displays. The PC sends JPEG frames over a bulk
endpoint, the video engine decodes them through the motion JPEG stream of `vdec_stream_*` and the picture goes to the video
plane. The statistics line every 5 s shows frames per second, KB/s and the decode and display time per frame.
`usb_display_selftest` runs a built-in 320x240 JPEG through the same decode and display path without USB
(3 ms decode per frame). The port stays a display until the next reboot: the CDC device (`usb_device_start` of the tests, `usb_bench_start` of the bench) and
`usb_display_start` exclude each other. After a download with xfel run `usb_reconnect` once so that the PC sees a fresh attach.

The size is chosen at start: `usb_display_start [width height [fps [quality 1..10 [frame limit KB]]]]` (default: the panel size, 60 fps,
quality 9, 500 KB). The size goes to the PC in the product string of the USB descriptor, so it cannot change before a reboot;
Windows then makes one monitor of exactly that size (the driver offers a single mode, no list to pick from) and the video plane of the
display engine scales the picture to the panel, keeping its shape. The frame limit has to be large enough for the size: with
`Bl128` the driver refused 1920x1080 as against its specification and fell back to 1280x720. Size limits are the memory (16 MB
PSRAM): the decoder needs three 16-aligned NV12 pictures at least (4.5 bytes per pixel), the stream buffer and the frame buffers come on top, about
7.6 MB are left with the display on, so about 1.6 million pixels at most: 800x480, 1024x600, 1280x720 and 1600x900 work (1600x900:
41 ms decode per frame, the show waits for the refresh because only one picture is kept), 1920x1080 and 4K do not (`usb_display_start`
says so; a 4K NV12 picture alone is 12.4 MB). With room for four pictures (about 1.27 million pixels) the show does not wait.


### Touch over USB (`usb_display_start ... touch`)

A trailing `touch` makes the device composite (PID 0x2986): interface 0 is the display (the display driver binds it as
`MI_00`), interface 1 is a HID multi-touch digitizer (interrupt IN 0x83, 5 contacts, absolute 0..32767, report ID 1, feature
report with the contact maximum). Windows loads it with the inbox HID driver as a touch screen (`GetSystemMetrics(SM_DIGITIZER)` =
0xC1: integrated touch, multi-input, ready; 5 touches) with nothing to install. `usb_touch_demo [1|2]` sends a synthetic
diagonal drag (two mirrored fingers with 2) to look at what the PC does with it. `usb_touch_send()` in `usb_touch.h` is the
entry point for a real touch controller. Which monitor the touch lands on is up to Windows (Settings > Touch / Tablet PC
Settings can map it to the board's monitor).

## USB display rates

`usb_display_start` with `Fps60` in the product string: 1024x600 JPEG at 38-42 fps with the window moving (4.5-4.8 MB/s), decode 17 ms and show 1 ms per frame, CPU 20-24% with the OTG DMA (28-38% with CPU copies), 0 broken frames over 4 minutes. Frames are checked for SOI/EOI before decoding; a broken one is counted and skipped (it used to show as green or garbled pictures).
