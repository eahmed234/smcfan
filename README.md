# smcfan — fan control for Intel Macs on Linux 7.3+

Linux 7.3 removed the `applesmc` driver and moved Intel Macs onto the
`macsmc` stack. On some models that leaves no way to drive the fans:

* `macsmc-hwmon` only registers a fan when the SMC exposes `F<N>Ac`, `F<N>Mn`,
  `F<N>Mx` **and** `F<N>Tg`. A MacBookPro11,2 exposes none of them through
  that path, so its `/sys/class/hwmon/hwmonN/` has `temp*`, `power*`, `curr*`
  and `in*` but no `fan*` at all — `macsmc_hwmon.fan_control=1` cannot create
  attributes the driver never registered.
* `macsmc-core` does publish the raw key interface as `/dev/smc`, and reads
  work, but its ioctl handler returns `EPERM` for a hardcoded list that
  contains every key needed to move a fan (`F<N>Tg`, `F<N>Md`, `FS! `).

Battery reporting regressed the same way on these machines; that fix lives in
its own project: [`smcbatt`](https://github.com/eahmed234/smcbatt).

This repository closes that gap without touching the macsmc stack:

| Piece | What it does |
| --- | --- |
| [`smcfan/`](smcfan/) | DKMS module. Finds the `struct apple_smc` behind `macsmc-core` and exposes the exported `apple_smc_read()`/`apple_smc_write()` pair as `/dev/smcfan` (root only, mode 0600). Reads pass through for any key; writes are limited to `F<N>Tg`, `F<N>Md` and `FS! `, so battery and charging keys stay unreachable even for root. It also registers a read-only hwmon with `fan<N>_input`/`_min`/`_max`, because `macsmc-hwmon` publishes no fan channels on these models. |
| [`mbpfan/`](mbpfan/) | Fork of [mbpfan](https://github.com/linux-on-mac/mbpfan) that learns the macsmc paths: `macsmc-hwmon`'s `fan*_target` attributes when they exist, then `/dev/smc`, then `/dev/smcfan`. The original `applesmc` path is unchanged, so it still works on kernels older than 7.3. |
| [`packaging/aur/`](packaging/) | `smcfan-dkms-git` and `mbpfan-macsmc-git` AUR packages, plus notes on publishing them. |

Nothing in `macsmc` is replaced, patched or blacklisted; the module only calls
the interface the driver already exports.

## Requirements

* An Intel Mac whose SMC is bound by `macsmc` (kernel 7.3+), on a kernel whose
  `macsmc.h` has the key ioctl ABI. On CachyOS that is the Intel-Mac port of
  7.3; 6.18 LTS does not have it and still ships `applesmc`, so DKMS skips
  those kernels on purpose.
* `dkms` plus the headers of the kernel you run.
* `coretemp` for CPU temperatures (the mbpfan unit pulls it in).

## Install (Arch / CachyOS, AUR)

```sh
yay -S smcfan-dkms-git mbpfan-macsmc-git
sudo systemctl enable --now mbpfan.service
```

`mbpfan-macsmc-git` provides and conflicts with `mbpfan`, and takes over
`/etc/mbpfan.conf` as a `backup=` file, so replacing the repo package keeps
your temperature tuning.

## Install (any distro, from source)

```sh
git clone https://github.com/eahmed234/smcfan
cd smcfan

# module
sudo make
sudo insmod smcfan/smcfan.ko          # depends on macsmc-core
ls -l /dev/smcfan

# daemon
make -C mbpfan
sudo mbpfan/bin/mbpfan -f -v          # foreground, verbose
```

For DKMS instead of `insmod`, install `smcfan/` into `/usr/src/smcfan-<ver>/`
with the `dkms.conf` that the PKGBUILD generates (see
`packaging/aur/smcfan-dkms-git/PKGBUILD`), then `dkms add`/`dkms install`.

## Configure

`/etc/mbpfan.conf`:

```ini
low_temp = 63        # below this, fans run at minimum speed
high_temp = 66       # above this, fan speed ramps up
max_temp = 86        # above this, fans run at full speed
polling_interval = 1
```

Per-fan overrides use `min_fan1_speed` / `max_fan1_speed`; without them mbpfan
reads `fan*_min` / `fan*_max` from the driver and falls back to 2000/6200.

## Verify

```sh
lsmod | grep smcfan                  # smcfan, used by 0, depends macsmc-core
ls -l /dev/smcfan                    # crw------- root root
systemctl status mbpfan --no-pager
journalctl -u mbpfan -b --no-pager | tail -20
```

With `-v`, mbpfan logs which backend it picked, e.g.
`using SMC key fan control via /dev/smcfan (2 fans)`.

Fan RPM is back in sysfs, read-only:

```sh
sensors | grep -A3 smcfan
cat /sys/class/hwmon/hwmon*/fan1_input
```

`extras/sensors.d/macsmc_hwmon.conf` hides the `macsmc_hwmon` channels that are
not temperatures (a filter coefficient that reads 8192 °C, a hot indicator,
and drive sensors this model does not have); copy it to `/etc/sensors.d/` if
`sensors` output bothers you. Desktop widgets read hwmon directly and ignore
that file.

## Troubleshooting

* **`mbpfan found no fan*_target ... Boot with macsmc_hwmon.fan_control=1`** —
  the kernel's `macsmc-hwmon` registered no fan at probe time, which is the
  case this module exists for. Install `smcfan-dkms-git`; `fan_control` alone
  will not help.
* **`SMC key writes are refused (Operation not permitted)`** — mbpfan fell
  back to `/dev/smc`, which blocks fan writes by design. Load the shim:
  `sudo modprobe smcfan` (the packaged unit does this itself via
  `ExecStartPre=-/usr/bin/modprobe smcfan`).
* **`dkms install ... exited 77`** — dkms reporting `BUILD_EXCLUSIVE_KERNEL_MIN`
  for a kernel older than 7.3. Not a failure; those kernels keep `applesmc`.
* **Fans never change speed** — check `FS! ` / `F<N>Md` are accepted:
  `sudo mbpfan/tools/smc-keytool -d /dev/smcfan -w 'FS! ' 0003` for two fans
  (the daemon sets the same bits itself). `smc-keytool` is built by
  `make -C mbpfan tools`.

## Layout

```
smcfan/          kernel module (source + kbuild Makefile)
extras/          lm-sensors profile for macsmc_hwmon
mbpfan/          daemon fork, tests, smc-keytool debug tool
packaging/aur/   AUR PKGBUILDs, unit, install messages
```

## License

* `smcfan/` — GPL-2.0-only, see `LICENSE`.
* `mbpfan/` — GPL-3.0-or-later, see `mbpfan/COPYING`.

`mbpfan` is the work of the mbpfan authors; this fork adds the macsmc
backends. `smcfan` exists because `macsmc-core`'s ioctl path and
`macsmc-hwmon`'s probe requirements each close one of the two usable doors.
