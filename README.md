# ttysecurelock

[xsecurelock](https://github.com/google/xsecurelock)'s authentication path on
the Linux text console, without X.

xsecurelock is a main program (X grabs, windows, saver processes), an X
authentication UI (`auth_x11`) and small authentication backends
(`authproto_*`) that speak a pipe protocol to the UI. The backends have no X
in them. This fork keeps `helpers/authproto_pam.c` and the protocol
(`helpers/authproto.c`, `helpers/authproto.h`) unchanged, and replaces the
X main program and `auth_x11` with one console program, `ttysecurelock.c`.
So the password goes through the same PAM conversation as with xsecurelock
in X; only the screen and keyboard handling differ.

Everything X-specific is removed: the main program, `auth_x11`, the dimmer,
monitor and saver helpers, the X test suite, autotools. Building needs a C
compiler and libpam.

## How it locks

- It must start as root, normally from a hotkey daemon. The locked user is
  the owner of the active VT, or `--user NAME`. Root-owned VTs (login
  prompts) are refused.
- A free VT is taken from the top (63 down to 13: above logind's autovt
  range, where switching to a VT makes logind start a getty that would hang
  it up), put in `VT_PROCESS` mode (the kernel asks us before
  every switch away, and we refuse) and brought to the front; switching is
  also disabled with `VT_LOCKSWITCH`. Ctrl+Alt+Fn and `chvt` / `VT_ACTIVATE`,
  even from root, cannot leave it.
- The keyboard is read raw from that VT: no echo, no signal keys. Ctrl+C,
  Ctrl+Z, Ctrl+\ do nothing; Esc and Ctrl+U clear the input; Backspace
  removes a character. The password is shown as asterisks, like
  `auth_x11`'s default prompt.
- Each attempt runs `authproto_pam` as the locked user, with a clean
  environment, and talks the xsecurelock protocol to it. Exit status 0 is
  success, as in xsecurelock. Then the previous VT comes back and the lock VT
  is freed (the mouse selection is handed to the console in front first: a
  console mouse daemon's pointer would otherwise keep the lock VT busy).

Running as root, the lock cannot be killed or signalled by the user's own
processes — stronger than xsecurelock in X, where any process of the user can
kill the locker. The per-attempt `authproto_pam` does run as the user (PAM
checks that user's password through the setuid `unix_chkpwd`); killing it only
fails that attempt. It is installed unreadable to users (mode 0711), so the
kernel makes it non-dumpable and the user's processes cannot ptrace it into
reporting success (Yama's `ptrace_scope=1` forbids that too).

## Limits

- Root can kill it. If it dies, switching stays disabled (`VT_LOCKSWITCH`):
  the console stays on the lock VT without a prompt, i.e. it fails closed;
  root has to undo `VT_LOCKSWITCH` and run `chvt`.
- Ctrl+Alt+Del still reaches systemd (reboot), and SysRq works as far as
  `kernel.sysrq` allows (16 = sync only on the machines here).
- No screen saver or idle timer: it locks when started and nothing else.
- Settings: `authproto_pam` still reads `XSECURELOCK_PAM_SERVICE` etc., but
  ttysecurelock starts it with a clean environment, so the compiled-in PAM
  service (`system-auth`, as Arch's xsecurelock) applies.

## Build, install, test

```sh
make                 # ttysecurelock and authproto_pam
sudo make install    # /usr/local/bin, /usr/local/lib/ttysecurelock
sudo make test       # tests/vt-test.py, uses VT 12 and a throwaway user
```

The test grabs the real keyboards for about 20 seconds and types through a
uinput keyboard: it checks that Ctrl+Alt+F1, root's `VT_ACTIVATE`, Ctrl+C and
signals from the locked user do not unlock, that a wrong password does not,
and that the right one does and frees the lock VT.

## Hooking it to a hotkey

[tty-i3-keys](https://github.com/dmikushin/tty-i3-keys) binds Win+Escape to
it: as root, in a transient scope of its own (`systemd-run --scope`, so
restarting the hotkey daemon leaves the lock alone), with `--user` set to
the owner of the VT in front.

## License

Apache 2.0, as xsecurelock.
