echo "Compilng NeetEmu"
./compile.sh

echo "Creating application bundle"
cd build
mkdir NeetEmu.app
mkdir NeetEmu.app/Contents
mkdir NeetEmu.app/Contents/Frameworks
mkdir NeetEmu.app/Contents/Resources
mkdir NeetEmu.app/Contents/MacOS

cp neetemu neetemu-bin
SDLPATH=$(otool -L neetemu-bin | grep "libSDL2" | awk '{print $1}')
echo "SDL2 is installed at $SDLPATH"

echo "Changing library paths of copy"
install_name_tool -change "$SDLPATH" @executable_path/../Frameworks/libSDL2.dylib neetemu-bin
install_name_tool -change @rpath/yslua.dylib @executable_path/../Frameworks/yslua.dylib neetemu-bin

echo "Adding binaries to application bundle"
mv ./neetemu-bin NeetEmu.app/Contents/MacOS/neetemu
cp ./yslua.dylib NeetEmu.app/Contents/Frameworks/
cp "$SDLPATH" NeetEmu.app/Contents/Frameworks/libSDL2.dylib

echo "Downloading emulator assets"
cd NeetEmu.app/Contents/Resources
cp ../../../../neetemu.icns neetemu.icns
cd ..
CURPATH=$(pwd)
cd /tmp
git clone https://github.com/redtoast/NeetComputers.git
cd NeetComputers/src/main/resources/data/neetcomputers/neet/hard_addresses
mkdir "$CURPATH/Resources/disk/"
cp -r ./1 "$CURPATH/Resources/disk/bios"
cd /tmp
rm -rf NeetComputers
cd "$CURPATH"

infoPlist="<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>en-US</string>
	<key>CFBundleExecutable</key>
	<string>neetemu</string>
	<key>CFBundleIdentifier</key>
	<string>net.rabiesland.neetemu</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>NeetEmu</string>
	<key>CFBundlePackageType</key>
	<string>BNDL</string>
	<key>CFBundleShortVersionString</key>
	<string>$1</string>
	<key>CFBundleVersion</key>
	<string>$2</string>
	<key>CFBundleIconFile</key>
	<string>neetemu</string>
</dict>
</plist>"

echo "$infoPlist" >> Info.plist
echo "Done!"
cd ../..
chmod a+xr NeetEmu.app