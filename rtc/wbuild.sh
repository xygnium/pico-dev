#!/bin/bash

export PICO_SDK_PATH=$HOME/dev/pico/pico-sdk
cmake -DPICO_BOARD=pico_w ..
make
