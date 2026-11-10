# Install completion checks

Arkeyan-Builder emits `@done` only after required shader capture, translation, compilation and linking succeed.
The menu music and icon commands have their own completion events and do not install a game.

| Step | Failure or retry behavior |
| --- | --- |
| Disc extraction | Missing or changed extracted inputs invalidate the inventory and reread the disc. A reread clears the previous runtime capture. Failed writes and empty required shader archives fail the install. |
| Code translation | Child failure, missing output or headers incompatible with the kit fail the install. |
| Shader translation | Child failure or missing output fails, retaining the Windows startup hint. A cache with no translated shaders fails. Every complete retained runtime shader must be represented in the cache, including on cached installs. |
| Movie capture | Missing or malformed completion stamps cannot skip an incomplete capture. Only complete containers count. Empty and short captures fail; complete captured containers survive for retry. The poll target includes previously retained containers. |
| Rebuild after capture | Translation, compilation and linking must all succeed before `movies/done.txt` is written. A complete capture without a marker retries the rebuild without launching another capture. |
| Cross build | Incomplete runtime capture fails with instructions to finish with the native kit. |
| Compile and link | Required child failures propagate to `@error`; the install emits no `@done`. |

`@warn` reports skipped disc shaders, unsupported archive entries, missing D3D12 variants,
icon/resource fallback and low free space. Warnings remain visible even when an install succeeds.
Missing required runtime translations are fatal. A recipe without a positive runtime target warns
that completeness cannot be verified; an empty capture still fails.

`ArkeyanBuilderTest` checks metadata, strict movie stamp parsing, Windows startup hints, synthetic archive
extraction and failed output writes. `ArkeyanBuilderGuardTest` calls the real install steps with invented
inputs and intercepts every child call. It checks inventory changes, partial containers, blocked and
incomplete translation, cached translation validation, short capture retention, resume polling,
environment restoration and delayed completion markers. It also calls the install entry point with
a blocked shader translator and checks exit 1, the Windows hint, `@error` and absence of `@done`.

These synthetic checks launch no game and do not prove native Windows execution or rendering.
The separate Windows install check and `test_all` sections remain available for their documented
integration checks; reconciliation does not require running them with game data.
