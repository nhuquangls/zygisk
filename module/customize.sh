#!/system/bin/sh

ui_print "- Runtime Shim - native compatibility module"
ui_print "- Runtime helper for the tested 2944x1840 HUD"
ui_print "- Sniper gyro aim with adaptive 5000/500/8 ms polling"
ui_print "- Target: com.vnggames.cfl.crossfirelegends"
ui_print "- ABI: arm64-v8a"

if [ "$ARCH" != "arm64" ]; then
  abort "! This module supports arm64-v8a only"
fi
if [ "$API" -lt 26 ]; then
  abort "! Android 8.0/API 26 or newer is required for the input bridge"
fi

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/zygisk/arm64-v8a.so" 0 0 0755
set_perm "$MODPATH/payload/libgcloudsync.so" 0 0 0755
