# Jpeg Compressor
This a new project for a native, Win32-based Windows application written entirely in C/C++ that compresses JPEG images and offers flexibility with adjusting the quality level as desired and reducing overall file size. This application incorporates the library libjpeg-turbo for JPEG file compression and optimizes the performance of all changes.


# Version: 1.4.0
The tool currently includes the following features:
* Batch file processing
* Error handling for non-JPEG files
* Includes dynamic linking (.dll) to libjpeg-turbo
* File overwrite behavior
* Image Quality slider bar (linked to compression rate)
* Drag-and-drop feature
* Cancellation support
* Check for update (in app)

<img width="612" height="398" alt="image" src="https://github.com/user-attachments/assets/767d8af4-32e6-4669-8d4a-06c9d5ee0996" />


## Future Changes
* ~~Batch file processing~~
* ~~Cancellation support / Cancel Button (after compression begins)~~
* ~~Non-UI blocking compression process~~
* ~~Memory optimization for JPEG decode process (speed up the decode by a 200ms+ per file)~~
* ~~Thread optimization (faster encodes for batch files)~~
* ~~Check-for-updates feature~~

## Installing
### Windows
Download the latest [JpegCompressor installer](https://github.com/waltonsurratt/JpegCompressor/releases/latest). `JpegCompressor-x86-setup.exe` is 32-bit. For 64-bit systems, download `JpegCompressor-x64-setup.exe`. All external libraries during build are statically linked, and so the executables act as independent entities and should launch on most Windows machines without additional setup or installation files required.

You can also download `JpegCompressor-x86.zip` (32-bit),`JpegCompressor-x64.zip` (64-bit), or `JpegCompressor-1.4.0.zip` (both versions) from the [releases page](https://github.com/waltonsurratt/JpegCompressor/releases/latest) for the current executables.

Note: The installation files with _setup_ in the name include the full installer that adds the program to the machine. The release/binary files without the text _setup_ are the fully compiled executables that can be launched from any folder.

NEW: A "Check for updates" button has been officially added to the application. Users may now use this method to receive updates if they wish. Likewise, those who do not wish to use the option may continue to download the latest releases directly from GitHub and install the application manually.
