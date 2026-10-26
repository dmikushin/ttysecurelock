#!/bin/bash
# Real authproto_pam (service system-auth) as a throwaway user:
#   a cancelled conversation (what auth_x11 sends on timeout) must not count,
#   wrong passwords must, the right one must pass.
set -u
U=tslpamtest; P='Correct-Horse-7'
H=$(ls /usr/local/libexec/ttysecurelock/authproto_pam /usr/local/lib/ttysecurelock/authproto_pam /usr/local/libexec/xsecurelock/authproto_pam 2>/dev/null | head -1)
[ -x "$H" ] || { echo "authproto_pam not found"; exit 1; }
sudo useradd -M -s /usr/bin/nologin $U 2>/dev/null; echo "$U:$P" | sudo chpasswd
sudo faillock --user $U --reset
pkt() { printf '%s %d\n%s\n' "$1" "${#2}" "$2"; }   # authproto packet: type len \n payload \n
run() { # $1 = what to answer the password prompt with: "cancel" or a password
  if [ "$1" = cancel ]; then printf 'x 0\n'; else pkt p "$1"; fi |
    sudo -u $U env USER=$U XSECURELOCK_PAM_SERVICE=system-auth "$H" >/dev/null 2>&1; echo $?
}
count() { sudo faillock --user $U | grep -c ' V$'; }
echo "3 cancels:   exits $(run cancel) $(run cancel) $(run cancel) -> failures $(count) (want 0)"
echo "right pass:  exit $(run "$P") (want 0) -> failures $(count)"
echo "2 wrong:     exits $(run nope1) $(run nope2) -> failures $(count) (want 2)"
echo "1 cancel:    exit $(run cancel) -> failures $(count) (want 2)"
echo "3rd wrong:   exit $(run nope3) -> failures $(count) (want 3)"
echo "right pass while locked: exit $(run "$P") (want non-zero)"
sudo faillock --user $U --reset; sudo userdel $U; echo "cleaned up: $(id $U 2>&1 | head -c 40)"
