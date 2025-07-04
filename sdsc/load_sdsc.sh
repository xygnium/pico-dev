#~/bin/bash

#PRG=$PICO_DEV/pico-examples/build/blink/blink.elf
#PRG=$PICO_DEV/gmcount/build/gmcount.elf
#PRG=$HOME/dev/github/pico-dev/gmcount/build/gmcount.elf
PRG=$HOME/dev/github/pico-dev/sdsc/build/simple_example.elf

PICO_DEV=/home/mike/dev/pico
OPENOCD_D=$PICO_DEV/openocd
OPENOCD=$OPENOCD_D/src/openocd
IF_CFG=$OPENOCD_D/tcl/interface/cmsis-dap.cfg
TGT_CFG=$OPENOCD_D/tcl/target/rp2040.cfg

sudo $OPENOCD -s tcl -f $IF_CFG -f $TGT_CFG -c "adapter speed 5000" -c "program $PRG verify reset exit"
