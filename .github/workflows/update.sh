#!/bin/bash

export QT_QPA_PLATFORMTHEME=gtk3

zenity --question --timeout=10 --title="yuzu updater" --text="New update available. Update now?" --icon-name=yuzu --window-icon=yuzu.svg --height=80 --width=400
answer=$?

echo "GAMEPATH '${GAMEPATH}'"

if [ "$answer" -eq 0 ]; then 
	$APPDIR/usr/bin/AppImageUpdate $PWD/yuzu-x86_64.AppImage && $PWD/yuzu-x86_64.AppImage "${GAMEPATH}"
elif [ "$answer" -eq 1 ]; then
	$APPDIR/usr/bin/yuzu "${GAMEPATH}"
elif [ "$answer" -eq 5 ]; then
	$APPDIR/usr/bin/yuzu "${GAMEPATH}"
fi
exit 0
