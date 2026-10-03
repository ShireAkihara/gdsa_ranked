# GDSA Ranked (Geode 3.2.0)

Build (needs Geode SDK + CLI, and the GD toolchain per platform):

    export GEODE_SDK=/path/to/geode
    geode build            # Windows/Linux host -> build/gdsa.ranked.geode
    geode build -p android32|android64|ios   # cross targets (needs Android NDK / macOS)

Result: `build/gdsa.ranked.geode` — copy into `<GD>/geode/mods/`.
If the compiler complains about a Geode API name, check it against your SDK version
(web / matjson / Popup APIs shifted between 3.x and 4.x).
