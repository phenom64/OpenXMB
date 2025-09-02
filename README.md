# OpenXMB

<p align="left">
  An open-source, cross-platform reimagining of Sony's XrossMediaBar, built with modern C++23, Vulkan, and SDL2 for high performance and portable efficiency.
</p>
<a href="https://syndromatic.com"><img src="interfaceFX/GraphicsServer/SyndromaticOrb.png" alt="Syndromatic Logo" width="100"/></a>

## About The Project

This project is a modern interpretation of the iconic XrossMediaBar (XMB) interface, famously used on the PlayStation 3 and other Sony devices. It is not a direct port, but a new implementation that leverages modern graphics APIs and C++ features to create a flexible and performant user interface for desktops and embedded systems.

The rendering backend is powered by [**AuroreEngine**](https://github.com/phenom64/AuroreEngine), a custom rendering library based on `dreamrender`. The entire application is built using C++23 modules for a clean and modern architecture.

## Screenshots

<p align="center">
  <img src="interfaceFX/GraphicsServer/Screenshot 2025-08-29 190126.png" alt="OpenXMB main menu and settings" width="300"/>
  <img src="interfaceFX/GraphicsServer/Screenshot 2025-08-30 201458.png" alt="Dynamic XMB colour schemes" width="300"/>
</p>

<p align="center">
  <em>Left: OpenXMB main menu & settings (pre-alpha) · Right: Dynamic XMB colour schemes adapting to time/month</em>
</p>

## Features

*   **Modern Tech Stack:** Built with C++23 modules, CMake, Vulkan for rendering (with MoltenVK on macOS), and SDL2 for windowing and input.
*   **Cross-Platform:** Designed to run on SynOS (and other Ubuntu-based distros), macOS, and Windows.
*   **Dynamic XMB Interface:** A fully navigable, PS3-style cross-media bar with smooth animations and a dynamic wave background.
*   **Extensible Menus:** Includes menus for Settings, Photos, Music, Videos, Games, and Applications. The application menu automatically scans for `.desktop` files.
*   **Built-in Media Viewers:**
    *   Image viewer for common formats.
    *   Text viewer.
    *   FFmpeg-based video player with GPU-accelerated YUV decoding.
*   **Highly Configurable:** Runtime behavior, colors, fonts, and more can be configured via a simple `config.json` file.
*   **Optional Modules:** The build system allows enabling or disabling major features like the video player, a planned CEF-based web browser, and libretro core support.

## Getting Started

### Prerequisites

Before you begin, ensure you have the following tools installed:
*   Git
*   CMake (version 3.22 or newer)
*   A C++23 compatible compiler (Clang 16+, GCC 13+, MSVC 19.34+)
*   Ninja build system (recommended)

### Build Instructions

#### macOS

1.  **Install Dependencies (via Homebrew):**
    ```bash
    brew install cmake ninja pkg-config ffmpeg sdl2 sdl2_image sdl2_mixer gettext fmt freetype glm
    # Install the Vulkan SDK, which includes MoltenVK
    brew install vulkan-sdk
    ```

2.  **Build OpenXMB:**
    ```bash
    # Clone the repository
    git clone https://github.com/phenom64/OpenXMB.git
    cd OpenXMB

    # Configure the project
    cmake -B build -G Ninja

    # Build the project
    cmake --build build

    # (Optional) Install the application
    # This will place the binary and assets in the specified directory.
    cmake --install build --prefix "/Applications/OpenXMB"
    ```
    The launcher script will automatically try to locate the `MoltenVK_icd.json` file required for Vulkan to work on macOS.

#### SynOS / Ubuntu-based Distributions

1.  **Install Dependencies (via APT):**
    ```bash
    sudo apt update
    sudo apt install build-essential git cmake ninja-build pkg-config \
        libvulkan-dev vulkan-validationlayers-dev spirv-tools \
        libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev \
        libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
        libglm-dev libfreetype-dev gettext libfmt-dev
    ```

2.  **Build OpenXMB:**
    ```bash
    # Clone the repository
    git clone https://github.com/phenom64/OpenXMB.git
    cd OpenXMB

    # Configure the project
    cmake -B build -G Ninja

    # Build the project
    cmake --build build

    # (Optional) Install the application system-wide
    sudo cmake --install build
    ```

#### Windows

1.  **Install Dependencies:**
    *   **Visual Studio 2022:** Install with the "Desktop development with C++" workload.
    *   **Git, CMake, Ninja:** Install these tools and ensure they are in your system's PATH.
    *   **Vulkan SDK:** Download and install from the [LunarG website](https://vulkan.lunarg.com/).
    *   **vcpkg:** Use vcpkg to install the remaining dependencies.
      ```powershell
      # Clone and set up vcpkg
      git clone https://github.com/microsoft/vcpkg.git
      cd vcpkg
      ./bootstrap-vcpkg.bat
      ./vcpkg integrate install

      # Install dependencies
      ./vcpkg install sdl2 sdl2-image sdl2-mixer ffmpeg freetype glm fmt gettext --triplet x64-windows
      ```

2.  **Build OpenXMB:**
    ```powershell
    # Clone the repository
    git clone https://github.com/phenom64/OpenXMB.git
    cd OpenXMB

    # Configure the project, replacing [path to vcpkg] with your vcpkg directory
    cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=[path to vcpkg]/scripts/buildsystems/vcpkg.cmake

    # Build the project
    cmake --build build

    # (Optional) Install the application
    cmake --install build --prefix "C:/OpenXMB"
    ```

### Build Options

You can customize the build using the following CMake options:
*   `-DENABLE_VIDEO_PLAYER=ON/OFF`: Enable the FFmpeg-based video player (Default: ON)
*   `-DENABLE_BROWSER=ON/OFF`: Enable the CEF-based browser module [ALPHA] (Default: OFF)
*   `-DENABLE_DISC_MEDIA=ON/OFF`: Enable DVD/Blu-ray support [ALPHA] (Default: OFF)
*   `-DENABLE_LIBRETRO=ON/OFF`: Enable the libretro core host for emulation [ALPHA] (Default: OFF)
*   `-DINTERFACE_FX_DEBUG=ON/OFF`: Enable interface/UI graphics+text debug overlays (Default: ON)

Example: `cmake -B build -DENABLE_BROWSER=ON`

## Configuration

OpenXMB is configured using the `config.json` file. When you first run the application using the `XMS` launcher script, a default `config.json` will be created in your working directory. You can edit this file to change settings like:

*   Background colors and type (`wave`, `color`, `image`)
*   Fonts and date/time display
*   Controller settings
*   Render quality (VSync, MSAA, FPS limit)

The application looks for assets (icons, sounds, fonts) in a directory specified by the `XMB_ASSET_DIR` environment variable. If not set, it defaults to the `share/shell` directory relative to the executable.

## Roadmap

*   **✅ M1: Scaffold:** App compiles & runs; static XMB; JSON config.
*   **🚧 M2: Menus, Fonts, Icons:** Fully navigable XMB with text & icons.
*   ** M3: Audio & Music:** Background music playback, visualizer, and sound effects.
*   **🚧 M4: Video Player:** File playback with software decoding is complete. Hardware acceleration and subtitles are next.
*   ** M5: Libretro Overlay:** Emulation support via a libretro core host.
*   ** M6: Web Browser:** Integration of a CEF-based browser.
*   ** M7: Disc Media:** Support for DVD/Blu-ray playback.
*   ** M8: Performance Pass:** Pipeline caching, optimized memory usage, and descriptor indexing review.

## License

OpenXMB is distributed under the **GNU General Public License v3.0 (GPLv3)**.  
See the [LICENSE](LICENSE) file for the full text.

A significant portion of the OpenXMB base is derived from **XMBShell**, and this is reflected in code taken from that project.  
Many thanks to its author, **JCM**.

All other original code is © 2025 Syndromatic Ltd and contributors, and licensed under version 3 of the GNU General Public License.  
AuroreEngine components adapted from **dreamrender** remain under the MPL 2.0 license.

---

## Acknowledgements

OpenXMB incorporates work from several outstanding open-source projects:

* **[XMBShell](https://github.com/JnCrMx/xmbshell)** – significant portions of OpenXMB are derived from XMBShell, originally created by JCM.  
* **[RetroArch](https://github.com/libretro/RetroArch)** – portions of the codebase are reused for media and emulation functionality.  
* **[dreamrender](https://github.com/JnCrMx/dreamrender)** – AuroreEngine, the rendering backend, is based on dreamrender.  

And of course, thanks to the wider ecosystem, including:  
* SDL2  
* Vulkan  
* FFmpeg  
* Freetype  
* glm  
* spdlog  
* i18n-cpp  
* nlohmann/json  
* argparse  
