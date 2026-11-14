If you wrote the Slax ISO directly to your USB drive using dd, Rawrite or a
similar tool, Windows shows only a small drive with this README and the
Use USB Space tool. The Slax partition is hidden and the remaining space is not yet
available for your own files. Most of the drive's capacity therefore appears
to be missing.

Follow the steps below to make it available for documents, photos and other files.
Slax will still start from the USB drive.

In Windows

1. Double-click usb-space.exe in the same folder as this file.
2. Follow the prompts and wait for the tool to finish.
3. Open the new USB-DATA drive in File Explorer and copy your files to it.

The tool also prepares slax/changes for Slax profiles. Slax automatically
saves your changes there when you use "Resume previous profile automatically".
Keep this directory to retain your saved changes.

In Slax

1. Open Partition Manager and select your USB drive.
2. If a "Repair layout" button appears, click it first.
3. Select "Unallocated space", click New, and choose FAT32 as the filesystem.
4. Click "Add to plan", then "Review changes" and "Apply changes".

Slax partition remains hidden in this setup.
