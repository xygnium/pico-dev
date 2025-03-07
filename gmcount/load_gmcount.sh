#~/bin/bash

PICO_DEV=/home/mike/dev/pico

#PRG=$PICO_DEV/pico-examples/build/blink/blink.elf
PRG=$PICO_DEV/gmcount/build/gmcount.elf

OPENOCD_D=$PICO_DEV/openocd
OPENOCD=$OPENOCD_D/src/openocd
IF_CFG=$OPENOCD_D/tcl/interface/cmsis-dap.cfg
TGT_CFG=$OPENOCD_D/tcl/target/rp2040.cfg

sudo $OPENOCD -s tcl -f $IF_CFG -f $TGT_CFG -c "adapter speed 5000" -c "program $PRG verify reset exit"
