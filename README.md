# Aksyn Audio Prototype

A high-fidelity, low-latency audio streaming application built in C++17. This system captures raw audio from a microphone (bypassing OS mixer degradation via WASAPI Exclusive mode) and streams it over WebSockets to a receiving node for playback and recording.

## 🔗 Project Links
* **Team Head Google Drive:** [Insert Link Here]
* **Client Google Drive:** [Insert Link Here]

---

## 🏗️ System Architecture

The project consists of two distinct executables:
* **Node A (Server & Capture):** Captures 32-bit float audio directly from the default microphone. It hosts a WebSocket server on port `9001` to broadcast the audio stream.
* **Node B (Client & Playback):** Connects to Node A, manages network jitter using an adaptive buffer, plays the received audio, and saves lossless `.wav` recordings to the disk.

---

## ⚙️ Prerequisites (For a New Computer)

To compile and run this code on a fresh machine, you will need to install a few standard developer tools:

1.  **C++ Compiler:** * *Windows:* Download and install **Visual Studio Community** (make sure to select the "Desktop development with C++" workload during installation).
    * *macOS/Linux:* Install `gcc` or `clang` (e.g., via `xcode-select --install` or `sudo apt install build-essential`).
2.  **CMake (Version 3.14 or higher):** Download from [cmake.org](https://cmake.org/download/) and ensure it is added to your system PATH.
3.  **Git:** Download from [git-scm.com](https://git-scm.com/). CMake uses Git behind the scenes to fetch the networking library.

*(Note: You do not need to manually download `miniaudio` or `IXWebSocket`—the build system will automatically fetch them for you.)*

---

## 🛠️ Build Instructions

1.  **Open a Terminal** (Command Prompt, PowerShell, or bash) and navigate to the folder containing your source code (`CMakeLists.txt`, `node_A.cpp`, etc.).
2.  **Create a build directory:**
    ```bash
    mkdir build
    cd build
    ```
3.  **Configure the project:**
    ```bash
    cmake ..
    ```
4.  **Compile the executables:**
    ```bash
    cmake --build . --config Release
    ```

Once finished, you will find `node_A` and `node_B` (or `.exe` files on Windows) in the `build/Release` (or `build/`) directory.

---

## 🚀 How to Run

### 1. Running Locally (Same Computer)
If you are testing both nodes on the same machine, run them in two separate terminal windows and go into the build folder.

**Terminal 1:**
```bash
./node_A.exe

**Terminal 2:**
```bash
./node_B.exe