# Linux kernel for GameCube/Wii/vWii

Compiling this kernel will has some dependencies that must be installed.  On a Debian based system, these dependecies can be installed by running the following command:

    sudo apt-get install advancecomp autoconfig automake bash build-essential bzip2 ccache  coreutils fakeroot file gcc g++ gzip libmpfr-dev libgmp-dev libnurses5-dev make strip

## History fo the DeltaRasero fork

The original (2.6.32 and prior) gcLinux work can be found at: http://sourceforge.net/projects/gc-linux/
This GC/Wii Linux kernel, as with all following the official 2.6.32.y version, are forward ports of multiple patches that include (but are not limited to) the following:  

1. A modified MINI Kernel Preview 5 patch (http://www.gc-linux.org/wiki/MINI:KernelPreviewFive) with most mainline improvements integrated.
    To clarify, DeltaRasero didn't write any of the original code, just updated and merged existing code from various sources.
    The existing code was altered so it would build against the newer 3.x based kernels.  I dubbed the changed patch as "MINI Kernel Preview 7".
2. Farter's Deferred I/O Framebuffer patch (http://fartersoft.com/)  
3. Nold360's GameCube SDHC support (http://www.gc-forever.com/forums/portal.php)  
    - (https://github.com/Nold360/GC-Linux-Kernel-2.6.32/commits/master)<br>  
4.  Additional default (defconfig) configurations (located in: "arch/powerpc/configs/")  
    - (Currently: "gamecube_smaller", "wii-ios-mode", and "wii-mini-mode")<br>  
5.  Various newer features backported from the mainline Linux kernel.

## neagix forks (this repository)

A few branches are currently maintained:
* [original v3.12.11 by DeltaRasero](https://github.com/neagix/linux/tree/rebased-deltares-v3.x), rebased for easier merges + v3.12.12 merge
* [stable-v3.x](https://github.com/neagix/linux/tree/stable-v3.x), latest working v3.x kernel with most up to date upstream patches
* [experimental-v3.x](https://github.com/neagix/linux/tree/experimental-v3.x), non-working v3.x merged with upstream tip

# Known issues

Due to significant changes since the last official kernel patch release gcLinux, the 2.6.32 (MIKEp5) patch, the MIKEp7 patch currently has some limitations and bugs that still require attention.  Some of these limitations are as follows:  

- In MINI mode, older (smaller than 2GB) cards don't all work correctly  

- In IOS mode, external swap partitions don't mount correctly as of kernel version 2.6.39.  As a workaround, use a local swapfile.  A short tutorial can be found at: http://www.cyberciti.biz/faq/linux-add-a-swap-file-howto/
This bug should be relatively easy to find using git bisect.  

- Both IOS and MINI modes seem to have a bug that prevents Linux from booting if a GameCube Controller is inserted in one of the ports while the serial port is enabled in the config.  This bug is caused by a glitch that was created when forward porting from 2.6.32 to 2.6.33.  It should be possible to find this bug using git bisect.

- Only Cube Xorg or Farter's Framebuffer can be used, not both at the same time.
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


# Linux README
```

        Linux kernel release 3.x <http://kernel.org/>

These are the release notes for Linux version 3.  Read them carefully,
as they tell you what this is all about, explain how to install the
kernel, and what to do if something goes wrong. 

WHAT IS LINUX?

  Linux is a clone of the operating system Unix, written from scratch by
  Linus Torvalds with assistance from a loosely-knit team of hackers across
  the Net. It aims towards POSIX and Single UNIX Specification compliance.

  It has all the features you would expect in a modern fully-fledged Unix,
  including true multitasking, virtual memory, shared libraries, demand
  loading, shared copy-on-write executables, proper memory management,
  and multistack networking including IPv4 and IPv6.

  It is distributed under the GNU General Public License - see the
  accompanying COPYING file for more details. 

ON WHAT HARDWARE DOES IT RUN?

  Although originally developed first for 32-bit x86-based PCs (386 or higher),
  today Linux also runs on (at least) the Compaq Alpha AXP, Sun SPARC and
  UltraSPARC, Motorola 68000, PowerPC, PowerPC64, ARM, Hitachi SuperH, Cell,
  IBM S/390, MIPS, HP PA-RISC, Intel IA-64, DEC VAX, AMD x86-64, AXIS CRIS,
  Xtensa, Tilera TILE, AVR32 and Renesas M32R architectures.

  Linux is easily portable to most general-purpose 32- or 64-bit architectures
  as long as they have a paged memory management unit (PMMU) and a port of the
  GNU C compiler (gcc) (part of The GNU Compiler Collection, GCC). Linux has
  also been ported to a number of architectures without a PMMU, although
  functionality is then obviously somewhat limited.
  Linux has also been ported to itself. You can now run the kernel as a
  userspace application - this is called UserMode Linux (UML).

DOCUMENTATION:

 - There is a lot of documentation available both in electronic form on
   the Internet and in books, both Linux-specific and pertaining to
   general UNIX questions.  I'd recommend looking into the documentation
   subdirectories on any Linux FTP site for the LDP (Linux Documentation
   Project) books.  This README is not meant to be documentation on the
   system: there are much better sources available.

 - There are various README files in the Documentation/ subdirectory:
   these typically contain kernel-specific installation notes for some 
   drivers for example. See Documentation/00-INDEX for a list of what
   is contained in each file.  Please read the Changes file, as it
   contains information about the problems, which may result by upgrading
   your kernel.

 - The Documentation/DocBook/ subdirectory contains several guides for
   kernel developers and users.  These guides can be rendered in a
   number of formats:  PostScript (.ps), PDF, HTML, & man-pages, among others.
   After installation, "make psdocs", "make pdfdocs", "make htmldocs",
   or "make mandocs" will render the documentation in the requested format.

INSTALLING the kernel source:

 - If you install the full sources, put the kernel tarball in a
   directory where you have permissions (eg. your home directory) and
   unpack it:

     gzip -cd linux-3.X.tar.gz | tar xvf -

   or

     bzip2 -dc linux-3.X.tar.bz2 | tar xvf -

   Replace "X" with the version number of the latest kernel.

   Do NOT use the /usr/src/linux area! This area has a (usually
   incomplete) set of kernel headers that are used by the library header
   files.  They should match the library, and not get messed up by
   whatever the kernel-du-jour happens to be.

 - You can also upgrade between 3.x releases by patching.  Patches are
   distributed in the traditional gzip and the newer bzip2 format.  To
   install by patching, get all the newer patch files, enter the
   top level directory of the kernel source (linux-3.X) and execute:

     gzip -cd ../patch-3.x.gz | patch -p1

   or

     bzip2 -dc ../patch-3.x.bz2 | patch -p1

   Replace "x" for all versions bigger than the version "X" of your current
   source tree, _in_order_, and you should be ok.  You may want to remove
   the backup files (some-file-name~ or some-file-name.orig), and make sure
   that there are no failed patches (some-file-name# or some-file-name.rej).
   If there are, either you or I have made a mistake.

   Unlike patches for the 3.x kernels, patches for the 3.x.y kernels
   (also known as the -stable kernels) are not incremental but instead apply
   directly to the base 3.x kernel.  For example, if your base kernel is 3.0
   and you want to apply the 3.0.3 patch, you must not first apply the 3.0.1
   and 3.0.2 patches. Similarly, if you are running kernel version 3.0.2 and
   want to jump to 3.0.3, you must first reverse the 3.0.2 patch (that is,
   patch -R) _before_ applying the 3.0.3 patch. You can read more on this in
   Documentation/applying-patches.txt

   Alternatively, the script patch-kernel can be used to automate this
   process.  It determines the current kernel version and applies any
   patches found.

     linux/scripts/patch-kernel linux

   The first argument in the command above is the location of the
   kernel source.  Patches are applied from the current directory, but
   an alternative directory can be specified as the second argument.

 - Make sure you have no stale .o files and dependencies lying around:

     cd linux
     make mrproper

   You should now have the sources correctly installed.

SOFTWARE REQUIREMENTS

   Compiling and running the 3.x kernels requires up-to-date
   versions of various software packages.  Consult
   Documentation/Changes for the minimum version numbers required
   and how to get updates for these packages.  Beware that using
   excessively old versions of these packages can cause indirect
   errors that are very difficult to track down, so don't assume that
   you can just update packages when obvious problems arise during
   build or operation.

BUILD directory for the kernel:

   When compiling the kernel, all output files will per default be
   stored together with the kernel source code.
   Using the option "make O=output/dir" allow you to specify an alternate
   place for the output files (including .config).
   Example:

     kernel source code: /usr/src/linux-3.X
     build directory:    /home/name/build/kernel

   To configure and build the kernel, use:

     cd /usr/src/linux-3.X
     make O=/home/name/build/kernel menuconfig
     make O=/home/name/build/kernel
     sudo make O=/home/name/build/kernel modules_install install

   Please note: If the 'O=output/dir' option is used, then it must be
   used for all invocations of make.

CONFIGURING the kernel:

   Do not skip this step even if you are only upgrading one minor
   version.  New configuration options are added in each release, and
   odd problems will turn up if the configuration files are not set up
   as expected.  If you want to carry your existing configuration to a
   new version with minimal work, use "make oldconfig", which will
   only ask you for the answers to new questions.

 - Alternative configuration commands are:

     "make config"      Plain text interface.

     "make menuconfig"  Text based color menus, radiolists & dialogs.

     "make nconfig"     Enhanced text based color menus.

     "make xconfig"     X windows (Qt) based configuration tool.

     "make gconfig"     X windows (Gtk) based configuration tool.

     "make oldconfig"   Default all questions based on the contents of
                        your existing ./.config file and asking about
                        new config symbols.

     "make silentoldconfig"
                        Like above, but avoids cluttering the screen
                        with questions already answered.
                        Additionally updates the dependencies.

     "make olddefconfig"
                        Like above, but sets new symbols to their default
                        values without prompting.

     "make defconfig"   Create a ./.config file by using the default
                        symbol values from either arch/$ARCH/defconfig
                        or arch/$ARCH/configs/${PLATFORM}_defconfig,
                        depending on the architecture.

     "make ${PLATFORM}_defconfig"
                        Create a ./.config file by using the default
                        symbol values from
                        arch/$ARCH/configs/${PLATFORM}_defconfig.
                        Use "make help" to get a list of all available
                        platforms of your architecture.

     "make allyesconfig"
                        Create a ./.config file by setting symbol
                        values to 'y' as much as possible.

     "make allmodconfig"
                        Create a ./.config file by setting symbol
                        values to 'm' as much as possible.

     "make allnoconfig" Create a ./.config file by setting symbol
                        values to 'n' as much as possible.

     "make randconfig"  Create a ./.config file by setting symbol
                        values to random values.

     "make localmodconfig" Create a config based on current config and
                           loaded modules (lsmod). Disables any module
                           option that is not needed for the loaded modules.

                           To create a localmodconfig for another machine,
                           store the lsmod of that machine into a file
                           and pass it in as a LSMOD parameter.

                   target$ lsmod > /tmp/mylsmod
                   target$ scp /tmp/mylsmod host:/tmp

                   host$ make LSMOD=/tmp/mylsmod localmodconfig

                           The above also works when cross compiling.

     "make localyesconfig" Similar to localmodconfig, except it will convert
                           all module options to built in (=y) options.

   You can find more information on using the Linux kernel config tools
   in Documentation/kbuild/kconfig.txt.

 - NOTES on "make config":

    - Having unnecessary drivers will make the kernel bigger, and can
      under some circumstances lead to problems: probing for a
      nonexistent controller card may confuse your other controllers

    - Compiling the kernel with "Processor type" set higher than 386
      will result in a kernel that does NOT work on a 386.  The
      kernel will detect this on bootup, and give up.

    - A kernel with math-emulation compiled in will still use the
      coprocessor if one is present: the math emulation will just
      never get used in that case.  The kernel will be slightly larger,
      but will work on different machines regardless of whether they
      have a math coprocessor or not.

    - The "kernel hacking" configuration details usually result in a
      bigger or slower kernel (or both), and can even make the kernel
      less stable by configuring some routines to actively try to
      break bad code to find kernel problems (kmalloc()).  Thus you
      should probably answer 'n' to the questions for "development",
      "experimental", or "debugging" features.

COMPILING the kernel:

 - Make sure you have at least gcc 3.2 available.
   For more information, refer to Documentation/Changes.

   Please note that you can still run a.out user programs with this kernel.

 - Do a "make" to create a compressed kernel image. It is also
   possible to do "make install" if you have lilo installed to suit the
   kernel makefiles, but you may want to check your particular lilo setup first.

   To do the actual install, you have to be root, but none of the normal
   build should require that. Don't take the name of root in vain.

 - If you configured any of the parts of the kernel as `modules', you
   will also have to do "make modules_install".

 - Verbose kernel compile/build output:

   Normally, the kernel build system runs in a fairly quiet mode (but not
   totally silent).  However, sometimes you or other kernel developers need
   to see compile, link, or other commands exactly as they are executed.
   For this, use "verbose" build mode.  This is done by inserting
   "V=1" in the "make" command.  E.g.:

     make V=1 all

   To have the build system also tell the reason for the rebuild of each
   target, use "V=2".  The default is "V=0".

 - Keep a backup kernel handy in case something goes wrong.  This is 
   especially true for the development releases, since each new release
   contains new code which has not been debugged.  Make sure you keep a
   backup of the modules corresponding to that kernel, as well.  If you
   are installing a new kernel with the same version number as your
   working kernel, make a backup of your modules directory before you
   do a "make modules_install".

   Alternatively, before compiling, use the kernel config option
   "LOCALVERSION" to append a unique suffix to the regular kernel version.
   LOCALVERSION can be set in the "General Setup" menu.

 - In order to boot your new kernel, you'll need to copy the kernel
   image (e.g. .../linux/arch/i386/boot/bzImage after compilation)
   to the place where your regular bootable kernel is found. 

 - Booting a kernel directly from a floppy without the assistance of a
   bootloader such as LILO, is no longer supported.

   If you boot Linux from the hard drive, chances are you use LILO, which
   uses the kernel image as specified in the file /etc/lilo.conf.  The
   kernel image file is usually /vmlinuz, /boot/vmlinuz, /bzImage or
   /boot/bzImage.  To use the new kernel, save a copy of the old image
   and copy the new image over the old one.  Then, you MUST RERUN LILO
   to update the loading map!! If you don't, you won't be able to boot
   the new kernel image.

   Reinstalling LILO is usually a matter of running /sbin/lilo. 
   You may wish to edit /etc/lilo.conf to specify an entry for your
   old kernel image (say, /vmlinux.old) in case the new one does not
   work.  See the LILO docs for more information. 

   After reinstalling LILO, you should be all set.  Shutdown the system,
   reboot, and enjoy!

   If you ever need to change the default root device, video mode,
   ramdisk size, etc.  in the kernel image, use the 'rdev' program (or
   alternatively the LILO boot options when appropriate).  No need to
   recompile the kernel to change these parameters. 

 - Reboot with the new kernel and enjoy. 

IF SOMETHING GOES WRONG:

 - If you have problems that seem to be due to kernel bugs, please check
   the file MAINTAINERS to see if there is a particular person associated
   with the part of the kernel that you are having trouble with. If there
   isn't anyone listed there, then the second best thing is to mail
   them to me (torvalds@linux-foundation.org), and possibly to any other
   relevant mailing-list or to the newsgroup.

 - In all bug-reports, *please* tell what kernel you are talking about,
   how to duplicate the problem, and what your setup is (use your common
   sense).  If the problem is new, tell me so, and if the problem is
   old, please try to tell me when you first noticed it.

 - If the bug results in a message like

     unable to handle kernel paging request at address C0000010
     Oops: 0002
     EIP:   0010:XXXXXXXX
     eax: xxxxxxxx   ebx: xxxxxxxx   ecx: xxxxxxxx   edx: xxxxxxxx
     esi: xxxxxxxx   edi: xxxxxxxx   ebp: xxxxxxxx
     ds: xxxx  es: xxxx  fs: xxxx  gs: xxxx
     Pid: xx, process nr: xx
     xx xx xx xx xx xx xx xx xx xx

   or similar kernel debugging information on your screen or in your
   system log, please duplicate it *exactly*.  The dump may look
   incomprehensible to you, but it does contain information that may
   help debugging the problem.  The text above the dump is also
   important: it tells something about why the kernel dumped code (in
   the above example, it's due to a bad kernel pointer). More information
   on making sense of the dump is in Documentation/oops-tracing.txt

 - If you compiled the kernel with CONFIG_KALLSYMS you can send the dump
   as is, otherwise you will have to use the "ksymoops" program to make
   sense of the dump (but compiling with CONFIG_KALLSYMS is usually preferred).
   This utility can be downloaded from
   ftp://ftp.<country>.kernel.org/pub/linux/utils/kernel/ksymoops/ .
   Alternatively, you can do the dump lookup by hand:

 - In debugging dumps like the above, it helps enormously if you can
   look up what the EIP value means.  The hex value as such doesn't help
   me or anybody else very much: it will depend on your particular
   kernel setup.  What you should do is take the hex value from the EIP
   line (ignore the "0010:"), and look it up in the kernel namelist to
   see which kernel function contains the offending address.

   To find out the kernel function name, you'll need to find the system
   binary associated with the kernel that exhibited the symptom.  This is
   the file 'linux/vmlinux'.  To extract the namelist and match it against
   the EIP from the kernel crash, do:

     nm vmlinux | sort | less

   This will give you a list of kernel addresses sorted in ascending
   order, from which it is simple to find the function that contains the
   offending address.  Note that the address given by the kernel
   debugging messages will not necessarily match exactly with the
   function addresses (in fact, that is very unlikely), so you can't
   just 'grep' the list: the list will, however, give you the starting
   point of each kernel function, so by looking for the function that
   has a starting address lower than the one you are searching for but
   is followed by a function with a higher address you will find the one
   you want.  In fact, it may be a good idea to include a bit of
   "context" in your problem report, giving a few lines around the
   interesting one. 

   If you for some reason cannot do the above (you have a pre-compiled
   kernel image or similar), telling me as much about your setup as
   possible will help.  Please read the REPORTING-BUGS document for details.

 - Alternatively, you can use gdb on a running kernel. (read-only; i.e. you
   cannot change values or set break points.) To do this, first compile the
   kernel with -g; edit arch/i386/Makefile appropriately, then do a "make
   clean". You'll also need to enable CONFIG_PROC_FS (via "make config").

   After you've rebooted with the new kernel, do "gdb vmlinux /proc/kcore".
   You can now use all the usual gdb commands. The command to look up the
   point where your system crashed is "l *0xXXXXXXXX". (Replace the XXXes
   with the EIP value.)

   gdb'ing a non-running kernel currently fails because gdb (wrongly)
   disregards the starting offset for which the kernel is compiled.
```
