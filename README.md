# Vivaldi Browser Source Code:Extract from "vivaldi-source_1.12.955.tar.xz" that downloaded from https://vivaldi.com/source

Building details from https://github.com/ric2b/Vivaldi-browser

The file README has the original readme from the developpers.

Building

(Thanks to RAZR_96 for providing these instructions in this comment)

The build instructions (from the README file) is missing a command to make the file (or you could make it manually): vivaldi-source\testdata\stp.viv and add to the file: // Vivaldi Standalone

Build according to the instructions on the README file

The build will use a standalone profile as a result of this file. To use your normal profile you need to delete stp.viv from:
vivaldi-source\out\Release. Be aware that using different versions of Vivaldi on the same profile will give an error on start up. It'll probably still work but I wouldn't advise it.

Once you've built it you need to find where Vivaldi is installed and copy this folder: Vivaldi\Application\1.8.770.50\resources\vivaldi

to this directory: vivaldi-source\out\Release\resources\

If anyone wants to build on windows, there's a build.bat here. You also need WinSparkle. Extract the contents of the zip to:

vivaldi-source\third_party_winsparkle_lib

And lastly the 'Release' folder will be about 30GB once built. Not all of the files/folders in it are needed to run the browser. So here's a Powershell script I made that copies only the stuff you need.
