# The MaryOS journey: from distro.conf to a login prompt — and a desktop — on a Raspberry Pi 5

These pages explain, in reading order, how a Mac turns the MaryOS definition
in `distro/` into an Ubuntu-based arm64 system, how that system boots on a
Raspberry Pi 5 and inside Apple's Virtualization.framework, how the Liquid
Platinum desktop is built from MaryUI and started, and what is Ubuntu's,
what is MaryOS's and what is still missing. They were written while
the work was done, so they say what exists and what does not.

| # | Chapter | What you learn |
|---|---|---|
| 1 | [The goal](01-the-goal.md) | Why an Ubuntu fork after the ravynOS bring-up, what MaryOS is, one rootfs for two targets |
| 2 | [The fork](02-the-fork.md) | The anatomy of `distro/`: packages, overlay, hooks, branding, and how to change MaryOS |
| 3 | [Building from source](03-building-from-source.md) | The Docker builder, its four stages, caching, images without loop devices, the kernel extractor |
| 4 | [Booting on the Pi 5](04-boot-on-pi5.md) | EEPROM, `config.txt`, the raspi kernel and initrd, the root by label, `flash-kernel` |
| 5 | [Booting in Virtualization.framework](05-boot-in-virtualization-framework.md) | Direct kernel boot, virtio devices, the window and the serial console, the entitlement |
| 6 | [First boot and userland](06-first-boot.md) | The first-boot service, the user, networking, ssh, what to change first |
| 7 | [Status and next steps](07-status-and-next-steps.md) | What works, what is still plain Ubuntu, known issues, milestones |
| 8 | [Troubleshooting](08-troubleshooting.md) | Docker, the entitlement, blank windows, cards that do not boot |
| 9 | [The desktop](09-the-desktop.md) | MaryUI and the parity contract, the `ui` stage, `vm.sh` vs `ui.sh`, the unit and the launcher, the dev loop, inside the compositor, limitations |
| – | [Glossary](glossary.md) | Terms used throughout |

Conventions: paths like `distro/...` and `builder/...` are inside `linux/`
in this repository; commands run from `linux/` unless stated otherwise.
