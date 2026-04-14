# AGENTS.md

## Purpose

This repository is a staged refactor of WinDirStat toward a final architecture with:

- UI fully in C# / Blazor
- scan engine fully in Rust
- process boundary between UI and engine over RPC

Current C++ host-side components may be transitional scaffolding. Do not optimize for preserving legacy structure when it conflicts with the target architecture.

## Working style

Before making non-trivial changes:

- Read the local architecture around the touched files first.
- Prefer small, runnable increments.
- Preserve a working application at the end of each slice.
- Do not expand scope unless required to make the slice coherent.
- If the task is ambiguous or large, produce a short plan first.

When writing explanations or summaries for this repo:

- Prefer clean capsules over long status dumps.
- Be precise about boundaries, ownership, assumptions, and what changed.
- Avoid noise and repetition.

## Core architectural rules

### Final target

The target shape is:

- UI in C# / Blazor
- scan engine in Rust
- RPC is the process boundary
- UI observes; engine executes

Do not propose architecture that recenters scanning in MFC/UI/model code.

### Boundary discipline

The engine must not depend on UI/model types such as `CItem`.

Preferred shape:

Engine -> DTOs / protocol -> adapter/projection -> UI/model tree

Do not leak `CItem*` or other UI/model objects into:

- traversal
- queue ownership
- engine contracts
- discovery contracts
- RPC protocol
- Rust FFI seam

### Discovery seam

Discovery is not traversal.

The discovery seam answers only:

- given a directory path, what are its immediate children and relevant metadata?

Do not silently move traversal/lifecycle responsibilities into discovery code.

### Ownership model

Engine owns:

- scan execution
- traversal
- queue / scheduling
- lifecycle
- cancel / completion behavior
- logging / progress emission

UI owns:

- observation
- projection into model objects
- rendering
- user interaction

### Transitional code

Temporary bridges and adapters are acceptable when they move the codebase toward the target architecture.

But every temporary layer must be clearly marked with:

- why it exists
- what boundary it protects
- what future state removes it

Do not create “temporary” code that quietly becomes permanent architecture.

## Implementation rules

### Comments

Add comments to generated code, but only where they add value.

Comments must explain:

- intent
- contract
- assumptions
- migration status
- removal condition for temporary code

Do not add line-by-line comments that merely restate the code.

Comment these areas consistently:

- module/file purpose
- public boundary functions
- normalization logic
- RPC / process-boundary logic
- filesystem assumptions
- concurrency / lifecycle logic
- temporary migration adapters

Useful labels when appropriate:

- Intent:
- Contract:
- Assumption:
- Out of scope:
- Temporary:
- Removal condition:

### Tests

Prefer tests that validate architectural seams and contracts.

For discovery tests:

- use Rust as the long-term home of the harness
- use real filesystem temp fixtures, not mocked filesystem behavior
- create fixtures inside each test
- test immediate-child discovery only
- normalize results before comparison
- compare contract-relevant fields only

Do not start with end-to-end UI or RPC tests when the seam itself is not covered.

### Naming

Prefer names that reflect ownership and boundaries clearly.

Good names distinguish:

- discovery vs traversal
- engine vs adapter
- protocol vs model
- temporary bridge vs target implementation

Avoid names that blur layers.

### Scope control

When implementing a slice:

- keep the smallest viable boundary
- do not rewrite adjacent systems unless necessary
- avoid opportunistic cleanup that obscures the architectural move
- keep behavior runnable and testable after each step

## Repository-specific guidance

### For C++ changes

When touching legacy C++:

- treat it as transitional unless clearly part of the enduring host boundary
- avoid introducing new dependencies from engine code to `CItem`
- prefer extracting narrow contracts over extending legacy entanglement

### For Rust changes

When adding Rust code:

- prefer durable ownership of new tests and seam contracts in Rust
- keep FFI narrow and explicit
- do not let Rust take accidental ownership of concerns that still belong to the host unless that move is intentional and aligned with the roadmap

### For RPC / protocol work

Protocol changes must be explicit and version-conscious.

When changing messages or contracts:

- describe what changed
- state compatibility assumptions
- state which side owns translation if versions differ

## Definition of done

A change is not done until:

- the code builds
- relevant tests pass
- comments explain boundary assumptions where needed
- the architectural direction is improved or at least preserved
- the app remains runnable at the end of the slice

For non-trivial work, include a short completion capsule with:

- goal
- files changed
- key decisions
- assumptions
- known follow-up

## Do-not rules

Do not:

- reintroduce `CItem` into engine contracts
- blur discovery and traversal responsibilities
- move logic into the UI layer just because it is convenient
- add mock-based seam tests where real filesystem behavior is the risk
- overengineer harnesses or abstractions before the seam is proven
- leave temporary migration code undocumented
- produce noisy summaries without a clean capsule

## Preferred agent behavior

When asked for implementation help:

1. restate the slice briefly in boundary terms
2. inspect the local code before proposing changes
3. propose the smallest coherent plan
4. implement in runnable increments
5. summarize with a clean capsule

When uncertain, preserve the architecture over local convenience.