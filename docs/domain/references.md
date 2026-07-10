# Working with the ASAM references

*How RoadMaker contributors obtain, read, and cite the ASAM standards texts.
Standards behavior is implemented from the normative text — never from
memory.*

## Which versions we target

| Standard | Version | Role |
|---|---|---|
| ASAM OpenDRIVE | **1.9.0** | Current spec — the normative reference we consult and converge toward |
| ASAM OpenDRIVE | **1.8.1** | Ecosystem baseline — what CARLA, esmini, and most consumers target |
| ASAM OpenSCENARIO XML | **1.4.0** | Later milestones (scenario authoring) |

The reader currently accepts OpenDRIVE 1.6/1.7 files and the writer emits
1.7 for maximum consumer compatibility — see the
[reader and writer stance](opendrive.md#reader-and-writer-stance).

## Obtaining the spec texts

ASAM's license does not permit redistributing the standards, so the texts
are **never committed to this repository**. A fetch script downloads the
publicly available specification pages and converts them into local,
searchable text:

```sh
python scripts/fetch_asam_specs.py --std all --out references/asam
```

- `--std` selects `opendrive`, `openscenario`, or `all`.
- `--out` is where the texts land — pick any local, **gitignored** directory.

The script writes one folder per spec version (`opendrive-1.9.0/`,
`opendrive-1.8.1/`, `openscenario-xml-1.4.0/`), each containing the chapters
as Markdown plus an `INDEX.md` with a chapter map and a topic → file lookup
table. Start every spec question at the `INDEX.md`.

## The working rule

**Before implementing or modifying anything that touches** xodr parsing or
writing, validation, the road data model, lane/junction semantics, or (later)
OpenSCENARIO:

1. Open the relevant folder's `INDEX.md` and find the chapter.
2. Read the normative text.
3. Then write the code.

Do not implement standards behavior from recollection — OpenDRIVE is full of
defaults, sign conventions, and edge cases that memory gets subtly wrong.
When your recollection and the local reference text disagree, the reference
text wins.

## Rule-id citations

Validator and parser diagnostics cite the normative checker rule whenever one
exists, via `Diagnostic::rule_id`
([diagnostics](../architecture/kernel.md#opendrive-io)):

```text
asam.net:xodr:1.4.0:ids.id_unique_in_class
```

Conventions:

- UIDs follow the ASAM checker-rule format
  `<emanating-entity>:<standard>:<definition-setting>:<rule_set.rule_name>`
  from the spec's normative annex (Annex E in 1.8.1, Annex F in 1.9.0).
- The version component is the spec version the rule **first appeared in**,
  not the version of the file being checked — a `1.4.0`-stamped UID is the
  current identifier even when reading a 1.7 file.
- Known UIDs are centralized as constants in
  `core/include/roadmaker/xodr/rules.hpp`, each with the rule text quoted
  verbatim. Add new rules there, never as inline string literals.
- When no normative rule applies (tool limitations, schema-level defects),
  `rule_id` stays empty.

## Version conflicts (1.8.1 vs 1.9.0)

Where the two OpenDRIVE versions differ, the difference is handled
**explicitly and tested** — never resolved by silently picking one:

- the code comment at the decision point cites **both** chapters,
- a test pins the chosen behavior for each version.

## Cross-checking interpretations

For genuinely ambiguous spec passages,
[libOpenDRIVE](https://github.com/pageldev/libOpenDRIVE) (Apache-2.0) is a
useful *interpretation cross-check* — read how it resolves the ambiguity and
compare. It is a reference for reading only: RoadMaker's data model and code
are entirely our own, and no code is copied from it
([dependency policy](../standards/dependencies.md)).
