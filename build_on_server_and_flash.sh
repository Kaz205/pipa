#!/bin/bash

build_mode=$1
echo $build_mode | grep -q "clean\|dirty"
if [ $? = 1 ]
then
    echo "Please specify whether to clean build or dirty build"
    exit 1
fi

if [ $build_mode = "clean" ]
then
    ssh kazuki@192.168.13.229 "rm -rf /home/kazuki/pipa/out"
fi

# Build
ssh -tt kazuki@192.168.13.229 "cd pipa && git pull origin && git reset --hard FETCH_HEAD && bash build_kernel.sh"
if [ $? != 0 ]
then
    echo $?
    echo -e "\e[0;31mBuild failed, exiting... \e[0m"
    exit 1
fi

cd prebuilts
scp kazuki@192.168.13.229:/home/kazuki/AnyKernel3/Image Image
scp kazuki@192.168.13.229:/home/kazuki/AnyKernel3/dtb dtb
scp kazuki@192.168.13.229:/home/kazuki/AnyKernel3/dtbo.img dtbo.img
python3 mkbootimg.py --header_version 3 --output boot.img --kernel Image --ramdisk pipa-ramdisk --os_version 13.0.0 --os_patch_level 2023-09
python3 mkbootimg.py --header_version 3 --vendor_boot vendor_boot.img --vendor_ramdisk pipa-vendor_ramdisk --dtb dtb

fastboot flash boot boot.img
fastboot flash vendor_boot vendor_boot.img
fastboot flash dtbo dtbo.img
