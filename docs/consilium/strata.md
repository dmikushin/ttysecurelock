# omen/strata

## Ответ

**Посылка верна — я проверил её экспериментально.** Ваш же `helpers/authproto_pam` при таймауте получает от `auth_x11` пакет `PTYPE_RESPONSE_CANCELLED` (auth_x11.c:1484), `ConverseOne` возвращает `PAM_CONV_ERR` (authproto_pam.c:47), `pam_vprompt` передаёт его в `pam_get_authtok`, который при `retval != PAM_SUCCESS` возвращает **PAM_AUTHTOK_ERR** (libpam `pam_get_authtok.c`, строки «We want to abort»). В стеке Arch `default=bad` для pam_unix означает, что и `authtok_err` тоже ведёт к `[default=die] pam_faillock authfail` — запись в tally. Запуск `printf 'x 0\n' | ./authproto_pam` реально создал запись в `/var/run/faillock/marcusmae`. `deny=3` срабатывает как `failures >= deny` (pam_faillock.c:241) — три таймаута = лок.

**Что не работает:** «отложить pam_authenticate» в рамках протокола невозможно — разговором управляет PAM, а `pam_get_authtok` никогда не возвращает `PAM_INCOMPLETE`/`PAM_CONV_AGAIN` (проверено в исходниках: `pam_get_user` конвертирует CONV_AGAIN→INCOMPLETE, но путь authtok — нет). `pam_faildelay` к подсчёту отношения не имеет. Отдельного «per-service» tally у faillock нет: счётчик всегда per-user, источник лишь записывается в строку (RHOST/TTY/SERVICE, pam_faillock.c:460-470).

**Предлагаемое решение (PAM-уровень, ноль изменений в коде форка):**

1. Отдельный сервис для замка: `/etc/pam.d/ttysecurelock` (уже поддерживается: `XSECURELOCK_PAM_SERVICE`, authproto_pam.c:146) со своим tally-каталогом `dir=/var/run/faillock-lock`, чтобы лок замка не блокировал вход в консоль и наоборот (иначе случайные нажатия клавиш = само-DoS).

2. Ключевая правка — различать «пароль не ввели» и «пароль неверный»:

```
auth  required   pam_faillock.so preauth dir=/var/run/faillock-lock
auth  [success=3 default=ignore]  pam_systemd_home.so
auth  [success=3 authtok_err=1 default=bad] pam_unix.so try_first_pass nullok
auth  [default=die] pam_faillock.so authfail dir=/var/run/faillock-lock
auth  [default=die] pam_deny.so
auth  optional    pam_permit.so
auth  required    pam_env.so
auth  required    pam_faillock.so authsucc dir=/var/run/faillock-lock
```

`authtok_err=1` перепрыгивает `authfail` на `pam_deny`: таймаут даёт отказ (7, экран остаётся закрытым), но **не** записывается в tally.

**Проверено прогоном** (probe, воспроизводящий ровно поведение `ConverseOne`; тестовый пользователь, отдельный tally-каталог):
- 5 таймаотов подряд → отказ, tally пуст;
- 3 неверных пароля → 3 записи, аккаунт заблокирован, `preauth` сообщает «account is locked due to 3 failed logins»;
- верный пароль во время лока → всё равно отказ (preauth раньше pam_unix);
- таймаут во время лока → новой записи, лок сохраняется;
- 2 таймаута + 3 неверных → ровно 3 записи, лок работает;
- успешный вход сбрасывает счётчик (authsucc).

**Аргумент безопасности:** faillock считает ровно те события, где пароль реально проверялся и не совпал. Прерванный разговор — не попытка аутентификации; защищать от него бессмысленно, а атака через таймауты (заблокировать пользователя) наоборот устраняется. Brute-force защита не ослабляется: deny=3 остаётся. Повышать deny глобально — ослабление, не решение.

**Оговорки:** я не проверял поведение `pam_systemd_home`/пустого пароля/`PAM_USER_UNKNOWN` в новом стеке; `default=bad`→die теперь не считает системные ошибки pam_unix (например, недоступность shadow) — это осознанный компромисс. Реальный бинарный `auth_x11` на консоли я не запускал — проверка шла через probe с идентичной семантикой пакетов.
