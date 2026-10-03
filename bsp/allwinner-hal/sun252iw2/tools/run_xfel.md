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
| usb/cherryusb/usb-otg-sun252i.c (CherryUSB MUSB device) | allwinner,sunxi-musb | `usb_device_start`, `usb_device_send` (a COM port on the host PC) |
| usb/cherryusb/usb-hci-sun252i.c + usb_hc_ohci.c (CherryUSB EHCI/OHCI host) | allwinner,sunxi-ehci | `usbh_start` (not run: the single port is the download cable) |

`test_all` runs the self contained ones in turn. The device tree is the only place that names pins, bases, interrupts,
clocks and resets; `drivers/clock_control/ccu-sun252i.c` is the one place that knows the clock registers.
