# Screens

The **Screens** section of the menu manages the screens that run LiveQX-Player: enrol a new screen, see how it is doing, send it a command, give colleagues rights.

## What you see and what you can do

It depends on your role and on the rights an administrator has given you:

- **With no rights given the list is empty.** That is not an error: no object or group is open to you yet. Ask an administrator.
- A **viewer** only looks.
- An **operator** can send commands and (if given the right) move screens.
- An **administrator** sees and does everything: creates objects, groups and screens, gives rights, sets the module up.

Screens stand in **groups**, groups belong to **objects** (for example the object "Shop floor" with the groups "Tills" and "Window"). Rights are given on a whole object or on a single group.

## The list of screens

On the left is the tree "All screens → object → group" with the number of screens. Pick a place to narrow the list.

Every screen is in one of four states:

- **Online** — the screen works and signalled recently.
- **Offline** — no signal for a while. At night, if the screen is switched off, that is normal: in the morning it comes on and returns by itself.
- **Awaiting enrolment** — the screen is created but the player on it is not enrolled yet.
- **Revoked** — the screen is cut off for good, its certificates no longer work.

Above the table are buttons with the number in each state. The server counts them **for the whole park**, not just the page. Click a state to keep only such screens.

**Search** looks in the name of the screen, the group and the object. Case does not matter for Latin and Cyrillic letters. The table shows 50 screens a page; the picked place, state, search and page are kept in the address, so a link can be sent to a colleague.

## A screen's card

Click a screen in the list. The card shows:

- **Now** — where it stands, the player version, the time of the last signal and what the screen reported about itself: health, what is playing, how long it has been running. If the screen is offline, what it reported last is shown.
- **Certificates** — each screen has its own certificate, valid for a year. The player renews it by itself. "Ends soon" means less than 60 days.
- **Commands** — what was sent to this screen and how it ended.
- **Event journal** — when the screen came online, dropped out, changed version.

The buttons depend on your rights: **Command**, **Move** (to another group) and, for an administrator, **Bind again**, **New token** (only while the screen is not enrolled), **Revoke** and **Delete**.

## Enrol a new screen (administrator)

1. **Settings.** On **Screens → Settings** set the three addresses (the server, the devices port, the command broker). Until they are set a new screen cannot be created. The same page holds the lifetime of the enrolment token (7 days by default) and the clock check: **strict** (recommended) or **relaxed** (only for a closed network without accurate time).
2. **Broker.** **Screens → Broker** shows whether the server is connected to the command broker, with a checklist of what is missing and how to fix it. Without the broker screens are visible, but commands do not reach them.
3. **Object and group.** On **Objects and groups** create the place: a screen always stands in a group. A place with something inside cannot be deleted.
4. **Add a screen.** On **Screens** click "Add a screen", enter a name and pick a group.
5. **Pairing package.** The server shows the package **once**: it holds a one-time token. Copy it or download the file and hand it to the screen's computer, then enrol the player with it. The window does not close until you copy or download the package, or tick "I have saved the package".
6. If the package is lost while the screen is not enrolled, click **New token** on the card: the earlier tokens stop working.

Until it is enrolled the screen is "Awaiting enrolment" in the list. When the player enrols, the screen becomes "Online".

## Bind again

For a screen that is already enrolled or revoked. Use it when:

- the screen's computer was replaced;
- the screen was off so long that its certificate ran out (a year without renewal);
- the device was lost or stolen and a new one takes its place.

What happens: **the old device stops working at once**, the screen goes back to "Awaiting enrolment" and you get a package with a new token (also shown once). The name, group and the whole history stay, the identifier is the same. The new device enrols with the package as usual.

**Revoke** cuts the screen off for good (its record and journal stay). **Delete** removes the record with its history; you confirm by typing the screen's name.

## Commands

Three commands:

- **Restart the player** — the player program starts again, the screen goes dark for a few seconds.
- **Reboot the screen** — the screen's computer reboots, which takes a few minutes.
- **Reload the playlist** — the player loads the playlist again, the screen does not go dark.

**To one screen:** click "Command" on its card. If the screen is offline, the command lives a few minutes and is lost if the screen does not come back.

**To a group, an object or everyone:** pick the place on the left and click "Command the group" (or "the object", or "everyone" for an administrator). It takes two steps:

1. **Check.** The server tells you how many screens would get the command, how many are offline (they will not get it) and whether any still wait for the same command. Nothing is sent.
2. **Confirm.** Type the phrase shown, for example `restart_player group Shop floor / Tills`. Case and spaces do not matter. The Send button stays off until the phrase matches.

If the screens changed while you were confirming, the command is not sent: check again. If even one screen still waits for the same command, sending is refused and the screen is named: wait for its answer or (an administrator) lift the block with "Lift the block" on the screen's card.

Who may send what:

- **A group** — with the "Command a group" mark; any operator with access to the group may reload the playlist.
- **An object** — only with the "Command an object" mark (the group mark does not replace it).
- **Everyone** — administrators only.

## Jobs

A command to a group, an object or everyone goes out in **batches** (10 screens, adjustable): the next batch goes when the previous one has confirmed. If more than 30 % of a batch do not confirm, sending **stops by itself** and the remaining screens do not get the command.

**Screens → Jobs** shows your jobs (an administrator sees all): how far each got, which batch is out, what each screen answered. The page updates by itself. While a job runs you can click "Cancel the rest": what has already reached screens stays and will be carried out, the screens not yet sent to will not get it.

If a screen does not confirm, after a few minutes (5 for a restart, 20 for a reboot) the command is marked "no confirmation" and the block lifts by itself.

## Rights on screens (administrator)

Open the user's page, the **Screens** tab (administrators have no such tab: they have every right). A table "place × marks":

- **Access** — see the screens of the place. A right on an object covers all its groups, future ones too.
- **Command a group**, **Command an object** — send commands to many screens at once.
- **Move** — move screens between groups. The mark is needed on both groups; only an administrator moves between objects.
- **Audit** — read the journal of actions about this place.

Edits gather in a draft and are saved with one "Save" button. Rights work at once, no re-login needed. If another administrator changed the same rights while you were editing, your save is refused: the page shows the current rights and the edits have to be made again. That way nobody's changes are overwritten.

For a **viewer** commands and moving do nothing. If they were stored earlier, the page says so and saving removes them, so they cannot come alive if the person is made an operator.

## Journal by place

**Screens → Journal** shows what was done with screens: who, what, to which screen and where. A move is one row "from → to". An administrator sees every place; everyone else only the places whose journal was opened to them (the "Audit" mark); if none is open there is no tab. You can narrow it to an object or a group. The server's full journal for an administrator is under **Settings → Audit Trail**.

## If something is wrong

- **A new screen does not become "Online".** Check that the pairing package reached the screen's computer and the token has not run out (the card says "Awaiting enrolment"). If it has, click "New token".
- **"Could not create the screen: the module is not set up".** The addresses are missing: open **Screens → Settings**.
- **Commands do not arrive.** Open **Screens → Broker**: the server has to be connected to the broker. On the main page an administrator sees a warning if it is not.
- **Many screens "Offline".** If it is night, wait: switched-off screens return by themselves. If not, check the network at the site and the broker page.
- **A screen does not confirm a command.** Wait for the confirmation time; an administrator can lift the block sooner.
- **"You have no right".** Ask an administrator to give the needed mark on that place.
