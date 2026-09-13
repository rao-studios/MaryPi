# computer-use — Swift twin: `Mary/Sources/MaryComputerUse`

Direct pipes into MaryOS's applications. `mcu_invoke(app, skill, args)` sends `skill.invoke` over the
desktop socket and waits for `skill.result`; the desktop calls the app's `perform` hook, the same code
its menus run. There is no accessibility tree, no screen capture and no synthetic input, because none
is needed (PORTING.md 9).

Status: planned (commit 12). Prefix `mcu_`.
