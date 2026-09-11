# Install VCPKG to utilize with MS Visual Studio
Link: https://github.com/microsoft/vcpkg

1. Download the ZIP folder or clone the .git respository
2. Unzip the folder and place it under the root directory of the C drive (C:\vcpkg)
3. Open command prompt (as administrator)
4. Run the commands to install:
	cd C:\vcpkg
	.\bootstrap-vcpkg.bat
	
# Install the required dependencies from the VCPKG online repository (note: this part may take some time as each library is first downloaded after each command, then built & compiled on your machine to ensure it runs)
5. From the same window, run the commands:
	vcpkg install libjpeg-turbo:x64-windows-static
	vcpkg install libjpeg-turbo:x86-windows-static
	vcpkg install cpp-httplib:x64-windows-static
	vcpkg install cpp-httplib:x86-windows-static
	vcpkg install openssl:x64-windows-static
	vcpkg install openssl:x86-windows-static
	vcpkg install nlohmann-json:x64-windows-static
	vcpkg install nlohmann-json:x86-windows-static
	
# Integrate the installed packages into the MS Visual Studio environment
6. From the same window, run the commands:
	vcpkg integrate install

# Adjust your MS Visual Studio project settings to align with the recommended compile options (Note: My project uses all "static" libraries, so these were the recommended settings in order for it to compile properly as a single executable)
7. Launch MS Visual Studio -> open project
8. Go to Project -> Properties -> vcpkg
9. Change "Use Static Libraries" to "Yes", then click Apply to save
10. Change Platform to Win32 -> update "Triplet" to "x86-windows-static", then click Apply to save
11. Change "Platform" to x64 -> update "Triplet" to "x64-windows-static", then click Apply to save
12. Go to Project -> Properties -> C\C++ -> General
13. Select "Additional Include Directories" -> Edit -> Check the box for "Inherit from parent or project defaults", then click Apply to save
14. Go to Project -> Properties -> C\C++ -> Code Generation
15. Change "Configuration" to Debug; All Platforms -> Select "Runtime Library" and select the option "Multi-threaded Debug (/MTd), then click Apply to save
16. Change "Configuration" to Release; All Platforms -> Select "Runtime Library" and select the option "Multi-threaded (/MT), then click Apply to save
