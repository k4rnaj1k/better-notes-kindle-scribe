#!/bin/sh
rm -f /tmp/betternotes-active
pkill -f betternotes 2>/dev/null
lipc-set-prop com.lab126.pillow disableEnablePillow enable 2>/dev/null
lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null
lipc-send-event com.lab126.hal.usbError cycled 2>/dev/null
echo "BetterNotes stopped"
