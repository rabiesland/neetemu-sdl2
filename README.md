# neetemu-sdl2

*neetemu-sdl2* is a fork of [neetemu](https://codeberg.org/SpartanSoftware/neetemu] by SpSf that lets it run on older OS X versions.

It's a soft fork so doesn't do any big divergent changes and should be close to upstream feature-wise.

## Building

Requires: Xcode, [MacPorts](https://macports.org], [ninja](https://ports.macports.org/port/ninja/), [pkgconfig](https://ports.macports.org/port/pkgconfig/), SDL2, [Neet-YSLua](https://codeberg.org/SpartanSoftware/Neet-YSLua).

Clone neetemu-sdl2 and Neet-YSLua into the same directory, cd into neetemu-sdl2, and run `./compile.sh`. Create an application bundle with `bundle.sh {VERSION} {VERSION}`