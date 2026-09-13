# skills — Swift twin: `Mary/Sources/MaryPlugin` and `MaryFoundation/Ability/SkillSchemas.swift`

What Mary can do with each application. On MaryOS the apps are Mary's own, so a skill is declared by
the app in code (`lp_skill` in MaryUI) and System Settings holds the per-app policy. This package is
maryd's side: the registry built from the desktop's `skills` message, the policy as the desktop
reported it, and `sk_tools_json`, which will render the enabled skills as Mistral `tools`.

Deviation: the `.mary` ability packages and their AX-recorded recipes are not ported (PORTING.md 10).

Status: planned (commit 12). Prefix `sk_`.
