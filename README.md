## Disclaimer
The majority of changes are entirely vibecoded. There will be bugs and the vast majority of them I do not understand. All of the commits I make that include "vibe" in its name are vibecoded commits.

## Comfy... Studio?
Comfy Studio is a *totally epic ⚡⚡* Chart Editor for creating custom Project DIVA Arcade-Style charts for use in modding.  *Holy smokes*, would you believe it!

*"Wow this totally sucks ass!"*, - Someone, probably.

![editor_example_chart](ComfyStudio/manual/image/other/editor_example_chart.png)

### Preamble
Dear Diary, Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est laborum.


So with that important message aside, here's some shitty code I worked on from 2019-2021 all under the "Comfy" umbrella. 
A name representing the deep, warm, cozy feeling of peace and comfort that only a loving passion project like this could provide.
... Or rather just some random name because I guess I kinda liked using that funny frog emote with his comfy blanket on a chair or something? Naming do be hard.
Time sure flies, when you're having fun (ᴵ ʰᵃᵗᵉ ᵐʸ ˡⁱᶠᵉ) ... --- ... h̶̶̵͘͞e̵̢ļ̷́p̴̷̡ ̶̡͘͝Mɏ ȼᵾɍɍɇNŦ ŁØȼȺŦɨønɨS...


#### :warning: **Disclaimer** :warning: 
I didn't check if the repository is even set up correctly and everything is included and compiles as it should. 
The code is provided as is and is licensed under GPL-3 License so basically do whatever the fuck you want as long as you keep it open source type shit. #YOLO #SWAG #WHENWILLTHEPAINFINALLYEND


## So I guess that's where we're at
I'm generally very unhappy with the overall structure looking back at it but hey at least it was a valuable learning exercise.
Lots of small deeply nested yet shallow OOP/C# style files, projects and directories garbo scattered across with very little actual meat inside that makes me wanna puke nowadays but somehow has enterprise gooners dripping wet - Would not recommend!

I didn't initially plan on making this public at all (...at least anything more that the chart editor) but I also didn't want to spend more time and effort ripping it apart while also losing the git history and OCD baiting myself into rewriting half of in the process just to bring it up to my current "standards" until I'm satisfied with it.
Letting it sit here to just rot away also feels kinda shitty for anybody who might want to keep hacking on it, so myeah fuck it.

A lot of the code was me playing around, just having fun with different ideas and I guess also some kind of chart editor..? for some fucked up weeb game with dancing dolls 'n shit?? I dunno man, feels like a fever dream tbh.

# AI Generated Docs Start

## Building the native Linux chart editor

The Linux build produces a native 64-bit `ComfyStudio` executable that uses SDL's native Wayland backend by default. Run it from a Wayland desktop session; it does not automatically fall back to XWayland. An explicit `SDL_VIDEODRIVER` override is honored, including `offscreen` for automated checks and `x11` for compatibility.

The following dependencies are needed:

```sh
clang python ninja pkgconf sdl2-compat wayland libxkbcommon libdecor glew mesa openssl zlib libvorbis ffmpeg zenity
```

From the source directory, build and run:

```sh
python tools/build-linux.py
./build/linux/ComfyStudio
```

The Python builder generates a Ninja build with compiler dependency tracking. It builds the shared file-format library, editor, and native `ComfyDataBuild` resource packer, packs `ComfyData.dat`, and copies the user manual beside the executable. Paths containing spaces are supported. No Git commands, Windows SDK, Wine, or downloaded build-time dependencies are used by the Linux builder. Build metadata uses the repository's fallback version information.

Place the extracted game assets in `build/linux/dev_rom/`, retaining the original `2d/`, `sound/`, and other subdirectories. These assets are still required for game graphics, fonts, and sound effects. Keep `ComfyData.dat`, `manual/`, and `dev_rom/` beside the executable when moving the build. Settings are stored in that directory, which must be writable. Linux filenames are case-sensitive.

For a separate debug build:

```sh
python tools/build-linux.py --debug --build-dir build/linux-debug
```

Use `--jobs 4` to change build parallelism. `--target ComfyDataBuild` and `--target libComfyLib.a` build those targets individually. The default compiler is Clang; `CXX` can select another compiler, though Clang is the tested compiler.

Run the Linux checks with:

```sh
python tools/build-linux.py --test
```

To also verify native Wayland startup, rendering, and shutdown against your running compositor:

```sh
python tools/build-linux.py --test-wayland
```

This briefly opens the editor and asserts that SDL selected `wayland`. It uses dummy audio and saves `linux-editor-smoke.png`. Combine it with `--test` to run both suites. SDL must have Wayland support and the compositor must provide desktop OpenGL through EGL. Wayland controls window placement, so saved absolute window positions cannot be restored; window size, maximization, and fullscreen requests still use SDL.

These checks exercise Unicode file I/O, archive AES decryption, DDS texture decoding, chart save/load roundtrips, OpenGL rendering, and normal editor startup and shutdown. When `dev_rom/` is present in the build directory, they also decode and upload the supplied game textures, load the font map, decode the sound banks, and open an SDL audio stream. A rendered HUD/font preview is saved as `linux-chart-preview.png`. When the `ffmpeg` command is installed, a generated 1080p video fixture also checks rapid seeking, playback immediately after a cursor seek, end-of-file handling, reopening, and GPU texture reuse. Audio checks verify callback buffer sizes and device shutdown; navigation-key checks cover arrows, Home/End, Page Up/Down, and both function-key ranges. The startup test saves `linux-editor-smoke.png` in the build directory. Rendering and startup tests use SDL's offscreen video driver and dummy audio driver. They require an OpenGL-capable offscreen driver, such as Mesa, and do not verify physical audio devices or controllers.

The initial Linux port focuses on the chart editor. The unfinished Aet and 3D/PV editors are excluded. Docking works inside the main window; detached OS-level ImGui windows are not implemented. SDL provides shared audio device access with a minimum 1024-frame buffer (about 23 ms at 44.1 kHz) to allow for desktop scheduling jitter; smaller saved buffer requests are raised to this minimum. Buffer requests are rounded up to a power of two. WASAPI exclusive mode is available only on Windows. Video previews use the separate chart song for audio, as the Windows player does. Linux video file opening runs synchronously; decoding and seeking run on a worker thread. Rapid timeline seeks replace pending requests, and the preview keeps its previous frame until the new one is ready. Ordinary playback updates do not cancel a pending decode. MSAA and some less common Aet blend modes do not yet match the Windows renderer. Offline YACbCr texture export is unavailable; existing YACbCr game textures can be decoded. Optional Discord rich presence needs a separately supplied `libdiscord_game_sdk.so`.

## Linux rendering performance

The Linux UI uploads ImGui vertex and index lists to GPU buffers and issues indexed draws, preserving clipping and vertex offsets. The chart renderer batches consecutive compatible sprites and primitives into streamed vertex buffers. Texture, sampler filter/addressing, blend mode, and topology changes end a batch. Masked sprites, checkerboards, and post-processing also flush pending geometry to preserve draw order. Strips and fans remain separate draws. This removes per-vertex immediate-mode calls from the ordinary UI/chart paths and reduces repeated sprite state changes. Buffer storage is replaced for each upload so the driver can retain storage still referenced by queued draws, following [Khronos buffer streaming guidance](https://wikis.khronos.org/opengl/Buffer_Object_Streaming).

Startup prints the OpenGL vendor, renderer, and version to stderr. Check this when comparing Linux with Wine: `llvmpipe` or `softpipe` means CPU software rendering, while a hardware renderer should identify the graphics device. The application uses the GPU selected by SDL and the graphics driver; it does not override desktop GPU selection. Lower GPU utilization alone does not establish a performance problem: compare frame times with the same chart, preview resolution, video, and swap interval. For an uncapped comparison, select **Swap Interval 0** from the application's swap interval menu in both versions. VSync defaults to interval 1; the Linux loop also delays 5 ms when unfocused or when main-loop power sleep is requested.

Build the tests with `python tools/build-linux.py --test`, then run the isolated sprite submission benchmark from the build directory:

```sh
cd build/linux
./LinuxTests --render-benchmark
```

The benchmark compares the previous immediate-mode vertex submission with the batched path for 2,000 six-vertex sprites over 20 frames, after warmup. It waits for rendering completion and verifies identical pixels. This is a synthetic submission benchmark, not a whole-editor or Windows comparison. For automated software-renderer testing, prefix it with `SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy`. The normal frame loop never calls the benchmark's `glFinish` synchronization.

Other candidates for profiling remain:

- **Video:** FFmpeg decodes on a worker using a CPU decoder, converts displayed frames to RGBA with `sws_scale`, allocates a frame-sized buffer, and uploads the pixels. GPU texture reuse already avoids repeated texture allocation, but hardware decoding or GPU YUV conversion would require a separate implementation.
- **Large charts:** `TargetTimeline::DrawTimelineTargets` walks targets from the beginning before skipping those left of the visible region. Button-sound playback also scans the target list each update. A time-range lookup could reduce this work for long charts; these shared paths also run on Windows.
- **Text borders:** Each bordered glyph still generates eight offset copies plus its original. Batching reduces submission cost, but a shader-based outline could reduce geometry and overdraw.
- **Special effects:** Masked sprites and checkerboards retain individual immediate-mode shader draws, and shader uniform locations are looked up repeatedly. These paths are excluded from the ordinary sprite batching optimization.

The rendering tests cover batch capacity rollover, overlapping blend changes, sampler filter changes on a shared texture, masked draw order, atlas orientation, and line-grid topology. Physical GPU utilization and compositor pacing must be measured on a desktop with GPU access.

## Building with Visual Studio 2026

On Windows, install the latest stable Visual Studio 2026 with the **Desktop development with C++** workload, the latest **MSVC v145 C++ x64/x86 build tools**, and a Windows SDK.

Open `Comfy.sln`, select `Debug` or `Release` with the `x64` platform, and build the solution. Alternatively, run this from a Visual Studio Developer Command Prompt in the source directory:

```bat
msbuild Comfy.sln /m /p:Configuration=Release /p:Platform=x64
```

All projects, including the bundled dependencies, use the v145 toolset. The Windows SDK version is set to `10.0`, which selects the latest installed SDK instead of requiring the old 10.0.17763.0 SDK. MSBuild uses the current installed version, and the solution requires Visual Studio 2026 or newer.

The existing build scripts call the Release versions of `ComfyVersion.exe` and `ComfyDataBuild.exe`, so build Release first before building Debug. The version generator uses Git during a normal Windows build. Runtime game files are still required as described below.

# AI Generated Docs End

## The Tower of Babel
Schizo rambling over with, the overall project structure I *think* was about the following:

- `ComfyLib`:
General common code and file format handling stuff (much wow)

- `ComfyEngine`:
Built on top of ComfyLib providing platform specific graphics, audio, input, window handling and so on using those file formats (very pog)

- `ComfyStudio`:
Although I *have* previously dabbled with more authoring tools like a **completly unfinished** Aet (2D animation) editor as well as some other 3D stuff *(before getting utterly cucked by After Effects)*
the important part here is of course the *mostly complete* **chart editor** which **"Comfy Studio"** is known for! Built on top of ComfyLib + ComfyEngine

- `ComfyData`:
Build-step program and data for packing static resources into a single data file which is then loaded by ComfyStudio at startup (great example of stuff I didn't really need but my heart desired!)

- `ComfyVersion`:
Build-step program for automatically generating version info source code via git to be included by ComfyStudio (K.I.S.S. I guess)

- `ComfySandbox`:
Unethical Unit 731 ComfyEngine experiments (completly useless garbage, do not unshackle the demons *or thou shall witness the wrath*)

*(Oh and to run it it also requires a couple game files extracted and placed in a `dev_rom/` directory next to `ComfyStudio.exe` which are not included in this repository for obvious reasons. Just yoink 'em from an already compiled build I guess)*


## I Guess That's it
[So thats how you wanna be? Ok, ... hey guys..... I guess thats it, 🤯🔫 KAPOW 📱 🎶](https://www.youtube.com/watch?v=UMLCSbDLkTU)
