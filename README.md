***
**_Linux kernel for GameCube/Wii/vWii (Branch Version: 3.12.11)_**
***
NOTE: Well, this worked at some point. Going to have to track which patch broke this and 3.10.y
This is the 3.12.y GC/Wii Linux kernel branch.  A full copy of this repository can be downloaded by using git to clone the full repository as shown below.

    git clone https://github.com/DeltaResero/GC-Wii-Linux-Kernels.git

To clone only a standalone copy of this branch, either download directly from Github from within the matching branch or releases section, or clone the branch with the following command (requires Git version 1.8.X or greater):

    git clone -b GC-Wii-Linux-Kernel-3.12.y --single-branch https://github.com/DeltaResero/GC-Wii-Linux-Kernels.git
<br>
For those who are using a version of Git prior to 1.8.X on Debian/Ubuntu based operating systems should able to easily update to at least an 1.8.X version via ppa.  The alternative is to compile Git from source (https://github.com/git/git).  By default any Linux operating system prior to Debian 7 and Ubuntu 13.10 use a version of Git that will require updating.  For Debian based systems, use the following commands to update Git (assuming git is already installed):

    sudo add-apt-repository ppa:pdoes/ppa
    sudo apt-get update
    sudo apt-get install git

Check which version of git is installed with the command: "git --version".  
<br>

Compiling this kernel will has some dependencies that must be installed.  On a Debian based system, these dependecies can be installed by running the following command:

    sudo apt-get install advancecomp autoconfig automake bash build-essential bzip2 ccache  coreutils fakeroot file gcc g++ gzip libmpfr-dev libgmp-dev libnurses5-dev make strip

<br>
***
**_About the Kernel_**  
***

The original (2.6.32 and prior) gcLinux work can be found at: http://sourceforge.net/projects/gc-linux/
This GC/Wii Linux kernel, as with all following the official 2.6.32.y version, are forward ports of multiple patches that include (but are not limited to) the following:  

1. A modified MINI Kernel Preview 5 patch (http://www.gc-linux.org/wiki/MINI:KernelPreviewFive) with most mainline improvements integrated.
    - To clarify, I didn't write any of the original code, I just updated and merged existing code from various sources.  The existing code was altered so it would build against the newer 3.x based kernels.  I dubbed the changed patch as "MINI Kernel Preview 7".  I decided to reserve version 6 for the work that was done in the "origin/cocktail/gc-linux-2.6.34-rc5" branch of the gcLinux Git repsoitory (git://git.infradead.org/users/herraa1/gc-linux-2.6.git).<br>  
2. Farter's Deferred I/O Framebuffer patch (http://fartersoft.com/)  
    - Apply this diff to get correct color output if not using the Xorg Cube driver.<br>  
3. Nold360's GameCube SDHC support (http://www.gc-forever.com/forums/portal.php)  
    - (https://github.com/Nold360/GC-Linux-Kernel-2.6.32/commits/master)<br>  
4.  Additional default (defconfig) configurations (located in: "arch/powerpc/configs/")  
    - (Currently: "gamecube_smaller", "wii-ios-mode", and "wii-mini-mode")<br>  
5.  Various newer features backported from the mainline Linux kernel.  

<br>

***
**_Known Bugs_**
***

Due to significant changes since the last official kernel patch release gcLinux, the 2.6.32 (MIKEp5) patch, the MIKEp7 patch currently has some limitations and bugs that still require attention.  Some of these limitations are as follows:  

- In MINI mode, older (smaller than 2GB) cards don't all work correctly  

- In IOS mode, external swap partitions don't mount correctly as of kernel version 2.6.39.  As a workaround, use a local swapfile.  A short tutorial can be found at: http://www.cyberciti.biz/faq/linux-add-a-swap-file-howto/
This bug should be relatively easy to find using git bisect.  

- Both IOS and MINI modes seem to have a bug that prevents Linux from booting if a GameCube Controller is inserted in one of the ports while the serial port is enabled in the config.  This bug is caused by a glitch that was created when forward porting from 2.6.32 to 2.6.33.  It should be possible to find this bug using git bisect.

- Only Cube Xorg or Farter's Framebuffer can be used, not both at the same time.  If Xorg is setup to use Cube on the target system, do not use a kernel that was compiled with Farter's framebuffer patch without adjusting the Xorg configuration file (usually in: /etc/X11/xorg.conf).  Using both simultaneously will cause the display to show nothing at best.  Due to this, I've made Farter's framebuffer patch optional by leaving a copy of it seperate in the source so that it could be patched manually at anytime.
<br>

    To patch, enter the command:

        "patch < 0001-vfb-defio-gc-wii.diff -p1"

    To remove the patch, enter the command:

        "patch < 0001-vfb-defio-gc-wii.diff -p1 -R"  

- Both IOS and MINI also still suffer from the same hardware limitations that they did in 2.6.32.y.  For example, wireless and disc support for Wii consoles is still limited to MINI mode.  Also, DVDs can be mounted as they were in version 2.6.32.y, but due to hardware limitations, it's unable to write to any disc and is unable to read CDs and certain types of DVD's
    - Support for DVD-RW and DVD-DL disc seems to vary.  Currently, -R and +R (both mini & full-size) DVDs are know to work on both GameCube and Wii consoles.  All WiiU as well as some of the newer Wii disc drives, lack support for DVDs as they don't contain the same type of disc drive.  In other words, support will vary on the age of the console, but most standard GameCube consoles should be able to read mini DVDs (full-sized DVDs are too big for unmodified Gamecube consoles, but they can be read).
    - To mount a disc in a GameCube/Wii Linux distribution, try doing the following:

<br>

Create a "dvd" folder (as root) in the "/media" directory (only if the folder doesn't exist) with the command:

        mkdir /media/dvd

Then run the following (also as root):

        mount /dev/rvl-di /media/dvd

DVDs can be inserted/switched anytime but should be unmount prior to ejecting and then remount again after to prevent errors.  To unmount a disc, enter the following command as root:

        umount /dev/rvl-di /media/dvd

Additional packages such as libdvdcss & libdvdread may need to be installed for DVD playblack (may need to search package manager as naming standards aren't consistant).  Mplayer and Xine seem to work the best but support will vary depending on the operating system.  
<br>

***
**_General Notices:_**  
***

_(Cross) Compiling the Kernel:_  

- Remember to edit the corresponding dts file (arch/powerpc/boot/)when not using the default boot method.  Also, enabling zcache requres editing the dts dts bootargs.  See this for more info: https://bugs.archlinux.org/task/27267

<br>

- A basic shell script was created to help in building the Kernel and should work on i386, X86_64, and PowerPC platforms.  To run this script, open a terminal and run (either "sh " or "./" followed by the name of the script):



        ./build-gc-wii-kernel.sh

    A basic menu should show if this script starts successfully.  The script & kernel both have a few dependencies (such as GNU Make) that are briefly mentioned near the top of this readme.
<br>

- For other basic (cross) compiling methods, see the following Wiki webpage at:  
http://www.gc-linux.org/wiki/Building_a_GameCube_Linux_Kernel_%28ARCH%3Dpowerpc%29
<br>

- More info on how to start zRam can be found here: http://forums.debian.net/viewtopic.php?t=77627  
- For simplicity, it's usually best to install the zram-config package in Ubuntu based distributions or add the script manually in other distributions such as Debian.  More information can be found here:  http://gionn.net/2012/03/11/zram-on-debian-ubuntu-for-memory-overcommitment/
<br>

- Verifying whether zRam has started can be done with the following command as root:



        swapon -s

- Turn zRam swapping off can be done with the following command as root: 
 
 

        "swapoff /dev/zram#"
        
    Change the # to the device number that swap should be stopped (typically # = 0)  

_Verifying Zcache Status:_  
Once the system boots, run "dmesg | grep zcache" to see its status (If nothing is shown Zcache is likely disabled)  
<br> 

**_Compiled Filesystem/Kernel Demos_**  
	Wii IOS/MINI kernels (3.12.?):  SERVER_ERROR...  
	Debian:  https://spideroak.com/browse/share/DeltaResero/wii/Linux/Filesytems/Demos/Debian/
<br>
	Ubuntu:  PLANNING...
<br>
	Mint:  PLANNING...
<br>
- NOTE: Debian Sid/Jessie is not currently recommended, it's recommended to use  (stable) Wheezy or (old stable) Squeeze.  

**_Fully customized (more complete) desktop distributions images:_**  
	Debian Wheezy: PENDING...... (still UTC clock bugs)
<br>
	Debian Jessie: NOT READY YET (waiting for stability)
<br>
  Mint: PLANNING... (won't be ready until after Debian Wheezy) release
<br>  
	Ubuntu (ICEWM): NOT READY YET... (13.10 is completely unstable [non-kernel related])  
<br>

- Once the disk image is download and extracted, it can be copied to devices (such as SD cards) in Linux with the command:

        sudo dd if=4gbWheezyWii.img of=/dev/sdx#
        
Where x is the drive mount point letter (typically "b" or "c") and # is the partition number (usually "2").  **Be careful**, using the wrong sdx# device **may result in lost data**.
  (Replace "4gbWheezyWii.img" with name of the actual disc image if not using this one)  


The above links and Debian disk images use the following credentials:  
Username: wii  
Password: delta  
*Root password is the same (delta)  

For those who are looking to create their own Debian setup and perfer an interactive GUI, take a look at Farter's blog in the link below:

		http://fartersoft.com/blog/2011/08/17/debian-installer-for-wii/

For those who are having networking issues, take a look the links below for more help:

		http://www.linux-tips-and-tricks.de/overview#english
		http://www.linux-tips-and-tricks.de/downloads/collectnwdata-sh/download
		http://www.gc-linux.org/wiki/WL:Wifi_Configuration
		http://forum.wiibrew.org/read.php?29,68339,68339
<br>

_Notice(s):_  
- Reportedly most (if not all) GC/Wii Linux kernels (partially) work on WiiU in virtual Wii mode with proper setups.  Virtual Wii mode lacks support for non Wii hardware, Bluetooth, wireless, disc drive.  As with Wii consoles, currently USB support is currently broken.  For more information, see: http://gbatemp.net/threads/vwii-tri-core-linux.351024 and http://code.google.com/p/gbadev/
