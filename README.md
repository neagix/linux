# A modern Linux kernel for Wii

![Tux + Wii](docs/tux-wii.png)

https://neagix.github.io/wii-linux-ngx/

The [wii-linux-ngx repository](https://github.com/neagix/wii-linux-ngx) contains Linux kernel branches with rebased patches for the purpose of running a modern Linux distribution on the Wii.

Up-to-date documentation and scripts can always be found on the [master branch](https://github.com/neagix/wii-linux-ngx/tree/master).

Feel free to open Issues/Pull requests for improvement/discussion purposes.

## How it works

### Wii

wii-linux-ngx works with an SD card (or USB mass storage) with the following layout:
* first partition, FAT16 with MINI and Bootmii and main/fallback bootloader, provided here as well for ease of use
* second partition with ext3 Linux rootfs

You can add other partitions at your choice; performance of SD cards is better than USB mass storage.

You can use Priiloader to make Bootmii your default choice, effectively creating this chain:

```
Wii power on -> MINI -> (Bootmii selection with power/eject buttons ->) Linux kernel zImage
```

The BootMii step is optional, but there is currently [an open bug that prevents video from correctly working](https://github.com/neagix/wii-linux-ngx/issues/2) (please report if it works for you instead).
Summary:

* if you wish to boot into Bootmii GUI, make sure `/bootmii/zImage` is renamed to something else like `zImage.ngx` (default)
* if you wish to boot directly into Linux kernel, put your kernel in `/bootmii/zImage`

This customized mini is available at: https://github.com/neagix/mini

### GameCube

Currently not tested on GameCube.

### vWii

Currently not tested on virtual Wii.

## Default credentials

The SD image and rootfs have `root:root` credentials.

If you prefer to login via USB serial console, edit the last line of `/etc/inittab` in order to use `ttyUSB0` as a login terminal; you might need to recompile the kernel with your serial-over-USB driver as only the PL2303 driver is included.

## History

Chronological history of Linux for Wii/GameCube:

* [gc-linux (v2.6-based) MIKEp5](http://www.gc-linux.org/wiki/MINI:KernelPreviewFive) - this is the original project and corresponds to the bulk work done to bring Linux to the Wii
* [DeltaResero's fork (unofficial MIKEp7](https://github.com/DeltaResero/GC-Wii-Linux-Kernels) - considerable work done by DeltaResero to bring up the GC/Wii patches into a v3.x kernel
* [this project, wii-linux-ngx](https://github.com/neagix/wii-linux-ngx) - continuation of the previous work, distribution packaging and maintenance

The original (2.6.32 and prior) gcLinux work can be found at: http://sourceforge.net/projects/gc-linux/; at the time of writing project has not seen activity since 2013.

### Status

A few branches are currently maintained:
* [stable-v3.x](https://github.com/neagix/linux/tree/stable-v3.x), latest working v3.x kernel with most up to date upstream patches
* [experimental-v3.x](https://github.com/neagix/linux/tree/experimental-v3.x), some features might be broken (SDHC)
* [experimental-v4.x](https://github.com/neagix/linux/tree/experimental-v4.x), some features might be broken (SDHC)
* [rebased-deltares-v3.x](https://github.com/neagix/linux/tree/rebased-deltares-v3.x), original v3.12.11 by DeltaResero rebased for easier merges + v3.12.12 merge
* [stable-v3.x-w-extras](https://github.com/neagix/linux/tree/stable-v3.x-w-extras), features added by DeltaRes but not in stable-v3.x can be found in this branch.

Farter's Deferred I/O Framebuffer patch (http://fartersoft.com/) is included in stable-v3.x. Some of the dropped features (easy to re-add through cherry-pick) are:
* Nold360's GameCube SDHC support (http://www.gc-forever.com/forums/portal.php)  
    - (https://github.com/Nold360/GC-Linux-Kernel-2.6.32/commits/master)<br>  

Experimental branches:
* broken-v4.x (v4.12.5) seems to panic regardless of SDHC enabled or not; probably due to the DMA coherency changes

## Known issues

Boot from MINI is well tested, but not boot from IOS.

Bugs probably introduced in the port of MIKEp5 from v2.6 to v3.x tree:
* In IOS mode, external swap partitions don't mount correctly as of kernel version 2.6.39. As a workaround, use a local swapfile (This bug should be relatively easy to find using git bisect)
* Both IOS and MINI modes seem to have a bug that prevents Linux from booting if a GameCube Controller is inserted in one of the ports while the serial port is enabled in the config.  This bug is caused by a glitch that was created when forward porting from 2.6.32 to 2.6.33.  It should be possible to find this bug using git bisect.
* Only Cube Xorg or Farter's Framebuffer can be used, not both at the same time.
* Both IOS and MINI also still suffer from the same hardware limitations that they did in 2.6.32.y.  For example, wireless and disc support for Wii consoles is still limited to MINI mode.  Also, DVDs can be mounted as they were in version 2.6.32.y, but due to hardware limitations, it's unable to write to any disc and is unable to read CDs and certain types of DVD's
    - Support for DVD-RW and DVD-DL disc seems to vary.  Currently, -R and +R (both mini & full-size) DVDs are know to work on both GameCube and Wii consoles.
    All WiiU as well as some of the newer Wii disc drives, lack support for DVDs as they don't contain the same type of disc drive.
    In other words, support will vary on the age of the console, but most standard GameCube consoles should be able to read mini DVDs (full-sized DVDs are too big for unmodified Gamecube consoles, but they can be read).
    
See [open issues](https://github.com/neagix/wii-linux-ngx/issues).

## Changing bootargs with baedit

It is possible to change kernel command line arguments (also known as `bootargs` from the DTS file) with a hex editor, with (very careful) usage of `sed`, or with the provided `baedit` tool.

To show current bootargs embedded in the kernel:
```
$ baedit zImage
>OK: 3201336 bytes read
current  bootargs = 'root=/dev/mmcblk0p2 console=tty0 console=ttyUSB0,115200 force_keyboard_port=4 video=gcnfb:tv=auto loader=mini nobats rootwait       
```

To change the arguments, just pass them as second parameter to `baedit`:
```
$ baedit zImage 'your new arguments here'
>OK: 3201336 bytes read
current  bootargs = 'root=/dev/mmcblk0p2 console=tty0 console=ttyUSB0,115200 force_keyboard_port=4 video=gcnfb:tv=auto loader=mini nobats rootwait       
replaced bootargs = 'your new arguments here                                                                                                                              '
>OK: 3201336 bytes written
```

# Connecting to Wi-Fi

The `whiite-ez-wifi-config` script is included in `/root` to easily connect to a Wi-Fi network.

# Swap

It is suggested to create a swap partition and enable it to speed up operations, since the Wii has little memory available (~80M).

# Installing packages

The Jessie rootfs is stripped down so you will need to run `apt-get update` before being able to install any package.

# Building the kernel

Compiling this kernel will has some dependencies that must be installed.
On a Debian-based system, these dependecies can be installed by running the following command:

    sudo apt-get install advancecomp autoconfig automake bash build-essential bzip2 ccache  coreutils fakeroot file gcc g++ gzip libmpfr-dev libgmp-dev libnurses5-dev make strip

**TODO:** add here instructions about the new building script

## (Cross) Compiling the Kernel

Remember to edit the corresponding dts file (`arch/powerpc/boot/wii.dts`  for the Wii when not using the default boot method from SD card (`/dev/mmcblk0p2`).
Enabling zcache and other kernel options requres editing the default bootargs.
You can alter the bootargs of an already compiled kernel by using the provided tool `baedit` (it's found in the rootfs image or you can run/build it with Go).

Related pages:
* http://www.gc-linux.org/wiki/Building_a_GameCube_Linux_Kernel_%28ARCH%3Dpowerpc%29

## ZRAM

The zram-config v0.5 files are provided in `/root/zram-config`, there is no init integration though.

You can verify whether zRam ha started by running the following command as root:

        swapon -s

zRam swapping can be turned off with the following command:
	
	swapoff /dev/zram0
        
Change the command accordingly to which device should be stopped.

Related readings:
* how to start zRam: http://forums.debian.net/viewtopic.php?t=77627  
* http://gionn.net/2012/03/11/zram-on-debian-ubuntu-for-memory-overcommitment/

## Mounting a disc

Create a "dvd" folder (as root) in the "/media" directory (only if the folder doesn't exist) with the command:

        mkdir /media/dvd

Then run the following (also as root):

        mount /dev/rvl-di /media/dvd

DVDs can be inserted/switched anytime but should be unmount prior to ejecting and then remount again after to prevent errors.  To unmount a disc, enter the following command as root:

        umount /dev/rvl-di /media/dvd

Additional packages such as libdvdcss & libdvdread may need to be installed for DVD playblack (may need to search package manager as naming standards aren't consistant).  Mplayer and Xine seem to work the best but support will vary depending on the operating system.  

## Related pages:
* http://fartersoft.com/blog/2011/08/17/debian-installer-for-wii/

## Old network troubleshooting resources

* http://www.linux-tips-and-tricks.de/overview#english
* http://www.linux-tips-and-tricks.de/downloads/collectnwdata-sh/download
* http://www.gc-linux.org/wiki/WL:Wifi_Configuration
* http://forum.wiibrew.org/read.php?29,68339,68339
