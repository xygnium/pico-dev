#!/bin/bash

export PICO_SDK_PATH=$HOME/dev/pico/pico-sdk
#cmake -DPICO_BOARD=pico_w ..
#cmake -DPICO_BOARD=pico2 ..
cmake -DPICO_PLATFORM=rp2350-arm-s ..
#cmake -DPICO_PLATFORM=rp2040 ..
#cmake ..
make
