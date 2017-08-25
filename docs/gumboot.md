# Gumboot

<a href="https://neagix.github.io/wii-linux-ngx/"><img alt="Gumboot" src="gumboot-logo.png" width="140" title="gumboot" /></a>

Gumboot is a configurable bootloader for Wii, aimed at running Linux with customizable kernel command line without the need of recompiling for every change.

It is released in ELF format with the wii-linux-ngx project:

* [https://github.com/neagix/wii-linux-ngx/releases](https://github.com/neagix/wii-linux-ngx/releases)

# Features

* GRUB legacy colors, CSS-style rgb() triples colors
* timeout and default entry
* customizable kernel command-line arguments
* FAT16/FAT32 directory browsing and launching of ELF files
* all 4 primary partitions
* reboot/poweroff
* boot any ELF (including Bootmii)

# Frequently Asked Questions

<dl>
<dt>Does it support multiple partitions?</dt>
<dd>Yes, all primary partitions in FAT16 and FAT32 format are supported.</dd>
<dt>Does it support extended partitions?</dt>
<dd>No.</dd>
<dt>Does it support long filenames? exFAT?</dt>
<dd>No, the middleware has broken support for them.</dd>
<dt>Does it support splash images?</dt>
<dd>No, it's a work in progress.</dd>
</dl>

# Screenshot

This is a screenshot of the default `gumboot.lst` menu:

<img alt="Gumboot default menu" src="gumboot-default.png" title="Gumboot default menu" />

# Thanks

Thanks to the following people/organizations/groups:
* the Linux Kernel developers
* the GC-Linux team
* DeltaResero
* marcan
