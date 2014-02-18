#!/bin/bash
#
# Linux Shell Script For Compiling GC/Wii Linux Kernels
# Written by DeltaResero <deltaresero@zoho.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# on the rights to use, copy, modify, merge, publish, distribute, sub
# license, and/or sell copies of the Software, and to permit persons to whom
# the Software is furnished to do so, subject to the following conditions:
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#
#


# Set defaults and checks if arch is supported by this script 
clear
echo "This basic script is meant as a way to help with cross compiling"
echo "GameCube and Wii Kernels.  While this isn't very robust, it should"
echo -e "suffice for most basic compilations.\n"

echo "Compiling a kernel usually requires the following dependencies at minimal:"
echo "advancecomp (advdef), autoconfig, automake, bash, build-essential, busybox,"
echo "bzip2, ccache, fakeroot, gcc, g++, gizp, libmpfr-dev, libgmp-dev,libnurses5-dev,"
echo "libnurses5-dev, strip.  This will also require 'gcc-powerpc-linux-gnu' if using"
echo "the newer OS cross compiler included in recent systems with multiarch support."
echo -e "While there are other dependencies, these are the most common ones."
echo "If any are missing, it's highly recommended that this script be stopped"
echo -e "and these dependency packages be installed before continuing.\n"

buildTarget=''
useConfig=''
MACHINE_TYPE=`arch`

#Find number of processors for setting number of parallel jobs
echo "- Processor Information -"
echo "CPU Architecture: "${MACHINE_TYPE}
numProcessors=$(grep -c ^processor /proc/cpuinfo)
echo "Number of processors:" ${numProcessors}
#------------------------------------------------------------------------------


while : # Gets the user to select a console target (GameCube or Wii)
do
  printf "\n\nEnter a value below matching the target platform or enter 4 to use '.config':\n"
  printf "(Selecting 'quit' at any time quit will exit this script)\n"
  printf "1) GameCube\n"
  printf "2) Wii\n"
  printf "3) Other/Use an existing .config\n"
  printf "4) Quit Script\n"
  echo -n "Response: "
  read opt1
  case $opt1 in
    1) printf "\n\nSelecting target: GameCube\n"
       buildTarget='GameCube'
       break;;
    2) printf "\n\nSelecting target: Wii"
       buildTarget='Wii'
       break;;
    3) printf "\n\nSelecting target: Unknown (Not needed if .config has already been created)\n"
       buildTarget='Unknown'
       break;;
    4) printf "\n\nQuitting script...\n"
       exit 0;;
    *) printf "\n\n$opt is an invalid option.\n"
       printf "Please select an option from 1-4 only\n"
       printf "Press [enter] key to continue...\n"
       read enterKey
       ;;
  esac
done
#------------------------------------------------------------------------------


if [[ ${buildTarget} != 'Unknown' ]]; then
  # Attempts to get user to select a base configuration to start with
  while :
  do
    printf "\n\nEnter a numerical value corresponding to the configuration to be used\n"
    printf "(Remember to edit the platform dts 'bootags' to correspond to the build type)\n"
    printf "1) Isobel's  - Default / With Modules (Wii MINI, GameCube - Default)\n"
    printf "2) DeltaResero's - Minimalist / No Modules (Wii MINI, GameCube - Small)\n"
    printf "3) DeltaResero's - Minimalist / No Modules (Wii IOS, GameCube - Same as 2 for now)\n"
    printf "4) Quit Script\n"
    echo -n "Response: "
    read opt2
    case $opt2 in
      1) if [[ ${buildTarget} == 'GameCube' ]]; then
          printf "\n\nSelecting config: gamecube_defconfig\n"
          useConfig='gamecube_defconfig'
        elif [[ ${buildTarget} == 'Wii' ]]; then
          printf "\n\nSelecting config: wii_defconfig\n"
          useConfig='wii_defconfig'
        fi
        break;;
      2) if [[ ${buildTarget} == 'GameCube' ]]; then
          printf "\n\nSelecting config: gamecube_smaller_defconfig\n"
          useConfig='gamecube_smaller_defconfig'
        elif [[ ${buildTarget} == 'Wii' ]]; then
          printf "\n\nSelecting config: wii-mini-mode_defconfig\n"
          useConfig='wii-mini-mode_defconfig'
        fi
        break;;
      3) if [[ ${buildTarget} == 'GameCube' ]]; then
          printf "\n\nSelecting config: gamecube_smaller_defconfig\n"
          useConfig='gamecube_smaller_defconfig'
        elif [[ ${buildTarget} == 'Wii' ]]; then
          printf "\n\nSelecting config: wii-ios-mode_defconfig\n"
          useConfig='wii-ios-mode_defconfig'
        fi
        break;;
      4) printf "\n\nQuitting script...\n"
        exit 0;;
      *) printf "\n\n$opt is an invalid option.\n"
        printf "Please select option from 1-4 only\n"
        printf "Press [enter] key to continue...\n"
        read enterKey
        ;;
    esac
  done

else
  printf "\n\nSelecting config: .config\n"
  useConfig='.config'
fi
#------------------------------------------------------------------------------


# Sets firmware folder path
FMW_DIRECTORY='lib/firmware'

# Removes the firmware directory if it already exist
if [[ -d "$FMW_DIRECTORY" ]]; then
  echo "Removing the old contents from firmware folder..."
  rm -R $FMW_DIRECTORY
fi

#Creates the firmware directory
echo "Creating firmware folder / adding contents into:" $FMW_DIRECTORY
mkdir $FMW_DIRECTORY


# Sets the headers folder path
HDR_DIRECTORY='usr/lib'

# Removes the headers directory if it already exist
if [[ -d "$HDR_DIRECTORY" ]]; then
  echo "Removing the old contents from headers folder..."
  rm -R $HDR_DIRECTORY
fi

#Creates the headers directory
echo "Creating headers folder / adding contents into:" $HDR_DIRECTORY
mkdir $HDR_DIRECTORY
#------------------------------------------------------------------------------


if [[ ${MACHINE_TYPE} == 'ppc' ]]; then
  printf "\n\nCross compiler not needed...\n"
  if [[ ${useConfig} != '.config' ]]; then
    make ${useConfig}
  fi
  make menuconfig
  make clean
  if [[ -f 'arch/powerpc/boot/ramdisk.image.gz' ]]; then
    printf "Ramdisk found, building zImage.initrd...\n"
    make firmware_install headers_install zImage.initrd -j${numProcessors} INSTALL_FW_PATH=$FMW_DIRECTORY INSTALL_HDR_PATH=$HDR_DIRECTORY 
  else
    printf "No ramdisk found, building zImage...\n"
    make firmware_install headers_install zImage -j${numProcessors} INSTALL_FW_PATH=$FMW_DIRECTORY INSTALL_HDR_PATH=$HDR_DIRECTORY 
  fi
  printf "\nNote: If this is the target machine, it should be possible to install everything with 'sudo make install'."

elif [[ ${MACHINE_TYPE} == 'x86_32' || ${MACHINE_TYPE} == 'x86_64' ]]; then
  while : # Gets the user to select a cross compiler to use
  do
    printf "\n\nSelect a cross compiler to use:"
    printf "(Selecting a quit option at any menu will exit this script)\n"
    printf "1) Included gcLinux 2009 Buildroot cross compiler (depreciated)\n"
    printf "2) External OS provided PPC cross compiler (requires gcc-powerpc-linux-gnu)\n"
    printf "3) Quit script (Alternatives: http://www.gc-linux.org/wiki/Cross-compiling)\n"
    echo -n "Response: "
    read opt3
    case $opt3 in

      1) printf "\n\nSelecting gcLinux Buildroot cross compiler\n"
        if [[ ${MACHINE_TYPE} == 'x86_32' ]]; then
          export LD_LIBRARY_PATH=H-i686-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/lib

          if [[ ${useConfig} != '.config' ]]; then
            make ${useConfig} ARCH=powerpc CROSS_COMPILE=H-i686-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-
          fi

          make menuconfig ARCH=powerpc CROSS_COMPILE=H-i686-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-
          make clean ARCH=powerpc CROSS_COMPILE=H-i686-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-
          if [[ -f 'arch/powerpc/boot/ramdisk.image.gz' ]]; then
            printf "Ramdisk found, building zImage.initrd...\n"
            make firmware_install headers_install zImage.initrd -j${numProcessors} INSTALL_FW_PATH=$FMW_DIRECTORY INSTALL_HDR_PATH=$HDR_DIRECTORY  ARCH=powerpc CROSS_COMPILE=H-i686-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-
          else
            printf "No ramdisk found, building zImage...\n"
            make firmware_install headers_install zImage -j${numProcessors} INSTALL_FW_PATH=$FMW_DIRECTORY INSTALL_HDR_PATH=$HDR_DIRECTORY  ARCH=powerpc CROSS_COMPILE=H-i686-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-
          fi
        elif [[ ${MACHINE_TYPE} == 'x86_64' ]]; then
          export LD_LIBRARY_PATH=H-x86_64-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/lib

          if [[ ${useConfig} != '.config' ]]; then
            make ${useConfig} ARCH=powerpc CROSS_COMPILE=H-x86_64-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-
          fi

          make menuconfig ARCH=powerpc CROSS_COMPILE=H-x86_64-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-
          make clean ARCH=powerpc CROSS_COMPILE=H-x86_64-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-
          if [[ -f 'arch/powerpc/boot/ramdisk.image.gz' ]]; then
            printf "Ramdisk found, building zImage.initrd...\n"
            make firmware_install headers_install zImage.initrd -j${numProcessors} INSTALL_FW_PATH=$FMW_DIRECTORY INSTALL_HDR_PATH=$HDR_DIRECTORY  ARCH=powerpc CROSS_COMPILE=H-x86_64-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-
          else
            printf "No ramdisk found, building zImage...\n"
            make firmware_install headers_install zImage -j${numProcessors} INSTALL_FW_PATH=$FMW_DIRECTORY INSTALL_HDR_PATH=$HDR_DIRECTORY  ARCH=powerpc CROSS_COMPILE=H-x86_64-pc-linux-gnu/cross-powerpc-linux-uclibc/usr/bin/powerpc-linux-
          fi
        fi         
        break;;

      2) printf "\n\nSelecting OS cross compiler\n"
        if [[ ${useConfig} != '.config' ]]; then
          make ${useConfig} ARCH=powerpc GCC_HOST=powerpc-linux-gnu- CROSS_COMPILE=powerpc-linux-gnu- CC="ccache powerpc-linux-gnu-gcc"
        fi

        make menuconfig ARCH=powerpc GCC_HOST=powerpc-linux-gnu- CROSS_COMPILE=powerpc-linux-gnu- CC="ccache powerpc-linux-gnu-gcc"
        make clean ARCH=powerpc GCC_HOST=powerpc-linux-gnu- CROSS_COMPILE=powerpc-linux-gnu- CC="ccache powerpc-linux-gnu-gcc"
        if [[ -f 'arch/powerpc/boot/ramdisk.image.gz' ]]; then
          printf "Ramdisk found, building zImage.initrd...\n"
          make firmware_install headers_install zImage.initrd -j${numProcessors} INSTALL_FW_PATH=$FMW_DIRECTORY INSTALL_HDR_PATH=$HDR_DIRECTORY  ARCH=powerpc GCC_HOST=powerpc-linux-gnu- CROSS_COMPILE=powerpc-linux-gnu- CC="ccache powerpc-linux-gnu-gcc"
        else
          printf "No ramdisk found, building zImage...\n"
          make firmware_install headers_install zImage -j${numProcessors} INSTALL_FW_PATH=$FMW_DIRECTORY INSTALL_HDR_PATH=$HDR_DIRECTORY  ARCH=powerpc GCC_HOST=powerpc-linux-gnu- CROSS_COMPILE=powerpc-linux-gnu- CC="ccache powerpc-linux-gnu-gcc"
        fi
        break;;

      3) printf "\n\nQuitting script...\n"
        exit 0;;

      *) printf "\n\n$opt is an invalid option.\n"
        printf "Please select an option from 1-3 only\n"
        printf "Press [enter] key to continue...\n"
        read enterKey
        ;;
    esac
  done
  
else # !ppc && !x86_32 && !x86_64
  while : # Gets the user to varify the cross compiler to use
  do
    printf "\n\nSelect a cross compiler to use:\n"
    printf "(Selecting a quit option at any menu will exit this script)\n"
    printf "1) External OS provided PowerPC cross compiler (requires gcc-powerpc-linux-gnu)\n"
    printf "2) Quit script (Alternatives: http://www.gc-linux.org/wiki/Cross-compiling)\n"
    echo -n "Response: "
    read opt4
    case $opt4 in
      1) printf "\n\nSelecting OS cross compiler\n"
        if [[ ${useConfig} != '.config' ]]; then
          make ${useConfig} ARCH=powerpc GCC_HOST=powerpc-linux-gnu- CROSS_COMPILE=powerpc-linux-gnu- CC="ccache powerpc-linux-gnu-gcc"
        fi

        make menuconfig ARCH=powerpc GCC_HOST=powerpc-linux-gnu- CROSS_COMPILE=powerpc-linux-gnu- CC="ccache powerpc-linux-gnu-gcc"
        make clean ARCH=powerpc GCC_HOST=powerpc-linux-gnu- CROSS_COMPILE=powerpc-linux-gnu- CC="ccache powerpc-linux-gnu-gcc"
        if [[ -f 'arch/powerpc/boot/ramdisk.image.gz' ]]; then
          printf "Ramdisk found, building zImage.initrd...\n"
          make firmware_install headers_install zImage.initrd -j${numProcessors} INSTALL_FW_PATH=$FMW_DIRECTORY INSTALL_HDR_PATH=$HDR_DIRECTORY  ARCH=powerpc GCC_HOST=powerpc-linux-gnu- CROSS_COMPILE=powerpc-linux-gnu- CC="ccache powerpc-linux-gnu-gcc"
        else
          printf "No ramdisk found, building zImage...\n"
          make firmware_install headers_install zImage -j${numProcessors} INSTALL_FW_PATH=$FMW_DIRECTORY INSTALL_HDR_PATH=$HDR_DIRECTORY  ARCH=powerpc GCC_HOST=powerpc-linux-gnu- CROSS_COMPILE=powerpc-linux-gnu- CC="ccache powerpc-linux-gnu-gcc"
        fi
        break;;

      2) printf "\n\nQuitting script...\n"
         exit 0;;

      *) printf "\n\n$opt is an invalid option.\n"
         printf "Please select an option from 1-2 only\n"
         printf "Press [enter] key to continue...\n"
         read enterKey
         ;;
    esac
  done    
fi
#------------------------------------------------------------------------------


# Placeholder for the path to the zImage/zImage.intrid binary
zImageFile=''

# Checks for a zImage/zImage.initrd binary and if none exist, build was likely unsuccessful
if [[ -f 'arch/powerpc/boot/zImage' ]]; then
  printf "zImage found...\n"
  zImageFile='arch/powerpc/boot/zImage'
elif [[ -f 'arch/powerpc/boot/zImage.initrd' ]]; then
  printf "zImage.initrd found...\n"
  zImageFile='arch/powerpc/boot/zImage.initrd'
else # No zImage/zImage.intrid (was likely an unsuccessful build)
  printf "Error, zImage (Kernel) not found!\n"
  printf "Check above for errors as this was likely an unsuccessful build\n"
  printf "Quitting script...\n"
  exit 1
fi

while :
do
  printf "\n\nReduce kernel size by removing extra debug data (stripping)?\n"
  printf "1) Yes, use SuperStrip (https://github.com/BR903/ELFkickers.git)\n"
  printf "2) No, keep the zImage / zImage.initrd as is...\n"
  echo -n "Response: "
  read opt5
  case $opt5 in
    1) printf "\n\nSuperStrip selected...\n"
      printf "Checking for an existing installation\n"
      if [[ ! -e 'type sstrip' ]]; then # Use local copy of sstrip
        printf "SuperStrip found, stripping the debug info out of the zImage / zImage.initrd.\n"
        sstrip -z $zImageFile
      else
        printf "SuperStrip not found, stripping the debug info out of the zImage will\n"
        printf "require a manual installation of SuperStrip (sstrip)\n"
        printf "For more information, see: https://github.com/BR903/ELFkickers.git\n"
        printf "Stripping has been skipped...\n"
      fi
      break;;

    2) printf "\n\nNot stripping zImage / zImage.initrd...\n"
      break;;

    *) printf "\n\n$opt is an invalid option.\n"
      printf "Please select an option from 1-2 only\n"
      printf "Press [enter] key to continue...\n"
      read enterKey
      ;;
  esac
done
#------------------------------------------------------------------------------


# Sets modules folder path
MOD_DIRECTORY='lib/modules'

# Removes modules directory if it already exist
if [[ -d "$MOD_DIRECTORY" ]]; then
  echo "Removing the old contents from modules folder..."
  rm -R $MOD_DIRECTORY
fi

#Creates the modules directory
echo "Creating modules folder / adding contents into:" $MOD_DIRECTORY
mkdir $MOD_DIRECTORY

# Checks for modules and places them in a the modules folder
find ! -path $MOD_DIRECTORY -name '*.ko' -exec cp -av {} $MOD_DIRECTORY \;
#------------------------------------------------------------------------------


# Script finish message
printf "\nDone! (Check to see if there were any errors above)\n\n"
printf "The kernel (zImage) can be found in: 'arch/powerpc/boot'\n"
printf "Although the inclusion of headers, modules, and firmware are mostly non essential for basic use,\n"
printf "it's recommended to include these.  At minimal, any built modules and the firmware should be\n"
printf "included in their folders on the target device in the /lib/ folder (/lib/modules and /lib/firmware).\n\n"  
echo "Kernel headers should be located in the folder:" $HDR_DIRECTORY
printf "\nUsually kernel headers can be installed manually or by using a package manager instead...\n"
echo "Firmware (if any) should be located in the folder:" $FMW_DIRECTORY
echo "The b43 (WLAN) Wii firmware must be built / retrieved separate, see the following site for"
printf " more info: http://www.gc-linux.org/wiki/WL:Wifi_Configuration\n"
printf "\nFirmware should be placed into the '/lib/firmware' folder of the target system.\n"
echo "Modules (if any) should be located in the folder:" $MOD_DIRECTORY
printf "\nModules should be placed into the '/lib/modules/KERNEL_VERSION_NUMBER' folder of\n"
printf "the target system where KERNEL_VERSION_NUMBER is the numerical version of the kernel.\n"
printf "\nWARNING: If using a ramdisk based kernel, remember to add the new modules / firmware \n"
printf "into their appropriate folders in the ramdisk image and rebuild the kernel (with the\n"
printf "same configuration) with the new ramdisk image.\n" 
exit 0
#
# More info on the gcLinux cross compile tool can be found at the following website:
# http://www.gc-linux.org/wiki/Building_a_GameCube_Linux_Kernel_%28ARCH%3Dpowerpc%29
#
