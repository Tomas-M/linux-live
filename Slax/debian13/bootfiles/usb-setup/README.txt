If you wrote the Slax ISO directly to your USB drive using dd, Rawrite or a
similar tool, Windows shows only a small drive with this README and the
setup tool. The Slax partition is hidden and the remaining space is not yet
available for your own files. Most of the drive's capacity therefore appears
to be missing.

Follow the steps below to make it available for documents, photos and other files.
Slax will still start from the USB drive.

In Windows

1. Double-click disk-setup.exe in the same folder as this file.
2. Follow the prompts and wait for setup to finish.
3. Open the new USB-DATA drive in File Explorer and copy your files to it.

In Slax

1. Open Partition Manager and select your USB drive.
2. If a "Repair layout" button appears, click it first.
3. Select "Unallocated space", click New, and choose FAT32 as the filesystem.
4. Click "Add to plan", then "Review changes" and "Apply changes".

Leave the existing partitions unchanged so Slax can still start.
