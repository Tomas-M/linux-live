Use the remaining space on your USB drive

Writing the Slax ISO image directly to a disk using rawrite tools (or dd)
leaves the space beyond the image unallocated. Replace the small SLAX-DATA
partition with a data partition that fills the available space.

In Slax

1. Open Partition Manager and select the drive you wrote the ISO to.
2. If Repair layout is shown, click it, then Review changes and Apply changes.
3. Unmount SLAX-DATA if it is mounted. Select SLAX-DATA and click the trash button.
   This removes this README and any files you added to SLAX-DATA. Keep the
   existing ISO and EFI partitions.
4. Select the unallocated space after the EFI partition and choose New
   partition. Click Use all space, choose FAT32, then click
   Add to plan.
5. Click Review changes. Check that the plan deletes only SLAX-DATA and
   creates the new data partition, then click Apply changes.

In Windows

1. Open Disk Management (right-click Start and choose Disk Management).
2. Find the USB disk by its capacity and the SLAX-DATA label. Right-click
   SLAX-DATA and choose Delete Volume. Keep the other partitions on that disk.
3. Right-click the resulting unallocated space and choose New Simple Volume.
   Accept the maximum size, assign a drive letter and choose exFAT, or FAT32
   if offered. Click Finish.

If Delete Volume is unavailable or the expected space is missing in Windows,
use the Slax instructions above.
