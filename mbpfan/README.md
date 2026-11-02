# mbpfan

[![Build Status](https://travis-ci.org/dgraziotin/mbpfan.svg?branch=master)](https://travis-ci.org/dgraziotin/mbpfan)

This is an enhanced version of [Allan McRae mbpfan](http://allanmcrae.com/2010/05/simple-macbook-pro-fan-daemon/)

mbpfan is a daemon that uses input from coretemp module and sets the fan speed using the applesmc module.
This enhanced version assumes any number of processors and fans (max. 10).

* It only uses the temperatures from the processors as input.
* It requires coretemp and applesmc kernel modules to be loaded.
* It requires root use
* It daemonizes or stays in foreground
* Verbose mode for both syslog and stdout
* Users can configure it using the file /etc/mbpfan.conf

**Table Of Contents**

- [Supported GNU/Linux Distributions](#supported-gnulinux-distributions)
- [Tested Mac Models](#tested-mac-models)
- [Requirements](#requirements)
- [Installation](#installation)
	- [Arch Linux](#arch-linux)
	- [CRUX Linux](#crux-linux)
	- [Debian](#debian)
	- [Fedora](#fedora)
	- [Gentoo](#gentoo)
	- [Solus](#solus)
	- [Ubuntu](#ubuntu)
	- [Generic Install Instructions (All Other Operating Systems)](#generic-install-instructions-all-other-operating-systems)
- [Run Instructions](#run-instructions)
- [Starting at boot](#starting-at-boot)
- [Usage](#usage)
- [References](#references)
- [License](#license)
- [Credits](#credits)

## Supported GNU/Linux Distributions

We provide scripts to to load mbpfan daemon at system boot for many distros.
Please note that the support is provided by volunteers. mbpfan needs tests and bug reports.

Supported distributions:

- Ubuntu
- Debian
- Archlinux
- Fedora
- RedHat
- CentOS
- Gentoo
- Alpine
- Trisquel
- Solus

## Tested Mac Models

See https://github.com/linux-on-mac/mbpfan/wiki/Tested-Mac-Models .

## Requirements

Be sure to load the kernel modules **applesmc** and **coretemp**.

These modules are often automatically loaded when booting up GNU/Linux on a MacBook. If that is not the case, you should make sure to load them at system startup.

**How do I know if applesmc and coretemp are loaded?**

In most distributions, you can run the following command:

```bash
lsmod | grep -e applesmc -e coretemp
```

If you see `coretemp` and `applesmc` listed, you are all set.

**If you do not see `coretemp` and `applesmc` listed, you must load them.**

This is _usually_ achieved by inserting the following two lines in the file `/etc/modules`

```
coretemp
applesmc
```

Please check the relevant documentation of your GNU/Linux distribution.

## Installation

### Arch Linux

See [mbpfan-git at AUR](https://aur.archlinux.org/packages/mbpfan-git/).
Otherwise, please refer to the Generic Instructions.

### CRUX Linux

Follow the instructions on [jolupalabs REPO](https://github.com/jolupa/jolupalabs) for installation.

### Debian

On Debian 10 or later install via:

```
sudo apt-get install mbpfan
```

### Fedora

On Fedora 30 or later:

```
sudo dnf install mbpfan
```

### Gentoo

Install the ```mbpfan``` package with:

    sudo emerge -av app-laptop/mbpfan


### Solus

On Solus, install the package with:
```
sudo eopkg install mbpfan
```
then enable the service.


### Ubuntu

On Ubuntu 18.04 or later install via:

```
sudo apt-get install mbpfan
```

### Generic Install Instructions (All Other Operating Systems)

Compile with

    make

Install with

    sudo make install

It copies mbpfan to /usr/sbin, mbpfan.conf to /etc (and overwrites existing files),
README.md to /usr/share/doc/mbpfan, and mbpfan.8.gz to /usr/share/man/man8

If you would like to compile with Clang instead of GCC, simply set your system's
default compiler to be Clang. Tested with Clang 3.8 and 3.9. Tested with Clang
4.0 along with llvm-lld (The LLVM Linker).


Run The Tests (Optional)
------------------------
Users may run the tests after building the program.  Please run the following command _from within the source directory_.

    sudo make tests

Note that this only works on MacBook and not desktop computers due to different environment expectations.


## Run Instructions

If not installed, run with

    sudo bin/mbpfan

If installed, manually run with

    sudo mbpfan

If installed and using the init file, run with (Ubuntu example)

    sudo service mbpfan start


## Starting at boot

**Ubuntu**

For systemd based init systems (Ubuntu 16.04+), see the systemd section below.

For upstart based init systems (Ubuntu before 16.04), an example upstart job has been provided.
For using it, execute:

    sudo cp mbpfan.upstart /etc/init/mbpfan.conf
    sudo start mbpfan

**Debian**
An init file suitable for /lib/lsb/init-functions (Debian)
is located in the main folder of the source files, called mbpfan.init.debian
Rename it to mbpfan, give it execution permissions (chmod +x mbpfan)
and move it to /etc/init.d
Then, add it to the default runlevels with (as root):

    sudo update-rc.d mbpfan defaults

**Redhat, CentOS, Fedora**
An init file suitable for /etc/rc.d/init.d/functions
(RHEL/CentOS & Fedora) is also located at the same place, this file is called
mbpfan.init.redhat. Also rename it to mbpfan, give it execution permissions
and move it to /etc/init.d
To add the script to the default runlevels, run the following as root:

    chkconfig --level 2345 mbpfan on && chkconfig --level 016 mbpfan off

**Gentoo**

To automatically run mbpfan at boot, run as root:

    rc-update add mbpfan default


**systemd**
As a special bonus, a service file for systemd is also included. To use it,
execute the following (as root):

    sudo cp mbpfan.service /etc/systemd/system/
    sudo systemctl enable mbpfan.service
    sudo systemctl daemon-reload
    sudo systemctl start mbpfan.service

**dinit**
A [dinit](https://github.com/davmac314/dinit) service definition is also included. To use it, execute the following:
    
    sudo cp mbpfan.dinit /etc/dinit.d/mbpfan
    sudo dinitctl enable mbpfan
    sudo dinitctl start mbpfan

## Usage

    Usage: ./mbpfan OPTION(S)

    -h Show the help screen
    -f Run in foreground
    -v Be (a lot) verbose

## References

* [mbpfan on Ask Ubuntu](https://askubuntu.com/search?q=mbpfan+is%3Aquestion)

## License

GNU General Public License version 3

## Credits

**This Project Is Based On:**

* http://allanmcrae.com/2010/05/simple-macbook-pro-fan-daemon/
* http://allanmcrae.com/2011/08/mbp-fan-daemon-update/
* https://launchpad.net/macfanctld
* https://www.lobotomo.com/products/FanControl/

**This Project uses following library:**

* [ANSI C Application Settings Management](http://pokristensson.com/settings.html) by Per Ola Kristensson.

## Kernel 7.3+: the macsmc backend

7.3 removed the in-tree `applesmc` driver. Its replacement, the `macsmc`
stack, is not Apple-Silicon-only: `macsmc-acpi` is the ACPI `APP0001` port
backend for Intel Macs and `macsmc-hwmon` publishes the fans. This fork adds
a second fan backend:

* if `/sys/devices/platform/applesmc.768` (or the ACPI driver directory)
  exists, behaviour is unchanged;
* otherwise mbpfan looks for an hwmon device named `macsmc_hwmon`, enables
  `/sys/module/macsmc_hwmon/parameters/fan_control`, and drives
  `fan<N>_target` (RPM) instead of `fan<N>_output` + `fan<N>_manual`.

Temperatures are untouched: mbpfan still reads CPU temperatures from
coretemp on both backends. If the `fan_control` parameter cannot be written,
mbpfan exits with a hint to boot with `macsmc_hwmon.fan_control=1`.

### Why `macsmc-hwmon` sometimes has no fans

CachyOS' 7.3 `macsmc-hwmon` registers a fan only when `F<N>Ac`, `F<N>Mn`,
`F<N>Mx` *and* `F<N>Tg` all resolve on the SMC; if one of them is missing the
fan is skipped silently and the hwmon device publishes temperatures, power,
voltage and current only. On such a machine no `fan*_target` ever appears, no
matter what `fan_control` is set to. `fan_control` only gates *writes* to a
fan that was registered at probe time, so setting it at runtime cannot create
missing fan attributes.

### Direct SMC access (`/dev/smc`) is blocked by the kernel

`macsmc-core` exposes the raw SMC key interface as `/dev/smc`, with read,
write, get-index and get-info ioctls (see `include/linux/mfd/macsmc.h`). Reads
work for every key, so this fork still uses it to enumerate fans (`FNum`,
`F<N>Ac`, `F<N>Mn`, `F<N>Mx`, `F<N>ID`) and to select the manual-mode key
(`FS! ` on older SMCs, `F<N>Md` on newer ones).

Writes, however, are refused: the ioctl handler returns `EPERM` for a
hardcoded list of keys that includes `F<N>Tg` (target speed), `F<N>Md` /
`FS! ` (manual mode), `F<N>Mc`, `F1Tf` and the battery/charging keys (`BCLM`,
`CH0B`, `CH0C`, `CH0K`, `CHLS`, `CHTD`, `CHTE`, `CLKL`, `CLKO`, `NAT*`,
`NESN`, `NTAP`, `NTOK`, `LKSA`, `LKSB`, `MBSE`, `MOCN`, `OSWD`). This is
deliberate - the misc device is meant for inspection, not for driving fans.

So `mbpfan` probes each candidate interface by writing `F0Tg`'s current value
back (a no-op for the hardware) and uses the first one that accepts it:

* `/dev/smc` - macsmc-core's own interface, fine on kernels without the
  blacklist, refused on 7.3.1-cachyos;
* `/dev/smcfan` - the shim module in [`../smcfan/`](../smcfan/smcfan.c). It
  looks up the `struct apple_smc` behind `macsmc-core` and calls the exported
  `apple_smc_write()`/`apple_smc_read()` - the same functions `macsmc-hwmon`
  uses - exposed as a misc device that only accepts writes to `F<N>Tg`,
  `F<N>Md` and `FS! `. Nothing is replaced and the charging keys stay
  unreachable.

If neither can write, mbpfan exits with:

```
mbpfan can read the SMC but not write fan keys (Operation not permitted).
/dev/smc is restricted by macsmc-core; load the smcfan module (modprobe
smcfan) or use applesmc. Exiting.
```

Load the shim with `sudo insmod ../smcfan/smcfan.ko` (or `sudo modprobe smcfan`
once it is installed; its `depends` is `macsmc-core`). Do **not** blacklist
`macsmc` while using it - the shim needs the stack. With the shim loaded the
third backend drives the fans exactly as applesmc did (`F<N>Tg` target,
`FS! `/`F<N>Md` manual mode), with no kernel replacement and no blacklist.

`tools/smc-keytool` inspects the same interface:

```sh
sudo ./tools/smc-keytool -n 1 FNum
sudo ./tools/smc-keytool -n 2 F0Ac
sudo ./tools/smc-keytool -i F0Mn      # type/size/flags: 0x40 = writable
sudo ./tools/smc-keytool -n 2 'FS! '
sudo ./tools/smc-keytool -n 16 F0ID
```

To get `macsmc-hwmon` fans at all, boot with `macsmc_hwmon.fan_control=1`
(`options macsmc_hwmon fan_control=1` in `/etc/modprobe.d/`); mbpfan then uses
that interface. If the fan attributes are still absent, the driver could not
register them, and applesmc (see the repository README) is the remaining way
to control the fans on that kernel.
