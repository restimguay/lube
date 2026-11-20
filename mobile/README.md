# Lube on phones

Template apps that run a Lube program full screen, with touches (and game controllers) as its input:

- `game/main.lube`: the program both run (a sample: tap to add a ball, with a sound from `game/sounds/`). Put yours here, with the files it reads beside it, in folders or not. It runs on a computer too: `lube mobile/game/main.lube`.
- `android/`: a NativeActivity (no Java). `gradle -p mobile/android assembleDebug` (or `installDebug` with a phone plugged in; a program elsewhere: `-PlubeGame=/path/to/it`), or open the folder in Android Studio. Needs the NDK. What the program prints: `adb logcat -s lube`.
- `ios/`: `cmake -G Xcode -S mobile/ios -B build/ios -DCMAKE_SYSTEM_NAME=iOS` (a program elsewhere: `-DLUBE_GAME=/path/to/it`), then open `build/ios/LubeApp.xcodeproj` and run it (choose a signing team for a phone).

Both build Lube from `../src` (elsewhere: `-DLUBE_SRC=/path/to/lube/src`), and show the program's window with `src/lube_gfx.h`. More in the main README, under "Phones: Android and iOS".
