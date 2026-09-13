# computer-use — Swift twin: `Mary/Sources/MaryComputerUse`

Direct pipes into MaryOS's applications. `mcu_invoke(app, skill, args)` sends `skill.invoke` over the
desktop socket and waits for `skill.result`; the desktop calls the app's `perform` hook, the same code
its menus run. There is no accessibility tree, no screen capture and no synthetic input, because none
is needed (PORTING.md 9).

`computer-use/invoke.h` is maryd's side: call ids, results matched to calls in any order, timeouts, and
every pending call ended when the desktop connection goes. It takes a send function and is fed what
arrives, so it knows nothing of sockets. `mcu_app_state` is declared and answers ENOSYS.

Status: working. Prefix `mcu_`.
