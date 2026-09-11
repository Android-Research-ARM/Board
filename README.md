# board

A small, local command-line tool for creating `.brd` device-profile files used by ARAS for **development, compatibility testing, and reproducible Android environments**.

A `.brd` file describes how an Android environment should identify itself at the software level, allowing developers to test applications against different device configurations without rebuilding the guest image.

## Why this exists

Android applications can make decisions based on the device information reported by the operating system.

For development and compatibility testing, it is useful to reproduce those reported values without maintaining a separate guest image for every configuration.

`board` provides a small, versioned profile format for doing exactly that.

Profiles are:

* local
* user-created
* explicitly selected by the user
* portable between development environments
* independent of ARAS accounts or services
* not distributed through a central profile store

ARAS ships with a neutral default identity (`pocket`). Users can create their own profiles when they need to reproduce a particular device configuration.

**Profiles describe software-reported device identity. They do not create or claim the underlying physical hardware.**

---

## Values should represent real devices

A useful device profile should contain values observed from the device or obtained from reliable public device specifications.

For example, developers may collect Android properties from a device using:

```sh
adb shell getprop
```

or use publicly documented device specifications.

`board validate` checks whether a `.brd` file is structurally valid. It does **not** verify that the values correspond to a particular manufacturer's hardware.

A syntactically valid profile can therefore still contain incorrect information.

---

## What a `.brd` file can change

### Device identity

A profile can specify:

* brand
* manufacturer
* model
* device codename
* human-readable device name

These correspond to software-visible device identity fields.

### Chip identity

A profile may optionally specify:

* SoC manufacturer
* SoC model
* GPU model

These three values are treated as one group and must either all be present or all be absent.

When omitted, ARAS retains its native chip information.

### Extended technical identity

Format version 3 additionally supports:

| Flag                     | Field             | Android property                   | Charset          |
| ------------------------ | ----------------- | ---------------------------------- | ---------------- |
| `--board <token>`        | Board             | `ro.product.board`                 | `[A-Za-z0-9._-]` |
| `--hardware <token>`     | Hardware          | `ro.hardware` / `ro.boot.hardware` | token            |
| `--build-id <text>`      | Build ID          | `ro.build.display.id`              | text             |
| `--gles-version <token>` | OpenGL ES version | `ro.opengles.version`              | token            |

Each field is independent. A profile may contain any subset of these fields.

A label-only profile leaves the corresponding native values unchanged.

### OpenGL ES version

`--gles-version` accepts the Android integer representation, for example:

```text
196609 = OpenGL ES 3.1
196610 = OpenGL ES 3.2
```

ARAS only accepts values supported by its graphics stack. A profile cannot add graphics capabilities that the underlying renderer does not provide.

---

## What `.brd` does not change

A `.brd` profile is an **identity/configuration layer**, not a hardware emulator.

It does not provide:

* physical hardware
* a different GPU implementation
* a different CPU
* a different bootloader
* a different security processor
* a different TEE
* hardware-backed cryptographic keys
* a different Android API level
* graphics features unavailable to the underlying renderer
* hardware-backed device attestation

Those properties remain determined by the underlying ARAS environment.

In particular, changing software-reported identity should **not be interpreted as changing Google certification, Play Integrity status, SafetyNet status, or other hardware/security attestations**.

Applications that require those services should be tested using the appropriate supported testing configuration.

---

## Chip and GPU identity

Chip and GPU identity are intentionally treated as a single group.

A profile must provide:

```text
soc manufacturer
soc model
gpu model
```

together, or provide none of them.

This prevents incomplete profiles such as specifying a chip without its corresponding GPU.

For example:

```sh
--soc-manufacturer "Qualcomm" \
--soc-model "Snapdragon 8 Elite" \
--gpu-model "Adreno 830"
```

Leaving all three unspecified is completely valid.

---

## Profiles are self-reported

Every `.brd` file contains an author field.

The author is simply metadata supplied by the person creating the profile. ARAS displays it as:

> **self-reported, not verified**

The author field is **not a manufacturer signature, certification, or proof of ownership**.

---

## What board does not do

`board` is deliberately simple:

* no account
* no sign-in
* no network connection
* no telemetry
* no phone-home mechanism
* no central profile repository
* no ARAS-run profile marketplace
* no automatic profile downloads

Profiles can be exchanged directly between developers or teams.

---

## Usage

Create a profile interactively:

```sh
board create
```

Inspect a profile:

```sh
board inspect <file>
```

View profile metadata and contents:

```sh
board view <file>
```

Validate a profile:

```sh
board validate <file>
```

Show available commands:

```sh
board help
```

### Example

```sh
board create \
  --brand samsung \
  --manufacturer Samsung \
  --model SM-S948B \
  --codename SM_S948B \
  --name "Galaxy S26 Ultra" \
  --author anonymous
```

An extended development profile can additionally specify the technical identity:

```sh
board create \
  --brand samsung \
  --manufacturer Samsung \
  --model SM-S948B \
  --codename SM_S948B \
  --name "Galaxy S26 Ultra" \
  --author anonymous \
  --soc-manufacturer "Qualcomm" \
  --soc-model "..." \
  --gpu-model "..." \
  --board ... \
  --hardware ... \
  --build-id "..." \
  --gles-version 196609
```

Use values appropriate to the device configuration being reproduced.

---

## Why a custom binary format?

The `.brd` format is intentionally small and versioned.

It is not intended to provide secrecy. `board` is open source, so the format is inherently inspectable.

The binary format instead provides:

* explicit versioning
* predictable field layouts
* strict validation
* compact profiles
* resistance to accidental structural modification
* direct consumption by the ARAS identity layer

A `.brd` is therefore closer to a small **device-profile payload** than a general-purpose configuration file.

See `brd_format.h` for the exact format specification.

---

## Building

```sh
make
```

Run the test suite:

```sh
make test
```

Requirements:

* C99-compatible compiler
* libc

No additional dependencies are required.

