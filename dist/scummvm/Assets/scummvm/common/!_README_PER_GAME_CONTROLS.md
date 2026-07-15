# Per-Game Controller Mappings

This ScummVM openFPGA build supports optional, per-game controller mappings through each game's `*_os.ini` file.

After the updated core binaries are installed, control layouts can be changed without recompiling the core. Each game can have its own mappings, and games without a `[controls]` section continue using the standard controls.

## INI File Location

Game configuration files are located under:

```text
Assets/scummvm/common/
```

Examples:

```text
tentacle_os.ini
monkey_os.ini
monkey2_os.ini
samnmax_os.ini
```

Edit the `*_os.ini` file that belongs to the game you want to customize.

## Adding a Controls Section

Add a `[controls]` section to the bottom of the game's INI file.

Example for Day of the Tentacle:

```ini
[controls]
select_left=1
select_up=2
select_right=3
l=l
r=u
```

Only mappings listed in the section are overridden. Any omitted button keeps its normal ScummVM openFPGA behavior.

## Supported Pocket Inputs

### Select Combinations

These mappings trigger when Select is held while pressing another button:

```ini
select_up=
select_down=
select_left=
select_right=
select_a=
select_b=
select_x=
select_y=
select_start=
```

### Standard Buttons

These mappings apply when the button is pressed normally:

```ini
a=
b=
x=
y=
l=
r=
start=
```

## Default Controls

When a mapping is not present, the existing control remains active.

| Input | Default action |
|---|---|
| A | Left mouse click |
| B | Right mouse click |
| X | Enter |
| Y | Space |
| Start | F5 / ScummVM menu |
| L | Slow mouse movement |
| R | Fast mouse movement |
| Select + Up | Master volume up |
| Select + Down | Master volume down |
| Select + Left | Music volume down |
| Select + Right | Music volume up |
| Select + B | Escape |
| Select + Start | Toggle numeric keypad mode |

For example, this overrides only L and R:

```ini
[controls]
l=pageup
r=pagedown
```

All other controls continue using their defaults.

## Supported Keyboard Values

Mappings are case-insensitive.

### Letters

```text
a through z
```

### Numbers

```text
0 through 9
```

### Common Keys

```text
enter
return
escape
esc
space
tab
backspace
delete
insert
home
end
pageup
pagedown
up
down
left
right
period
dot
comma
minus
equals
slash
semicolon
```

### Function Keys

```text
f1 through f12
```

### Keyboard Prefix

The optional `keyboard:` prefix is supported.

These are equivalent:

```ini
x=enter
```

```ini
x=keyboard:enter
```

Other examples:

```ini
select_left=keyboard:1
l=keyboard:l
start=keyboard:f5
```

## Disabling a Mapping

Use `none` or remove the line.

```ini
l=none
```

A missing value or `none` means that no custom keyboard mapping is configured for that input, so its normal default behavior remains active.

## Complete Template

Copy this template into a game's `*_os.ini` file and fill in only the mappings you need:

```ini
[controls]

# Select combinations
select_up=
select_down=
select_left=
select_right=
select_a=
select_b=
select_x=
select_y=
select_start=

# Standard buttons
a=
b=
x=
y=
l=
r=
start=
```

Blank entries may be removed. Keeping only configured lines makes the file easier to read.

## Example Profiles

### Day of the Tentacle

```ini
[controls]
select_left=1
select_up=2
select_right=3
l=l
r=u
```

### Adventure Game Shortcuts

```ini
[controls]
x=enter
y=space
start=f5
select_b=escape
```

### Keyboard-Heavy Game

```ini
[controls]
a=enter
b=escape
x=tab
y=space
l=pageup
r=pagedown
select_left=left
select_right=right
select_up=up
select_down=down
```

## Applying Changes

1. Edit the correct `*_os.ini` file under `Assets/scummvm/common/`.
2. Save the file.
3. Copy the updated INI file to the same location on the Analogue Pocket SD card.
4. Fully exit and relaunch the game.

The core does not need to be recompiled when changing mappings in an INI file.

## Troubleshooting

### The custom mapping does not work

Confirm that:

- The section name is exactly `[controls]`.
- The button name matches one of the supported input names.
- The key value is supported.
- You edited the INI file for the correct game.
- The updated INI was copied to the Pocket SD card.
- The game was fully exited and relaunched after the change.

### The original action still occurs

The mapping may be missing, misspelled, or unsupported. Unknown values are ignored, and the original control remains active.

For example:

```ini
select_left=one
```

is invalid. Use:

```ini
select_left=1
```

### A or B no longer clicks the mouse

Assigning a keyboard value to `a=` or `b=` replaces that button's normal mouse-click action.

For example:

```ini
a=enter
```

changes A from left click to Enter. Remove the line to restore the default mouse action.

### L or R no longer changes mouse speed

Assigning `l=` or `r=` replaces that shoulder button's normal slow/fast mouse behavior. Remove the mapping to restore the default.

## Notes

- Mapping changes are per game.
- Games without a `[controls]` section use standard controls.
- Unspecified buttons retain their existing behavior.
- Invalid values are ignored safely.
- The physical Analogue Pocket does not have L3 or R3 buttons, so those inputs are not exposed in this configuration.