#!/bin/bash
# script by Olaf Koch 3.11.2018

### settings
dev=$1
liveimagename="mediakit"
squashfilesystem="/run/initramfs/memory/data"
if [ -e /run/initramfs/memory/data/vmlinuz ]; then
 kernelfile="/run/initramfs/memory/data/vmlinuz"
else
 kernelfile="/run/initramfs/memory/data/mediakit/boot/vmlinuz"
fi
if [ -e /run/initramfs/memory/data/initrfs.img ]; then
 initrfsfile="/run/initramfs/memory/data/initrfs.img"
else
 initrfsfile="/run/initramfs/memory/data/mediakit/boot/initrfs.img"
fi

###



if [[ ${EUID} -ne 0 ]]; then
  echo "this script must be executed with elevated privileges"
  exit 1
fi

if [[ -z "${dev}" || ! -b ${dev} ]]; then
  echo "param 1 must be target block device"
  exit 1
fi


#if [[ "${dev}" =~ ^.*[0-9]$ ]]; then
#  echo "target block device should be device, not a partition (must not end with digit)"
#  exit 1
#fi
tput setaf 11
echo "liveimagename="$liveimagename
#echo "squashfilesystem="$squashfilesystem
echo "kernelfile="$kernelfile
echo "initrfsfile="$initrfsfile
echo "device="${dev}
#echo "Install on device ${dev}? (Enter to continue/STRG-C to abort)"
#read
tput sgr0
#rm -rf $dirname/LIVE_BOOT/

tput setaf 11
echo "unmounting old partitions"
tput sgr0
umount ${dev}*

tput setaf 11
echo "install all neccessary packages..."
tput sgr0
#apt-get install \
#    grub-pc-bin \
#    grub-efi-amd64-bin \
#    mtools

###########
tput setaf 11
echo "creating bootable medium ..."
#read

tput sgr0
export disk=${dev}
tput setaf 11
echo "wipe out medium ..."
#read
tput sgr0
sgdisk --zap-all $disk

mkdir -p /mnt/{usb,efi}
tput setaf 11
echo "create partitions on medium ..."
#read
tput sgr0

parted --script $disk \
    mklabel gpt \
    mkpart primary fat32 2048s 4095s \
        name 1 BIOS \
        set 1 bios_grub on \
    mkpart ESP fat32 4096s 413695s \
        name 2 EFI \
        set 2 esp on \
    mkpart primary ext4 413696s 100% \
        name 3 $liveimagename \
        set 3 msftdata on

tput setaf 11
echo "create bootrecords on medium ..."
#read
tput sgr0
gdisk $disk << EOF
r     # recovery and transformation options
h     # make hybrid MBR
1 2 3 # partition numbers for hybrid MBR
N     # do not place EFI GPT (0xEE) partition first in MBR
EF    # MBR hex code
N     # do not set bootable flag
EF    # MBR hex code
N     # do not set bootable flag
83    # MBR hex code
Y     # set the bootable flag
x     # extra functionality menu
h     # recompute CHS values in protective/hybrid MBR
w     # write table to disk and exit
Y     # confirm changes
EOF


# get partitions
mapfile -t info < <( lsblk ${disk} -l)



i=1
for entry in "${info[@]}"
do
 if [[ `echo "$entry" | awk '{ print $6}'` == "part" ]]; then
       echo -e "\033[43m $i \e[0m" $entry
       parts[$i]=`echo "$entry" | awk '{ print "/dev/"$1}'`
       i=`expr $i + 1` 
    
fi
done



tput setaf 11
echo "Format UEFI and Data Partitions..."
#read
tput sgr0
#wipefs -af ${disk}2
mkfs.vfat -F32 ${parts[2]}
#wipefs -af ${disk}3
mkfs.ext4 -F ${parts[3]}

tput setaf 11
echo "Mount UEFI Partitions..."
#read
tput sgr0
mount ${parts[2]} /mnt/efi && \
mount ${parts[3]} /mnt/usb
rm -rf /mnt/efi/*
rm -rf /mnt/usb/*

mkdir -p /mnt/usb/{boot/grub,live}
tput setaf 11
echo "Install Grub for UEFI..."
#read
tput sgr0
grub-install \
    --target=x86_64-efi \
    --efi-directory=/mnt/efi \
    --boot-directory=/mnt/usb/boot \
    --removable \
    --recheck
tput setaf 11
echo "Install Grub for BIOS..."
#read
tput sgr0

sudo grub-install \
    --force \
    --target=i386-pc \
    --boot-directory=/mnt/usb/boot \
    --recheck \
    ${disk}

tput setaf 11
echo "Install Fallbackgrub for BIOS..."
#read
tput sgr0

sudo grub-install \
    --force \
    --target=i386-pc \
    --boot-directory=/mnt/usb/boot \
    --recheck \
    ${parts[3]}

tput setaf 11
echo "Install Live Filesystem ..."
tput sgr0

mkdir /mnt/usb/$liveimagename
mkdir /mnt/usb/$liveimagename/modules
mkdir /mnt/usb/$liveimagename/changes
#mkdir /mnt/usb/scripts
cp  -R $squashfilesystem/$liveimagename/* /mnt/usb/$liveimagename/
#cp ./createBootDevice.sh /mnt/usb/scripts/
#cp ./saveLiveKit2Disk.sh /mnt/usb/scripts/

cp  $kernelfile /mnt/usb/
cp  $initrfsfile /mnt/usb/

touch /mnt/usb/$liveimagename

tput setaf 11
echo "create grub ..."
tput sgr0
echo "search --set=root --label $liveimagename" >/mnt/usb/boot/grub/grub.cfg
echo " " >>/mnt/usb/boot/grub/grub.cfg
echo "insmod all_video">>/mnt/usb/boot/grub/grub.cfg
echo " ">>/mnt/usb/boot/grub/grub.cfg
echo 'set default="0"'>>/mnt/usb/boot/grub/grub.cfg
echo 'set timeout=1'>>/mnt/usb/boot/grub/grub.cfg
echo " ">>/mnt/usb/boot/grub/grub.cfg
echo 'menuentry "'$liveimagename'" {'>>/mnt/usb/boot/grub/grub.cfg
echo '    linux /vmlinuz load_ramdisk=1 prompt_ramdisk=0 rw printk.time=0 net.ifnames=0 biosdevname=0 mediakit.flags=perch,nochange quiet splash'>>/mnt/usb/boot/grub/grub.cfg
echo '    initrd /initrfs.img' >>/mnt/usb/boot/grub/grub.cfg
echo '}'>>/mnt/usb/boot/grub/grub.cfg

tput setaf 10
echo "Umount ${dev}..."
tput sgr0


umount /mnt/{usb,efi}
tput setaf 10
echo "Everthing is done!"
echo "<Press any key to continue>"
tput sgr0
read

