# MC/PS MemCard Pro for Android

The same app as the Windows build (`app/main.cpp`, SDL2 + Dear ImGui) running on SDL's Android
activity. `MCPSActivity` adds the Android parts: the system file picker for a BIOS, cards and saves,
the "save to" dialog for exports, and opening files from other apps.

Put away, the PocketStation keeps running (a foreground service with a quiet "PocketStation is
running" notification keeps Android from freezing it), and games calling come up as notifications:
see Alerts in `app/README.md`. Time the phone spends asleep is run through quickly when it wakes,
so events in it still arrive.

Your own PocketStation BIOS is needed, as on Windows. Pick it with **Choose BIOS…** on the
Cards page, or copy it to the app's folder (`Android/data/com.f3bandit.mcps/files/`) by USB.
The card, settings and the saved unit live in that folder too.

## Build

Needs the Android SDK (platform 34, build-tools 34, CMake 3.22.1, NDK 26.3.11579264) and SDL2's
source:

    git clone -b release-2.30.9 --depth 1 https://github.com/libsdl-org/SDL.git ../../SDL2
    echo "sdk.dir=/path/to/android/sdk" > local.properties
    ./gradlew assembleRelease            # or -PSDL2_SRC=/path/to/SDL2

The APK lands in `app/build/outputs/apk/release/`. `org/libsdl/app/` is SDL's own Java code
(zlib licence), copied from SDL 2.30.9's `android-project`.
