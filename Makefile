CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra
PREFIX ?= /usr/local
HELPER_PATH ?= $(PREFIX)/lib/ttysecurelock
# The PAM service xsecurelock uses on Arch (configure --with-pam-service-name).
PAM_SERVICE_NAME ?= system-auth

DEFS = -D_GNU_SOURCE -DHAVE_EXPLICIT_BZERO -DPAM_CHECK_ACCOUNT_TYPE \
	-DHELPER_PATH=\"$(HELPER_PATH)\" -DPAM_SERVICE_NAME=\"$(PAM_SERVICE_NAME)\"

COMMON = env_info.c env_settings.c logging.c util.c

all: ttysecurelock authproto_pam

ttysecurelock: ttysecurelock.c helpers/authproto.c $(COMMON)
	$(CC) $(CFLAGS) $(DEFS) -o $@ $^

authproto_pam: helpers/authproto_pam.c helpers/authproto.c $(COMMON)
	$(CC) $(CFLAGS) $(DEFS) -o $@ $^ -lpam

install: all
	install -Dm755 ttysecurelock $(DESTDIR)$(PREFIX)/bin/ttysecurelock
	# Unreadable to users: exec then makes the helper non-dumpable, so the
	# locked user cannot ptrace it into reporting success.
	install -Dm711 authproto_pam $(DESTDIR)$(HELPER_PATH)/authproto_pam

clean:
	rm -f ttysecurelock authproto_pam

test: all
	python3 tests/vt-test.py

.PHONY: all install clean test
