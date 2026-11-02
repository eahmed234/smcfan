# AUR packaging

Two packages, both built from this repository
(`https://github.com/eahmed234/smcfan`):

| Package | Provides | Needs |
| --- | --- | --- |
| `smcfan-dkms-git` | `/dev/smcfan`, the fan-key entry point on macsmc kernels | `dkms`, headers of a kernel whose `macsmc-core` exports `apple_smc_read/write` |
| `mbpfan-macsmc-git` | `mbpfan` (fork), daemon + unit + config | `glibc`; a fan backend: in-tree `applesmc` (kernel < 7.3) or `smcfan-dkms-git` |

They are separate because `smcfan` is an out-of-tree kernel module while
`mbpfan` also runs on kernels that still have `applesmc`. `mbpfan-macsmc-git`
`provides`/`conflicts` `mbpfan`, so it replaces the repo/AUR package rather
than sitting next to it.

## How the packages are built

Both use a VCS source and derive the version from the commit:

```
source=("smcfan::git+https://github.com/eahmed234/smcfan.git#branch=main")
pkgver() { printf 'r%s.%s' "$(git rev-list --count HEAD)" "$(git rev-parse --short=7 HEAD)"; }
```

`prepare()` fails with a readable message if the fetched tree does not contain
`smcfan/` or `mbpfan/`, which is what a wrong URL or branch looks like.

`smcfan-dkms-git` ships only `/usr/src/smcfan-<ver>/{smcfan.c,Makefile,dkms.conf}`;
the `dkms.conf` is generated in `package()` so the version is already
substituted when `pkgver()` runs. DKMS then builds the module for every
installed kernel through `/usr/share/libalpm/hooks/70-dkms-install.hook`.
Kernels older than 7.3 are skipped via `BUILD_EXCLUSIVE_KERNEL_MIN="7.3"`:
they still have `applesmc` and predate the SMC key ioctl ABI, so the hook
prints one `WARNING: ... exited 77` line per such kernel and carries on.

The package deliberately installs **no** `modules-load.d` entry — that file
would apply to every kernel, including ones where the module does not exist,
and `systemd-modules-load.service` would fail. `mbpfan.service` loads the shim
with `ExecStartPre=-/usr/bin/modprobe smcfan`, whose leading `-` absorbs the
miss. Load it by hand with `sudo modprobe smcfan` if you want `/dev/smcfan`
without the daemon.

## Build and install locally

```sh
cd packaging/aur/smcfan-dkms-git     && makepkg -si
cd packaging/aur/mbpfan-macsmc-git   && makepkg -si
```

## Publish to the AUR

The AUR is a plain git remote per package; there is no review step. Prerequisites,
once per machine:

1. Create an account at <https://aur.archlinux.org/register>.
2. Add an SSH public key under **My Account → SSH Public Key**
   (`ssh-keygen -t ed25519` if you have none, then paste `~/.ssh/id_ed25519.pub`).
3. Check the name is still free:
   `curl -s "https://aur.archlinux.org/rpc/v5/info?arg[]=smcfan-dkms-git"` → `"resultcount":0`.

Then, per package:

```sh
cd packaging/aur/smcfan-dkms-git
makepkg --printsrcinfo > .SRCINFO      # must match PKGBUILD exactly
git init -b main
git add PKGBUILD .SRCINFO smcfan-dkms-git.install
git commit -m "initial upload: smcfan-dkms-git"
git remote add aur ssh://aur@aur.archlinux.org/smcfan-dkms-git.git
git push aur main
```

Repeat in `mbpfan-macsmc-git` with `PKGBUILD`, `.SRCINFO`,
`mbpfan-macsmc-git.install` and `mbpfan.service`.

`makepkg --printsrcinfo` runs `pkgver()`, so it clones the GitHub repo — push
the source repository first. Every later source change needs:

```sh
makepkg --printsrcinfo > .SRCINFO      # after bumping pkgrel if needed
git commit -am "upstream update"
git push aur main
```

The AUR rejects a push whose `.SRCINFO` disagrees with the `PKGBUILD`.

## Notes

* `mbpfan-macsmc-git` installs `/usr/lib/modules-load.d/mbpfan.depend.conf`
  (`coretemp`), which is valid on every kernel.
* Both PKGBUILDs carry a real `# Maintainer:` line — the AUR refuses
  placeholders.
* `pkgver` is `r<commits>.<hash>`; a `-git` package moves on every rebuild,
  which is what you want for a DKMS module that tracks kernel internals.
