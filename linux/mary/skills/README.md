# skills — Swift twin: `Mary/Sources/MaryPlugin` and `MaryFoundation/Ability/SkillSchemas.swift`

What Mary can do with each application. On MaryOS the apps are Mary's own, so a skill is declared by
the app in code (`lp_skill` in MaryUI) and System Settings holds the per-app policy. This package is
maryd's side: the registry built from the desktop's `skills` message, the policy as the desktop
reported it, `sk_tools_json`, which renders the skills Mary may use as Mistral `tools`, and
`sk_ability_records`, which writes the registry into the Thread.

Deviation: the `.mary` ability packages and their AX-recorded recipes are not ported (PORTING.md 10).

The registry decides a call the way the desktop does — unknown, then denied (the app or skill is off),
then needs confirmation (destructive, or the app asks first), else allowed — so maryd can say why before
asking. Allowed skills, and those that need confirmation (the skills lane parks them for the person),
render as Mistral tools named `<app>__<skill>`.

A skill carries the Mac's `SkillSchema` fields when the desktop declares them — kind (cognitive, effectful,
workflow), access (seamless, confirm, reversible; from the effect otherwise), triggers (tokens and phrases),
target classes and spoken values — and an app its `AbilitySchema` (title, summary, aliases, paradigm,
discipline, the surface fields it perceives). `sk_ability_records` turns the registry into threadd
`deposit` requests: one `ability` document per skill (the app `offers` the skill, the skill `effects` its
effect), one `ability-schema` manifest per app (what it `perceives`) and one document per discipline
naming the apps that realize it, in groups minted as `ThreadMemoryTopology.abilityGroup` mints them
(`mary-ability-<fnv(owner|app|paradigm)>`), so a second deposit replaces the first.

Status: working. Prefix `sk_`.
