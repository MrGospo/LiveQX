# Profile

The page of your personal settings. Open it through **Settings → Profile** in the menu.

## Account

The top block holds information about you:

- **Username** — your login. It cannot be changed.
- **Email** — your email, if the administrator set one.
- **Role** — your permission level:
  - **Viewer** — viewing channels you have been given access to.
  - **Operator** — managing channels on which you have the "Operate" permission.
  - **Admin** — full access.
- **Source** — where the account is stored: `local` (created by an administrator in LiveQX) or `ldap` (the corporate directory).
- **Last IP** and **Last login** — when and from where you signed in last time.

More about roles: [Users](USERS_USER.md).

## Interface language

A **Русский / English** switch. It applies immediately. The choice is stored in the browser, so on another device or browser you need to choose the language again.

## Changing the password

Only for **local** accounts. The password of an LDAP account is changed in the corporate system.

Three fields:

- **Current password**;
- **New password** — **at least 12 characters**; use letters, digits and special characters. The new password must differ from the current one;
- **Confirm new password** — must match the new one.

After a successful change:

- the password changes immediately;
- **the current session stays active** — you do not need to sign in again;
- **all your other sessions** (other browsers and devices) **are ended** — you will need to sign in there again with the new password;
- a record of the password change appears in the Audit Trail.

If an administrator created your account with a temporary password or reset the password, the system itself asks you to set a new one at first sign-in — the "Change password" page opens before you can do anything else.

## Active sessions

A table of devices where you are signed in with your account:

- **Device** — determined from the browser;
- **IP** — the sign-in address;
- **Last seen** — when the session made its last request;
- **Created** — when the sign-in happened;
- a **"This device"** mark on your current session.

The **Revoke** button ends the selected session. If you revoke another device, it is signed out on its next request. If you revoke the current session, you are signed out immediately.

Revoke sessions if you forgot to sign out on someone else's computer or suspect someone learned your password.

## Signing out

The sign-out button in the interface ends the current session.
