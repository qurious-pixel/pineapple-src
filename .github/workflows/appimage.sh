#!/bin/bash -ex

BUILDBIN=/yuzu/build/bin
BINFILE=yuzu-x86_64.AppImage
LOG_FILE=$HOME/curl.log
BRANCH=`echo ${GITHUB_REF##*/}`
export CC=${GCC_BINARY}
export CXX=${GXX_BINARY}

# QT 5.14.2
# source /opt/qt514/bin/qt514-env.sh
#QT_BASE_DIR=/opt/qt514
#export QTDIR=$QT_BASE_DIR
#export PATH=$QT_BASE_DIR/bin:$PATH
#export LD_LIBRARY_PATH=$QT_BASE_DIR/lib/x86_64-linux-gnu:$QT_BASE_DIR/lib:$LD_LIBRARY_PATH
#export PKG_CONFIG_PATH=$QT_BASE_DIR/lib/pkgconfig:$PKG_CONFIG_PATH

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
	sed -i -e 's#Exec=yuzu %f#Exec=env QT_QPA_PLATFORMTHEME=gtk3 yuzu %f#g' ./squashfs-root/yuzu.desktop
curl -sL https://github.com/AppImage/AppImageKit/releases/download/continuous/runtime-x86_64 -o ./squashfs-root/runtime
mkdir -p squashfs-root/usr/share/applications && cp ./squashfs-root/yuzu.desktop ./squashfs-root/usr/share/applications
mkdir -p squashfs-root/usr/share/icons && cp ./squashfs-root/yuzu.svg ./squashfs-root/usr/share/icons
mkdir -p squashfs-root/usr/share/icons/hicolor/scalable/apps && cp ./squashfs-root/yuzu.svg ./squashfs-root/usr/share/icons/hicolor/scalable/apps
mkdir -p squashfs-root/usr/share/pixmaps && cp ./squashfs-root/yuzu.svg ./squashfs-root/usr/share/pixmaps
curl -sL "https://raw.githubusercontent.com/$GITHUB_REPOSITORY/$BRANCH/.github/workflows/update.sh" -o $HOME/squashfs-root/update.sh
chmod a+x ./squashfs-root/runtime
chmod a+x ./squashfs-root/update.sh
#cp /tmp/libssl.so.47 /tmp/libcrypto.so.45 /usr/lib/x86_64-linux-gnu/

version=$(cat /yuzu/README.md | grep -o 'early-access [[:digit:]]*' | cut -c 14-17) 
echo EA-$version > $HOME/squashfs-root/version.txt

unset QT_PLUGIN_PATH
unset LD_LIBRARY_PATH
unset QTDIR

mkdir $HOME/artifacts/
mkdir -p /yuzu/artifacts/version
# Version AppImage
    mkdir -p squashfs-root/usr/optional/{libstdc++,libgcc_s}
    cp /usr/lib/x86_64-linux-gnu/libstdc++.so.6 ./squashfs-root/usr/optional/libstdc++/
    cp /lib/libgcc_s.so.1 ./squashfs-root/usr/optional/libgcc_s/
    curl -sSfL https://github.com/RPCS3/AppImageKit-checkrt/releases/download/continuous2/AppRun-patched-x86_64 -o ./squashfs-root/AppRun
    chmod a+x ./squashfs-root/AppRun
    curl -sSfL https://github.com/RPCS3/AppImageKit-checkrt/releases/download/continuous2/exec-x86_64.so -o ./squashfs-root/usr/optional/exec.so
    printf "#include <sstream>\n#include <exception>\n#include <memory_resource>\nint main(){auto x = std::stringbuf();x.get_allocator();std::make_exception_ptr(0);std::pmr::get_default_resource();}" \
    | $CXX -x c++ -std=c++2a -o ./squashfs-root/usr/optional/checker -
# /tmp/squashfs-root/AppRun $HOME/squashfs-root/usr/bin/yuzu -appimage -unsupported-allow-new-glibc -no-copy-copyright-files -no-translations -bundle-non-qt-libs
/tmp/squashfs-root/AppRun $HOME/squashfs-root/usr/bin/yuzu -unsupported-allow-new-glibc -no-copy-copyright-files -no-translations -bundle-non-qt-libs
export PATH=$(readlink -f /tmp/squashfs-root/usr/bin):$PATH
export LD_LIBRARY_PATH="$(readlink -f ./squashfs-root/usr/lib):$(readlink -f ./squashfs-root/usr/optional):${LD_LIBRARY_PATH}"
	mkdir $HOME/squashfs-root/usr/plugins/platformthemes/
	cp /opt/qt515/plugins/platformthemes/libqgtk3.so $HOME/squashfs-root/usr/plugins/platformthemes/
/tmp/squashfs-root/usr/bin/appimagetool $HOME/squashfs-root
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
#cp "$BUILDBIN"/yuzu /yuzu/artifacts/version/
chmod -R 777 /yuzu/artifacts
cd /yuzu/artifacts
ls -al /yuzu/artifacts/
ls -al /yuzu/artifacts/version
