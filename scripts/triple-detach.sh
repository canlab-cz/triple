#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
# Detach every adapter and unload the module.
pkill -f "ldattach -?d? ?29" 2>/dev/null || pkill -f "ldattach 29" 2>/dev/null
sleep 0.5
rmmod usb2cansocketcan 2>/dev/null
echo "detached"
