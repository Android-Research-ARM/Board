# board

a small, local command-line tool for creating `.brd` files, device-identity
labels that aras can load to present as a different phone brand/model.

## why this exists

aras ships with a made-up, safe default device identity ("pocket"), not a
real company, no trademark exposure. some games check for a recognized real
device and won't run properly against a made-up one. `.brd` files are how a
user opts into a real brand's identity, entirely by the user's own choice.
board never ships pre-made brand files, and neither does aras.

## the values have to be real, not made up

typing a valid-looking brand/manufacturer/model/codename is not the same as
typing a *real* one. games and apps that check for a recognized device match
against the actual property strings a real phone reports, not just "does
this look like a plausible string." a `.brd` filled in with guesses will
pass `board validate` (the format only checks that the fields are the right
shape) but won't fool anything that's actually checking for a known device.

getting a working `.brd` means researching the real values for a real
device, brand, manufacturer, model, and codename as that device actually
reports them, not inventing something that merely looks right. `adb shell
getprop` on a real device (or a public device-properties database) is where
those values come from. board has no way to verify a value is real, only
that it's well-formed, so that research is on whoever creates the file.

a correction, so this doesn't get overstated: today, a bad or made-up `.brd`
cannot bootloop or otherwise break the app. it only changes what the host
reports about itself, it never reaches the guest android system at all, on
purpose, because unread identity values on a boot command line are a known
way to cause a silent boot failure that looks like something else entirely.
that's exactly why they're kept apart today. if a future version ever wires
`.brd` values into the guest boot path, that risk becomes real, which is
one more reason to treat these as researched values and not guesses, even
though nothing enforces that today.

## the guest side

a `.brd`'s identity is meant to reach the android system running inside the
emulator too, not just the host app, that's the actual point: an app inside
android should see the same identity a `.brd` applies. aras's own loader
owns that delivery, the same way it owns reading and applying the file on
the host side, this was never meant to be a two-tier thing where the guest
is left out.

that delivery is being completed now, separately from the host half `board`
already covers, because it touches the guest's own boot data rather than
just what the host reports. until it lands, an app running inside android
still reflects whatever identity that particular guest image was built
with. tracked as its own piece of work, not silently skipped.

## what a `.brd` file can and can't change

**can:** brand, manufacturer, model name, device codename. the cosmetic
label a game or the settings app would show.

**can't:** anything technical. chip name, gpu name, or any property the
graphics stack depends on to actually work. those stay exactly as aras
ships them, always. a `.brd` file is structurally incapable of touching
them, the file format has no fields for that data at all, so there's
nothing to misuse even if someone tried.

## why a custom binary format instead of json

not for secrecy. board is meant to be open source, so the format can never
be truly secret from someone determined to read the code. the real reason
is softer: json is designed to be hand-edited in a text editor, which is
precisely what invites someone to "just tweak a field" and end up with a
malformed file. a small binary layout with a version header rejects garbage
naturally and makes `.brd` files unambiguous to identify, see
`brd_format.h` in this directory for the exact format spec.

there's also a category difference json would hide. a `.brd` is not a
settings file the app just reads on startup the way it might read a
preferences json. it's device-identity data, the same kind of thing a real
phone bakes into its own build.prop inside the system image itself, not
something meant to be casually flipped in a text editor after the fact. on
a real device those values live in a partition, not a config file, exactly
because they're not meant to be edited casually. `.brd` doesn't go that
far, it stays a plain file, editable by anyone who authors one on purpose,
but the format should look and feel like what it actually is: identity
data, not a settings toggle. json's whole design point is "quick to open
and change", which is the wrong shape for that.

## every file has an author, always

board requires a name on every file it creates, it defaults to your local
mac username, but you can change it to anything, including "anonymous."
there's no way to create a file with no author field at all. aras always
shows this name as **"self-reported, not verified"** before applying a
file, it's a label the file's creator chose, not a confirmed identity.

## what board does *not* do

- no account, no sign-in, no network calls. fully local.
- no aras-run store or hub. files are shared community-to-community,
  wherever people want (a forum, a discord, a repo), not through aras.
- no telemetry, no phone-home of any kind.

## usage

```sh
board create              # interactively prompts for brand/model/author, writes a .brd file
board inspect <file>       # prints a .brd file's contents in plain text
board validate <file>      # checks a .brd file is well-formed and safe to load
```

## status

implemented. `board create`/`inspect`/`validate` and the `.brd` codec
(`brd_format.c`/`.h`) are complete, and the host app reads, previews and
installs `.brd` files through the same validator this tool uses, so a file
board writes is never one the host app then rejects.

`make test` in this directory runs the format's own test suite: a create →
inspect → validate round trip, plus nineteen deliberately malformed or
hostile files written byte-by-byte by `tests/forge_brd.py` (which bypasses
the encoder on purpose, so it can produce files the encoder would refuse to
emit). every one must be rejected. that includes two files that try to
smuggle a chip/hardware value in under an unrecognised field id, a value
with an embedded NUL, shell metacharacters, a newline, a bad checksum, and
a payload appended after the end of the file.

two of those tests caught real bugs in the first version of the decoder,
which is the reason they are written the way they are:

- a value containing `pocket\0evil` validated as `pocket`, because the
  charset check ran on a NUL-terminated string and stopped early. the tail
  was being silently dropped.
- the unrecognised-field-id rejection was never actually running: an
  earlier field-count check rejected those files first, for the wrong
  reason, so the check that matters went untested.

## building

```sh
make          # builds the board binary
make test     # builds, then runs the test suite
```

no dependencies beyond a c99 compiler and libc.
