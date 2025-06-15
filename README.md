# nobszram

**The only actually uncomplicated zram swap manager for Linux.**

*Finally, a zram manager that doesn't make you read a PhD thesis to configure swap.*

## Features

- **Simple** - Three commands. That's it.
- **Fast** - Single C binary, no bloat
- **Configurable** - Support for all major compression algorithms
- **Flexible sizing** - Percentage or absolute values
- **Priority control** - Fine-tune swap behavior
- **Zero dependencies** - Just needs util-linux

## Installation

```bash
make
sudo make install
```

## Usage

It's literally three commands:

```bash
# Start zram swap
sudo nobszram start

# Stop zram swap
sudo nobszram stop

# Check status
nobszram status
```

## Configuration

Create or edit `/etc/nobszram/nobszram.conf` (it's self-explanatory) as root:

```ini
# Enable/disable zram (because sometimes you need an off switch)
enabled=true

# Size: percentage of RAM or absolute (1G, 512M, etc.)
size=25%

# Compression algorithm (zstd is usually the sweet spot)
algorithm=zstd

# Swap priority (-1 to 32767, higher = more preferred)
priority=100
```

## Requirements

- Linux with zram support (basically any modern kernel)
- `util-linux` package
- Root privileges for start/stop (status works for everyone)

---

*No systemd units, no Python dependencies, no YAML configs, no enterprise-grade overcomplicated nonsense. Just zram that works.*
