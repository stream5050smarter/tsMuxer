export PATH=/usr/lib/mxe/usr/bin:$PATH
rm -rf build
mkdir build
mkdir ./bin/w32
cd build
export CCACHE_DISABLE=1
export MXE_USE_CCACHE=

i686-w64-mingw32.static-cmake ../
make
mv tsMuxer/tsmuxer.exe ../bin/w32/tsMuxeR.exe

i686-w64-mingw32.static-qmake-qt5 ../tsMuxerGUI
make
mv ./tsMuxerGUI.exe ../bin/w32/tsMuxerGUI.exe
cd ..
rm -rf build
zip -jr ./bin/w32.zip ./bin/w32
ls ./bin/w32/tsMuxeR.exe && ls ./bin/w32/tsMuxerGUI.exe && ls ./bin/w32.zip
