# board

a small, local command-line tool for creating `.brd` files, device-identity
labels that aras can load to present as a different phone brand/model.

## why this exists

aras ships with a made-up, safe default identity ("pocket"), not a real
company, no trademark exposure. some games only run against a recognized
real device. `.brd` files are how a user opts into a real brand's identity,
entirely by their own choice. board never ships pre-made brand files, and
neither does aras.

## the values have to be real, not made up

a plausible-looking brand/manufacturer/model/codename isn't the same as a
real one. games match against the actual strings a real phone reports, not
"does this look right." `board validate` only checks shape, not truth, so a
`.brd` full of guesses will validate fine and still fool nothing. getting a
working file means researching real values (`adb shell getprop` on a real
device, or a public device-properties database), not inventing them.

## what a `.brd` file can and can't change

**can:** brand, manufacturer, model name, device codename, the cosmetic
label a game or the settings app would show.

**can't:** anything technical, chip or gpu name, or anything the graphics
stack depends on. those stay exactly as aras ships them. the format has no
fields for that data at all, so there's nothing to misuse even if someone
tried.

## why a custom binary format instead of json

not for secrecy, board is open source, so nothing here can be truly secret.
the deeper reason is what a `.brd` actually is: the live pipe aras's own
loader reads to change device identity without rebuilding the guest image
at all. rebuilding an image to change a handful of strings is heavy;
a `.brd` is deliberately small and versioned instead, closer to a channel
something reads from than a file something opens. json invites hand-editing
in a text editor, which invites "just tweak a field" and a malformed file,
exactly the kind of casual mutation a pipe like this can't tolerate on the
other end. a small versioned binary layout rejects garbage naturally, and
reads like what it is: identity data moving through a channel, not a
settings file sitting still. see `brd_format.h` for the exact spec.

## every file has an author, always

board requires a name on every file it creates, defaulting to your local
mac username, but changeable to anything, including "anonymous," never
blank. aras always shows it as **"self-reported, not verified"** before
applying a file, a label its creator chose, not a confirmed identity.

## what board does *not* do

- no account, no sign-in, no network calls. fully local.
- no aras-run store or hub. files are shared community-to-community, not
  through aras.
- no telemetry, no phone-home of any kind.

## usage

```sh
board create              # interactively prompts for brand/model/author, writes a .brd file
board inspect <file>       # prints a .brd file's contents in plain text
board validate <file>      # checks a .brd file is well-formed and safe to load
```

## building

```sh
make          # builds the board binary
make test     # builds, then runs the format's own test suite
```

no dependencies beyond a c99 compiler and libc.
