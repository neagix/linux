# wii-linux-ngx

![Tux + Wii]({{ site.url }}/wii-linux-ngx/tux-wii.png)

Running modern Linux on Wii consoles.

This project provides:
* source code and scripts to compile a working Linux kernel for GC/Wii, in the form of git branches (stable Linux trees + necessary patches)
* kernel single-binary releases in ELF format (zImage)
* ready to use Debian Jessie images/rootfs

See [README](https://github.com/neagix/wii-linux-ngx/tree/master) for a full description and instructions.

# Releases

* [https://github.com/neagix/wii-linux-ngx/releases](https://github.com/neagix/wii-linux-ngx/releases)

# Issues

Visit the issues tracker to see what needs help:
* [https://github.com/neagix/wii-linux-ngx/issues](https://github.com/neagix/wii-linux-ngx/issues)

Pull requests are welcome!

# Frequently Asked Questions

* does the ethernet dongle work?
```
Yes.
```

* does the Wi-Fi work?
```
Yes.
```

* does SD/SDHC/USB work?
```
Yes.
```

* can I run Xorg on it?
```
It works with the framebuffer driver.
```

* does it work on GameCube?
```
It should. GameCube is supported but author cannot test on it.
```

* what type of multimedia/gaming applications can I use?
```
Anything that works on a framebuffer will do. But forget about video playback, 3D or high end games. There is no hardware acceleration support so far.
```

# Thanks

Thanks to the following people/organizations/groups:
* the Linux Kernel developers
* the GC-Linux team
* DeltaResero
