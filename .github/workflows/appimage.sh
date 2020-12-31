#!/bin/bash -ex

BUILDBIN=/yuzu/build/bin
BINFILE=yuzu-x86_64.AppImage
LOG_FILE=$HOME/curl.log
BRANCH=`echo ${GITHUB_REF##*/}`

# QT 5.14.2
# source /opt/qt514/bin/qt514-env.sh
QT_BASE_DIR=/opt/qt514

mkdir -p /yuzu/build/bin/
curl -sL https://github.com/qurious-pixel/pineapple-src/releases/download/EA-1267/yuzu -o /yuzu/build/bin/yuzu
ls /yuzu/build/bin/

export QTDIR=$QT_BASE_DIR
export PATH=$QT_BASE_DIR/bin:$PATH
export LD_LIBRARY_PATH=$QT_BASE_DIR/lib/x86_64-linux-gnu:$QT_BASE_DIR/lib:$LD_LIBRARY_PATH
export PKG_CONFIG_PATH=$QT_BASE_DIR/lib/pkgconfig:$PKG_CONFIG_PATH

cd /tmp
	curl -sLO "https://github.com/probonopd/linuxdeployqt/releases/download/continuous/linuxdeployqt-continuous-x86_64.AppImage"
	curl -sLO "https://github.com/$GITHUB_REPOSITORY/raw/$BRANCH/.github/workflows/update.tar.gz"
	tar -xzf update.tar.gz
	chmod a+x linuxdeployqt*.AppImage
./linuxdeployqt-continuous-x86_64.AppImage --appimage-extract
cd $HOME
mkdir -p squashfs-root/usr/bin
cp -P "$BUILDBIN"/yuzu $HOME/squashfs-root/usr/bin/

curl -sL https://raw.githubusercontent.com/$GITHUB_REPOSITORY/$BRANCH/dist/yuzu.svg -o ./squashfs-root/yuzu.svg
curl -sL https://raw.githubusercontent.com/$GITHUB_REPOSITORY/$BRANCH/dist/yuzu.desktop -o ./squashfs-root/yuzu.desktop
curl -sL https://github.com/AppImage/AppImageKit/releases/download/continuous/runtime-x86_64 -o ./squashfs-root/runtime
mkdir -p squashfs-root/usr/share/applications && cp ./squashfs-root/yuzu.desktop ./squashfs-root/usr/share/applications
mkdir -p squashfs-root/usr/share/icons && cp ./squashfs-root/yuzu.svg ./squashfs-root/usr/share/icons
mkdir -p squashfs-root/usr/share/icons/hicolor/scalable/apps && cp ./squashfs-root/yuzu.svg ./squashfs-root/usr/share/icons/hicolor/scalable/apps
mkdir -p squashfs-root/usr/share/pixmaps && cp ./squashfs-root/yuzu.svg ./squashfs-root/usr/share/pixmaps
curl -sL "https://raw.githubusercontent.com/$GITHUB_REPOSITORY/$BRANCH/.github/workflows/update.sh" -o $HOME/squashfs-root/update.sh
chmod a+x ./squashfs-root/runtime
chmod a+x ./squashfs-root/update.sh
#cp /tmp/libssl.so.47 /tmp/libcrypto.so.45 /usr/lib/x86_64-linux-gnu/

echo $GITHUB_SHA > $HOME/squashfs-root/version.txt

unset QT_PLUGIN_PATH
unset LD_LIBRARY_PATH
unset QTDIR

mkdir $HOME/artifacts/
mkdir -p /yuzu/artifacts/version
# Version AppImage
curl -sL https://github.com/AppImage/AppImageKit/releases/download/continuous/AppRun-x86_64 -o $HOME/squashfs-root/AppRun
chmod a+x ./squashfs-root/AppRun
# /tmp/squashfs-root/AppRun $HOME/squashfs-root/usr/bin/yuzu -appimage -unsupported-allow-new-glibc -no-copy-copyright-files -no-translations -bundle-non-qt-libs
/tmp/squashfs-root/AppRun $HOME/squashfs-root/usr/bin/yuzu -unsupported-allow-new-glibc -no-copy-copyright-files -no-translations -bundle-non-qt-libs
export PATH=$(readlink -f /tmp/squashfs-root/usr/bin/):$PATH
mkdir $HOME/squashfs-root/usr/plugins/platformthemes/
cp /opt/qt514/plugins/platformthemes/libqgtk3.so $HOME/squashfs-root/usr/plugins/platformthemes/
mkdir $HOME/squashfs-root/dist/
cp /yuzu/dist/yuzu.ico $HOME/squashfs-root/dist/
/tmp/squashfs-root/usr/bin/appimagetool $HOME/squashfs-root
version=$(cat /yuzu/README.md | grep -o 'early-access [[:digit:]]*' | cut -c 14-17) 
mv ./yuzu-x86_64.AppImage /yuzu/artifacts/version/Yuzu-EA-$version.AppImage

# Continuous AppImage
rm $HOME/squashfs-root/AppRun
curl -sL "https://raw.githubusercontent.com/$GITHUB_REPOSITORY/$BRANCH/.github/workflows/AppRun" -o $HOME/squashfs-root/AppRun
chmod a+x ./squashfs-root/AppRun
mv /tmp/update/AppImageUpdate $HOME/squashfs-root/usr/bin/
mv /tmp/update/* $HOME/squashfs-root/usr/lib/
/tmp/squashfs-root/usr/bin/appimagetool $HOME/squashfs-root -u "gh-releases-zsync|pineappleEA|pineapple-src|continuous|yuzu-x86_64.AppImage.zsync"

mv yuzu-x86_64.AppImage* /yuzu/artifacts

cp -R $HOME/artifacts/ /yuzu/
cp "$BUILDBIN"/yuzu /yuzu/artifacts/version/
chmod -R 777 /yuzu/artifacts
cd /yuzu/artifacts
ls -al /yuzu/artifacts/
ls -al /yuzu/artifacts/version
