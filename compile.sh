cd ../Neet-YSLua
git pull
cd lua
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd ../../neetemu-sdl2
cmake -S . -B build -G Ninja
cmake --build build