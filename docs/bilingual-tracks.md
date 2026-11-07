# Bilingual subtitle tracks

This workflow reconciles two subtitle timelines into one bilingual IR and
renders it as a sidecar ASS or SRT. The first input is the primary event
skeleton, normally English; the second supplies translations, normally
Chinese.

## ASS style sheets

Use `iris build --style-sheet path/to/theme.json` to select an external JSON
style sheet. It takes precedence over `--template`. Packaged themes are
installed beside the executable under `themes`; TADC character styles are
discovered automatically when the IR references a known TADC character style.
Without a selected theme, Iris uses the built-in default styles.

## Import and merge

```bash
iris tracks episode.mkv
iris convert episode.mkv --track lang=eng -o episode.en.iris.json
iris convert episode.mkv --track lang=chi -o episode.zh.iris.json
iris merge episode.en.iris.json episode.zh.iris.json \
  --primary-lang en --secondary-lang zh \
  --report episode.merge.html -o episode.bi.iris.json
iris build episode.bi.iris.json --format ass --bilingual -o episode.ass
iris build episode.bi.iris.json --format srt --bilingual -o episode.srt
```

For long or weakly punctuated primary cues, SaT semantic splitting is
available as an opt-in merge preprocessing step:

```bash
iris merge episode.en.iris.json episode.zh.iris.json \
  --split sat --split-model models/sat -o episode.bi.iris.json
```

The default remains `--split rule`; if the SaT model cannot be opened, merge
keeps the original cues and continues with the normal alignment path.

### Multi-speaker text

The reconciliation step recognizes repeated leading speaker labels such as
`-[Caine] Pomni. -[Ragatha] Pomni.` and compact labels such as `-A -B Hello`.
It emits one IR sentence per speaker, stores the label in `Sentence.speaker`, and
allocates the original cue duration evenly. This is an interim boundary estimate; a future
Qwen3 ForcedAligner integration can replace the timing without changing the IR
shape.

### Optional LLM post-editing

Machine translation remains the default. To optionally polish each OPUS/M2M100
result with an OpenAI-compatible local or remote chat endpoint:

```powershell
iris translate input.iris.json --model models/translation/opus-mt-en-zh-f16.gguf `
  --engine opus-mt --from en --to zh `
  --llm-endpoint http://127.0.0.1:8080/v1/chat/completions `
  --llm-model local-model --out translated.iris.json
```

The scheduler sends the original sentence and machine translation, requests
only the final subtitle text, retries transient failures, and fails explicitly
if the endpoint returns an invalid response. Omit `--llm-endpoint` to retain
the original machine-translation-only path. `IRIS_LLM_API_KEY` can provide the
Bearer token instead of `--llm-api-key`.

`convert` accepts supported text tracks and subtitle files. Track selectors can
use a stream index, `lang=...`, or `title=...`; graphical PGS/VobSub tracks
must use OCR or an external extractor.

## Reconciliation rules

`merge` performs a monotonic global alignment rather than choosing the locally
largest overlap:

| Relationship | Result |
|---|---|
| 1:1 | Attach the translation to the primary event |
| 1:n | Join translations in time order |
| n:1 | Split by duration/character proportion when possible |
| Unmatched primary | Apply the single-line policy |
| Unmatched secondary annotation | Preserve as an annotation event |

Each secondary cue is consumed at most once. Speaker labels at the beginning
of a line are stored as the sentence speaker and are emitted through the ASS
Name field rather than duplicated in the dialogue text.

Before alignment, inline SDH annotations such as `[groans]` and
`[air rushing]` are removed from mixed dialogue cues so the remaining dialogue
can match a clean translation cue. A malformed leading bracket is classified
as SDH and is not promoted to a speaker label.

Blank dash-only cues and leading speaker-switch dashes are removed before
alignment. Interjections use a stable lexical list, and an adjacent
interjection cannot inherit a duplicate translation from its neighboring
dialogue cue.

## Single-language policies

```text
--single-line sdh=glossary,interjection=drop,dialogue=keep,lyric=keep
```

`keep` emits the primary line only, `drop` removes it, and `glossary` looks up
the complete line in a TSV glossary and falls back to `keep` on a miss.
Default behavior keeps all categories. `role` values are
`dialogue`, `sdh`, `lyric`, `interjection`, and `annotation`.

## Karaoke and templates

```bash
iris karaoke episode.bi.iris.json --even --even-syllables --role lyric \
  -o episode.k.iris.json
iris build episode.k.iris.json --bilingual \
  --template docs/themes/tadc.json -o episode.k.ass
```

Karaoke prefers word timings from forced alignment. Without them it uses
weighted even timing; translated lyric lines receive projected timings.
`lint` checks that the `\K` sum matches the event duration.

Templates can be JSON or an existing ASS file. `iris style extract` converts
an ASS style table to a reusable JSON template.

## Limitations

- Text-track timing is retained on the primary skeleton; dense sections may
  require manual CPS and line-break review.
- Graphical subtitle tracks are not parsed by `convert`.
- Speaker colors and role-specific styling require reliable speaker metadata or
  manual assignment.
