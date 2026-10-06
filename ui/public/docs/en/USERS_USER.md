# Users

The **Settings → Users** section lists all accounts in the system. It is available to **administrators only**.

## The list

A table with search by username and email and a filter by role. Columns: **Username**, **Email**, **Role**, **Source** (local or ldap), **Status**, **Last login**.

Statuses:

- **enabled** — a normal working account;
- **disabled** — sign-in is forbidden;
- **locked** — a temporary lock after failed sign-in attempts;
- **must change pw** — at the next sign-in the user must set a new password.

## Roles

The role determines which sections are available. Access to **specific channels** is granted separately (see below).

### Viewer

Can watch channels they have been given access to: state, playlist, metrics. Cannot change anything.

### Operator

Everything the viewer can, plus managing channels with the "Operate" permission: start and stop, playlist, schedule, outputs, changing configuration. Sees the **Operations** section (viewing the stress test, and profiling) and metrics.

An operator **cannot**: create and delete channels, manage users, shares, time, TLS, plugins and gateways.

### Admin

Full access to all channels and all sections: creating and deleting channels, users, LDAP, SMTP, Audit Trail, Master Key, TLS, Storage, Time, plugins, gateways, configuring and running the stress test.

## Channel access

For a viewer and an operator, access to each channel is granted separately. The permissions:

- **View** — see the channel;
- **Operate** — manage the channel (also requires the operator role).

There are two ways to grant access:

- in the user's card → the **Channel grants** tab (shown for non-administrators);
- on the channel page → the **Permissions** tab → **Add user**.

Administrators have full access to all channels without separate grants. If a viewer or operator has no grant for a channel, they cannot access it.

## Creating a user

1. **Create user**.
2. Fill in:
   - **Username** — Latin letters, digits, `.`, `_`, `-`, up to 64 characters;
   - **Email** — optional;
   - **Role**;
   - **Source** — `local` (the password is stored in LiveQX) or `ldap` (sign-in through the directory);
   - **Password** (optional, at least 8 characters) — if left empty, the server generates a **one-time password**;
   - **Require password change at first login** — on by default.
3. Save. The initial password is shown **once** — copy it and give it to the user.

## The user's card

Clicking a row opens a card with an overview (creation date, last login, number of failed logins) and an edit form. You can change the **email** and the **role**. The username and source cannot be changed.

## Actions on a user

The "⋯" menu in the row:

- **Edit**;
- **Enable / Disable** — a disabled user cannot sign in, and all their active sessions are ended;
- **Unlock** — removes the temporary lock after failed sign-ins (the item appears only if the account is locked);
- **Reset password** — revokes all the user's sessions and generates a new one-time password, shown once. At the next sign-in the user is asked to change it;
- **Delete** — **complete and irreversible** removal: login, sessions, channel grants and reset tokens. You must type the username to confirm. Audit history is preserved (it keeps the username at the time of the record).

You cannot delete your own account or the last enabled administrator.

## Lockout after failed sign-ins

A local account is locked automatically after **5 failed attempts in a row**. The first lock lasts 30 seconds, each next failure doubles the time, up to a maximum of 1 hour. A successful sign-in resets the counter. An administrator can unlock the account early. This protection does not apply to LDAP accounts — the directory has its own.

## LDAP accounts

If sign-in through LDAP is configured (**Settings → LDAP**), directory users can sign in, and their role is determined by the LDAP settings. The password of such an account is changed in the corporate system, not in LiveQX.

## Audit Trail

Who changed what and when in the system — see **Settings → Audit Trail** (administrator only). The log records sign-ins, password changes, operations on users, channels, outputs and other changes, with filtering, details ("was → became") and an integrity check of the records.
