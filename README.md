# Xilinx Platform Cable USB driver
Tested on fedora33


```
cd /opt/Xilinx/
sudo git clone git@github.com:lukaszwielgosz/xilinx-usb-driver.git
cd xilinx-usb-driver/
sudo dnf install fxload libusb-devel
sudo make
./setup_pcusb /opt/Xilinx/14.7/ISE_DS/ISE
sudo udevadm control --reload-rules
```

plug and unplug


You may need to set the following env variable after sourcing a settings file.

in `~/.bashrc` add line
```
export LD_PRELOAD=/opt/Xilinx/xilinx-usb-driver/libusb-driver.so
```

```
source ~/.bashrc
impact
```

This library emulates Jungo Windrvr USB and parallel port functions in
userspace which are required by XILINX impact to access the Platform cable USB
and Parallel Cable III.
With this library it is possible to access the cables without loading a
proprietary kernel module which breaks with every new kernel release. It uses
the functionality provided by the libusb userspace library for USB access and
the kernel interface at /dev/parport0 for parallel port access instead and
should work on every kernel version which is supported by libusb and supports
ppdev. It was written against impact from ISE Webpack 9.1SP1 and tested with
the following software:

 * ISE 13.1
 * ISE 12.1
 * ISE Webpack 11.1
 * ISE Webpack 10.1
 * ISE Webpack 9.2SP1, SP2, SP3, SP4
 * ISE Webpack 9.1SP1, SP2, SP3
 * ISE Webpack 8.2SP3
 * ISE Webpack 8.1SP3
 * ChipScope 10.1
 * ChipScope 9.2.01i, 9.2.02i, 9.2.03i, 9.2.04i
 * ChipScope 9.1.02i, 9.1.03i
 * ChipScope 8.2.04i
 * EDK 10.1
 * EDK 9.2.01i, 9.2.02i
 * EDK 9.1.01i, 9.1.02i
 * EDK 8.2.02i
 * EDK 8.1.02i
 * Synplicity Identify

In addition to the XILINX USB and parallel cables, devices based on the FTDI
2232 serial converter chip are also experimentally supported. This includes
devices like the Amontec JTAGkey(-Tiny).

Build the library by calling `make'. If you are on a 64 bit system but want
to build a 32 bit library, run `make lib32' instead. Be sure to have the 32
bit versions of libusb-devel and libftdi-devel installed!

To use this library you have to preload the library before starting impact:

$ LD_PRELOAD=/path/to/libusb-driver.so impact
or
$ export LD_PRELOAD=/path/to/libusb-driver.so  (for sh shells)
$ setenv LD_PRELOAD /path/to/libusb-driver.so  (for csh shells)
$ impact

The source for this library can be found at:
http://git.zerfleddert.de/cgi-bin/gitweb.cgi/usb-driver

The main website is located at:
http://www.rmdir.de/~michael/xilinx/

The Git repository can be cloned with:
git clone git://git.zerfleddert.de/usb-driver


Notes for the USB cable
=======================

To use the device as an ordinary user, put the following line in a new
file "libusb-driver.rules" in /etc/udev/rules.d/ and restart udev:
ACTION=="add", SUBSYSTEMS=="usb", ATTRS{idVendor}=="03fd", MODE="666"


If your cable does not have the ID 03fd:0008 in the output of lsusb,
the initial firmware has not been loaded (loading it changes the
product-ID from another value to 8). To load the firmware follow
these steps:

1. Run ./setup_pcusb in this directory, this should set up everything
   correctly:
   - When $XILINX is set correctly:
     $ ./setup_pcusb
   - When $XILINX is not set, and ISE is installed in /opt/Xilinx/13.1:
     $ ./setup_pcusb /opt/Xilinx/13.1/ISE_DS/ISE

Old instructions, use only when the above script did not work:

1. If you have no /etc/udev/rules.d/xusbdfwu.rules file, copy it from
   /path/to/ISE/bin/lin/xusbdfwu.rules to /etc/udev/rules.d/xusbdfwu.rules

2. If you are running a newer version of udev (as in Debian Squeeze and
   Ubuntu 9.10), you need to adapt the rules-file to the new udev-version:
   sed -i -e 's/TEMPNODE/tempnode/' -e 's/SYSFS/ATTRS/g' -e 's/BUS/SUBSYSTEMS/' /etc/udev/rules.d/xusbdfwu.rules

3. Install the package containing /sbin/fxload from your linux distribution.
   It is usually called "fxload"

4. copy the files /path/to/ISE/bin/lin/xusb*.hex to /usr/share/

5. restart udev and re-plug the cable


If you have multiple cables connected, you can specify the cable to use
in the XILINX_USB_DEV environment-variable as "bus:device".
These identifiers are available in the output of lsusb:
Bus 001 Device 004: ID 03fd:0008 Xilinx, Inc.
    ^^^        ^^^
To use this cable, set the XILINX_USB_DEV variable to "001:004".


Notes for the parallel cable
============================

To access the parallel port from userspace, the kernel needs to be built with
the features "Parallel port support" (CONFIG_PARPORT), "PC-style hardware"
(CONFIG_PARPORT_PC) and "Support for user-space parallel port device drivers"
(CONFIG_PPDEV) builtin or as modules. If these features are built as modules,
they need to be loaded before using this library.
These modules are called:
parport
parport_pc
ppdev


To use the device as an ordinary user, put the user in the group 'lp'


If you have an almost compatible cable which works with other software but not
with Impact, try adding -DFORCE_PC3_IDENT to the CFLAGS line in the Makefile.
This enables a hack by Stefan Ziegenbalg to force detection of a parallel cable.


Parallel Cable IV is currently only supported in 'compatibility mode', as no
attempt to configure the ECP registers is done by this library.


If you get "Programming failed" or "DONE did not go high" when programming
through the parallel cable with Impact 9.1, make sure to have the option "Use
HIGHZ instead of BYPASS" enabled in Edit -> Preferences -> iMPACT Configuration
Preferences.
If you are using batch mode, add the following line to your cmd file:
setPreference -pref UseHighz:TRUE
(This problem also occurs on windows and when using the real windrvr in linux
and is solved with the same workaround. Impact 8.2 is working fine with the same
boards and designs)


Notes for FTDI 2232 based cables
================================

To build the driver with FTDI 2232 support, you need to have libftdi and
the libftdi development package installed. On debian, you can install both
by installing 'libftdi-dev'.

To set-up the device:
1. Find out the vendor and product id of your cable using lsusb:
   Bus 003 Device 005: ID 0403:cff8 Future Technology Devices ...
                          ~~~~~~~~~

2. Copy the sample libusb-driverrc to ~/.libusb-driverrc, edit it and replace
   the vendor and product-id in the example file with the values provided in
   the lsusb-output. You can also change the 'parallel port' which is mapped to
   this cable. Impact sees the device at that port as a Parallel Cable III.

3. To use the device as an ordinary user, put the following line in a new file
   in /etc/udev/rules.d/ and restart udev:
   ACTION=="add", SUBSYSTEMS=="usb", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="cff8", MODE="666"
   (replace the vendor and product id with your values)

The support for FTDI 2232 based devices is experimental and they are currently
significantly slower than the other supported cables.


Locked cables
=============

If you get the message 'The cable is being used by another application.' from
impact, try running the following command:

echo -e 'cleancablelock\nexit' | impact -batch
