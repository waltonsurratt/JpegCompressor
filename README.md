# Jpeg Compressor
This a new project for a native, Win32-based Windows application written entirely in C++ that compresses JPEG images and offers flexibility with adjusting the quality level as desired and reducing overall file size. This application incorporates the library libjpeg-turbo for JPEG file compression and optimizes the performance of all changes.


# Version: 1.3.0
The tool currently includes the following features:
* Batch file processing
* Error handling for non-JPEG files
* Includes dynamic linking (.dll) to libjpeg-turbo
* File overwrite behavior
* Image Quality slider bar (linked to compression rate)
* Drag-and-drop feature
* Cancellation support

<img width="612" height="398" alt="image" src="https://github.com/user-attachments/assets/767d8af4-32e6-4669-8d4a-06c9d5ee0996" />


## Future Changes
* ~~Batch file processing~~
* ~~Cancellation support / Cancel Button (after compression begins)~~
* ~~Non-UI blocking compression process~~
* Memory optimization for JPEG decode process (speed up the decode by a few 100ms)
* Thread optimization (faster encodes for batch files)
* Check-for-updates feature

## Installing
### Windows
Download the latest [JpegCompressor installer](https://github.com/waltonsurratt/JpegCompressor/releases/latest). `JpegCompressor-x86-setup.exe` is 32-bit. For 64-bit systems, download `JpegCompressor-x64-setup.exe`. All external libraries during build are statically linked, and so the executables act as independent entities and should launch on most Windows machines without additional setup or installation files required.

_This application does not yet automatically update for new releases._

You can also download `JpegCompressor-x86.zip` (32-bit),`JpegCompressor-x64.zip` (64-bit), or `JpegCompressor-x86-x64.zip` (both versions) from the [releases page](https://github.com/waltonsurratt/JpegCompressor/releases/latest) for the current executables. The `.zip` versions of the application do not automatically update either.
