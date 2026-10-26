#!/bin/bash
# Do not count an aborted password prompt (no password typed) as a failed
# login; a wrong password still counts. See ttysecurelock docs/consilium/.
set -e
f=/etc/pam.d/system-auth
old='^auth[[:space:]]+\[success=1 default=bad\][[:space:]]+pam_unix\.so[[:space:]]+try_first_pass nullok$'
grep -qE "$old" $f || { grep -n pam_unix $f; echo "unexpected pam_unix line"; exit 1; }
sudo cp -a $f /root/system-auth.before-authtok-err
sudo sed -i -E "s/^(auth[[:space:]]+)\[success=1 default=bad\]([[:space:]]+pam_unix\.so)/# A prompt that was aborted (timeout, Escape) checked no password: it fails\n# the login but is not a failed password for pam_faillock (authtok_err,\n# conv_err skip authfail). A wrong password still goes to authfail.\n\1[success=1 authtok_err=die conv_err=die default=bad]\2/" $f
grep -n -B3 'pam_unix' $f
