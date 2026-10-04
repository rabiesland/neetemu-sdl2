# NeetEmulator

Runs NeetComputers Lua machines on desktop.

## Build

Requires: CMake, C compiler, pkg-config, SDL3, Neet-YSLua.

```sh
cmake -S . -B build -G Ninja
cmake --build build
sudo cmake --install build
```

YSLua path: `-DYSLUA_DIR=/path/to/Neet-YSLua` or `YSLUA_DIR=... cmake ...`.

## Usage

```sh
neetemu                   # boot NeetOS, computer 0
neetemu -c 3              # boot NeetOS, computer 3
neetemu --list            # list computers
neetemu -c 3 -- prog.lua args   # run program with args
neetemu --help            # all options
```

Storage: `~/.local/share/neetemu/computers/<id>/` (`$XDG_DATA_HOME` if set). Created from bundled image on first boot. `bios:/path` maps to `<disk>/bios/path`.

## Custom boot path (build.json)

Like NeetComputers, each computer folder may contain a `build.json`:

```json
{
    "entrypoint" : "mypart:boot.lua",
    "language" : "Lua",
    "partitions" : [
        { "path" : "bios", "readonly" : false, "hidden" : false },
        { "path" : "mypart", "readonly" : false, "hidden" : false }
    ]
}
```

## Keys

F1 help. F9 snapshot BMP to `snapshots/`. F10 reboot. F11 fullscreen. Ctrl+1/Ctrl+2 zoom. Ctrl+L stats overlay. Key repeat ignored. Last frame kept on exit/crash.
