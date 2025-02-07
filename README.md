# pico-dev

A place for notes on learning Raspberry Pi pico project development.

A C development environment is described because of the author's familiarity 
with the language, and the opinion
that it forces one to have a deeper understanding of the system.

# Equipment

- Dell desktop (purchased use from a vendor on Amazon)
- several Pico 1 MCU (pico and pico W)
- USB to serial converters
- long USB cables`
- breadboards
- jumper wires, a.k.a. [Dupont wires](https://www.reddit.com/r/electronics/comments/ioc6sf/i_finally_foundout_why_dupont_connectors_are/?rdt=36730)

# References

1. Getting started with Raspberry Pi Pico-series, build-date: 2024-10-15, build-version: 2b6018e-clean
2. Programming The Raspberry Pi Pico in C, First Printing, April 2021, Revison 0, ISBN: 9781871962680
3. [RPi microcontroller documentation](https://www.raspberrypi.com/documentation/microcontrollers/)

# Notes

## Install Tools

1. sudo apt update
1. sudo apt list --upgradable
1. sudo apt upgrade

1. sudo apt install automake autoconf build-essential texinfo libtool \
   libftdi-dev libusb-1.0-0-dev pkg-config libhidapi-dev git minicom

1. ls -l /dev/ | grep USB
1. sudo apt remove brltty 
1. ls -l /dev/ | grep USB

## Build/Load Debugprobe on Pico

1. git clone https://github.com/raspberrypi/debugprobe.git
1. ls
1. cd debugprobe/
1. git submodule update --init
1. mkdir build
1. cd build/
1. ls
1. ls ../../
1. export PICO_SDK_PATH=../../pico-sdk
1. cmake -DDEBUG_ON_PICO=ON -DPICO_BOARD=pico ..
1. ls
1. make
1. ls
1. ls /
1. ls /media/
1. ls /media/mike/
1. ls /media/mike/RPI-RP2/
1. cp -v debugprobe_on_pico.uf2 /media/mike/RPI-RP2/
1. ls /media/mike/RPI-RP2/
1. ls /media/mike/

1. download prog to debugger pico 
 wire debugger pico to pico

## Build/Run Openocd

- Compile for Linux

- https://forums.raspberrypi.com/viewtopic.php?t=347489
- https://github.com/libusb/hidapi?tab=readme-ov-file#installing-hidapi

1. git clone https://github.com/raspberrypi/openocd.git
1. ls
1. cd openocd/
1. ./bootstrap
1. ./configure --enable-cmsis-dap
1. make

	modify /home/mike/dev/pico/openocd/tcl/target/rp2040.cfg:
	source [find /home/mike/dev/pico/openocd/tcl/target/swj-dp.tcl]
	#source [find <absolute path>/target/swj-dp.tcl] # my comment
	#source [find target/swj-dp.tcl] # original line only works from openocd build dir

## Use Openocd to flash pico

- Run VB Mate/Ubuntu guest vm

- VB vm menu->Devices->USB-><select Raspberry Pi Debugprobe on Pico (CMSIS-DAP)[201]>

Command:

mike@xygdev3:~/dev/pico/openocd$ sudo src/openocd -s tcl -f tcl/interface/cmsis-dap.cfg -f tcl/target/rp2040.cfg -c "adapter speed 5000" -c "program ../pico-examples/build/blink/blink.elf verify reset exit"

Scripted:

	mike@xygdev3:~/dev$ cat /home/mike/dev/pico/openocd/loadprog.sh
	#~/bin/bash
	
	PICO_DEV=/home/mike/dev/pico
	
	PRG=$PICO_DEV/pico-examples/build/blink/blink.elf
	
	OPENOCD_D=$PICO_DEV/openocd
	OPENOCD=$OPENOCD_D/src/openocd
	IF_CFG=$OPENOCD_D/tcl/interface/cmsis-dap.cfg
	TGT_CFG=$OPENOCD_D/tcl/target/rp2040.cfg
	
	sudo $OPENOCD -s tcl -f $IF_CFG -f $TGT_CFG -c "adapter speed 5000" -c "program $PRG verify reset exit"
`
