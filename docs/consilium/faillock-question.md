# Question for the consilium: screen lock vs. pam_faillock

## Setup
- Linux laptops (Arch/CachyOS), text console only, no X. One user per machine.
- Screen lock: ttysecurelock, a fork of google/xsecurelock 1.9.0 for the Linux
  text console. Upstream code unchanged: main.c starts the auth helper
  auth_x11 when the user presses a key; auth_x11 runs the PAM helper
  authproto_pam, which calls pam_authenticate() with PAM service
  "system-auth" and a conversation that asks auth_x11 for the password.
  XSECURELOCK_AUTH_TIMEOUT (here 60 s) closes the prompt if nothing is
  typed; the screensaver (matrix rain) comes back.
- /etc/pam.d/system-auth (Arch default):
    auth  required  pam_faillock.so preauth
    auth  [success=2 default=ignore] pam_systemd_home.so
    auth  [success=1 default=bad]    pam_unix.so try_first_pass nullok
    auth  [default=die]              pam_faillock.so authfail
    auth  optional  pam_permit.so ...
  /etc/security/faillock.conf: defaults (deny=3, fail_interval=900,
  unlock_time=600).
- The same account logs in on the console (getty/login, Ly display
  manager), over ssh with keys only, and via sudo (passwordless here).

## Problem (measured)
Every auth prompt that times out without input is counted by
pam_faillock as a failed authentication: the conversation is aborted, so
pam_unix fails, pam_faillock authfail records it. Three such timeouts
within 15 minutes lock the account for 10 minutes; during that time even
the correct password is refused at the screen lock (and at console
login). With a screensaver, a bumped key or a laptop folded into tablet
mode (its keyboard sends stray keys) is enough. upstream xsecurelock under
X behaves the same way.

## Options considered so far (the user finds all of them weaker)
1. A dedicated PAM service for the lock without pam_faillock.
2. Raise faillock's deny globally.
3. Leave it.

## The question
Propose the best design that keeps or improves brute-force protection at
the lock and elsewhere, and does not lock the user out because of prompts
that time out with no password typed. Be concrete: what changes where
(PAM stack, pam_faillock options, the lock's helpers — note the user
wants the fork to stay a true fork of xsecurelock: changes outside its
console platform layer need strong justification; a wrapper helper or a
PAM-level fix is preferred over patching upstream files), and the
security argument. Consider for example: not calling pam_authenticate
until a password has actually been entered (does upstream's protocol
allow the auth helper to defer the PAM call? authproto_pam's
conversation is driven by PAM's prompts), PAM_CONV_ERR vs PAM_AUTH_ERR
and how pam_unix/pam_faillock treat an aborted conversation, faillock's
per-service or per-tty behaviour, pam_faildelay, and anything better.
Point out what is wrong in the premises if you see it. Keep the answer
under 500 words.
