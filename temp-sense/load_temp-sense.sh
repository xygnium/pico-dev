#~/bin/bash

PICO_DEV=/home/mike/dev/pico

PRG=$HOME/dev/github/pico-dev/temp-sense/build/temp-sense.elf

OPENOCD_D=$PICO_DEV/openocd
OPENOCD=$OPENOCD_D/src/openocd
IF_CFG=$OPENOCD_D/tcl/interface/cmsis-dap.cfg
TGT_CFG=$OPENOCD_D/tcl/target/rp2040.cfg

# No sudo: mike is in plugdev, and the debugprobe's /dev/bus/usb node is
# root:plugdev, so openocd can claim it directly.
$OPENOCD -s tcl -f $IF_CFG -f $TGT_CFG -c "adapter speed 5000" -c "program $PRG verify reset exit"
