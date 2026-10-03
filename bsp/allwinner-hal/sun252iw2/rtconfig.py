import os

# toolchains options
ARCH        = 'risc-v'
VENDOR      = ''
CPU         = 'c907'
CROSS_TOOL  = 'gcc'

RTT_ROOT = os.getenv('RTT_ROOT') or os.path.join(os.getcwd(), '..', '..', '..')

if os.getenv('RTT_CC'):
    CROSS_TOOL = os.getenv('RTT_CC')

if CROSS_TOOL == 'gcc':
    PLATFORM    = 'gcc'
    EXEC_PATH   = os.getenv('RTT_EXEC_PATH') or os.path.join(os.getenv('CROSS_COMPILE', '/opt/riscv/bin/x'), '..')
else:
    print('Please make sure your toolchains is GNU GCC!')
    exit(0)

BUILD = 'debug'

if PLATFORM == 'gcc':
    PREFIX  = os.getenv('RTT_CC_PREFIX') or 'riscv64-unknown-elf-'
    CC      = PREFIX + 'gcc'
    CXX     = PREFIX + 'g++'
    AS      = PREFIX + 'gcc'
    AR      = PREFIX + 'ar'
    LINK    = PREFIX + 'gcc'
    TARGET_EXT = 'elf'
    SIZE    = PREFIX + 'size'
    OBJDUMP = PREFIX + 'objdump'
    OBJCPY  = PREFIX + 'objcopy'

    # XuanTie C907 (rv32imac in the SoC book, the core also has FPU/DSP):
    # machine mode, the BROM / the loader starts the image directly.
    DEVICE  = ' -mcmodel=medany -march=rv32imafdc_zicsr_zifencei_xtheadcmo -mabi=ilp32d -mno-relax'
    CFLAGS  = DEVICE + ' -Wno-cpp -ffreestanding -fno-common -ffunction-sections -fdata-sections -fstrict-volatile-bitfields'
    AFLAGS  = ' -c' + DEVICE + ' -x assembler-with-cpp -D__ASSEMBLY__'
    LFLAGS  = DEVICE + ' -nostartfiles -Wl,--gc-sections,-Map=rtthread.map,-cref,-u,_start -T link.lds' + ' -lsupc++ -Wl,--start-group -lm -lc -lgcc -Wl,--end-group -static'
    CPATH   = ''
    LPATH   = ''
    if os.environ.get('SUN252I_BOARD') == 'usbdisp':
        # the trimmed USB second screen image is built for size
        CFLAGS += ' -Os'
    elif BUILD == 'debug':
        CFLAGS += ' -g -O0'
        AFLAGS += ' -g'
    else:
        CFLAGS += ' -O2'

DUMP_ACTION = OBJDUMP + ' -D -S $TARGET'
POST_ACTION = OBJCPY + ' -O binary $TARGET rtt.bin' + '\n' + SIZE + ' $TARGET \n'
