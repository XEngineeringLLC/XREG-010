#!/bin/zsh
# GX app changelog entry generator, run by publish_gx.sh once a release is on the OTA server; generate_changelog.sh is
# the firmware's, and the model run, its tools and the house rules follow it.
# Base = the highest gxapp_versions.json version below this one, diffed from its newest PUBLISHED_gx<version> commit
# (the xregbackup2 snapshot publish_gx.sh takes). A version published again is a reissue: same base, and its entry
# replaces the earlier one. No lower version: the entry is the first release's, written from the app's sources.
#
#   gx_app/gx_changelog.sh                                   # current tree vs the previous published version
#   gx_app/gx_changelog.sh --base <sha> --base-version 0.2   # this base instead of the manifest's
#   gx_app/gx_changelog.sh --manifest <file>                 # this gxapp_versions.json, not ota.xengineering.net's
#   gx_app/gx_changelog.sh --entry <file>                    # write the entry there (publish_gx.sh: the release record)
#   gx_app/gx_changelog.sh --dry-run                         # print the entry, write nothing
#
# The docs page gets the entry only if it exists: a new public page is the owner's to create. The last line printed
# on success starts "Changelog: " and says what was written; publish_gx.sh shows it in its summary.
# CHANGELOG_MODEL overrides the model (default: opus).

set -e

REPO="${0:A:h:h}"
PAGE="$HOME/Documents/xengineering-site/documentation_site/docs/software/gx-app-changelog.md"
MANIFEST_URL="https://ota.xengineering.net/gxapp_versions.json"
MODEL="${CHANGELOG_MODEL:-opus}"

BASE=""
BASEVER=""
MANIFEST_FILE=""
ENTRY=""
DRYRUN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --base)         BASE="$2"; shift 2 ;;
    --base-version) BASEVER="$2"; shift 2 ;;
    --manifest)     MANIFEST_FILE="$2"; shift 2 ;;
    --entry)        ENTRY="$2"; shift 2 ;;
    --dry-run)      DRYRUN=1; shift ;;
    *) echo "gx_changelog.sh: unknown argument '$1'"; exit 1 ;;
  esac
done
[[ -n "$MANIFEST_FILE" ]] && MANIFEST_FILE=${MANIFEST_FILE:a}
[[ -n "$ENTRY" ]] && ENTRY=${ENTRY:a}

cd "$REPO"

VER=$(tr -d ' \r\n' < gx_app/xregulator/VERSION)
[[ "$VER" =~ '^[0-9]+(\.[0-9]+)*$' ]] || { echo "ERROR: gx_app/xregulator/VERSION holds no version ('$VER')"; exit 1 }

if [[ -n "$BASE" || -n "$BASEVER" ]]; then
  [[ -n "$BASE" && -n "$BASEVER" ]] || { echo "ERROR: --base and --base-version go together"; exit 1 }
  SHA=$(git rev-parse --verify -q "$BASE^{commit}") || { echo "ERROR: --base $BASE is not a commit"; exit 1 }
  BASE="$SHA"
fi

WORK=$(mktemp -d -t gx_changelog)
# Keep the work dir on failure: the model run is the expensive part of this script and
# must survive a splice error, a bad flag, or the file being edited while it runs.
cleanup() {
  local rc=$?
  if [[ $rc -eq 0 || ! -e "$WORK/claude.log" ]]; then
    rm -rf "$WORK"
  else
    echo "gx_changelog.sh: FAILED (exit $rc). Model output kept at: $WORK"
  fi
}
trap cleanup EXIT

# No manifest yet (HTTP 400/404) means nothing is published; any other failed fetch must not pass for that, or
# the entry would claim a first release.
if [[ -z "$MANIFEST_FILE" ]]; then
  MANIFEST_FILE="$WORK/gxapp_versions.json"
  HTTP=$(curl -s --retry 3 --max-time 15 -w '%{http_code}' "$MANIFEST_URL?r=$RANDOM" -o "$MANIFEST_FILE" || echo 000)
  if [[ "$HTTP" == 400 || "$HTTP" == 404 ]]; then
    echo '{"versions": {}}' > "$MANIFEST_FILE"
  elif [[ "$HTTP" != 200 ]]; then
    [[ -n "$BASE" ]] || { echo "ERROR: $MANIFEST_URL fetch failed (HTTP $HTTP) -- retry, or pass --base <sha> --base-version <v>"; exit 1 }
    echo "Changelog: WARNING: $MANIFEST_URL fetch failed (HTTP $HTTP): the entry carries today's date and no build."
    echo '{"versions": {}}' > "$MANIFEST_FILE"
  fi
fi
[[ -f "$MANIFEST_FILE" ]] || { echo "ERROR: manifest $MANIFEST_FILE not found"; exit 1 }

# "<highest listed version below VER or -> <VER's build or ->"
INFO=$(python3 -c '
import json, re, sys
key = lambda s: tuple(int(x) for x in s.split("."))
vs = {v: e for v, e in json.load(open(sys.argv[1]))["versions"].items() if re.fullmatch(r"[0-9]+(\.[0-9]+)*", v)}
lower = [v for v in vs if key(v) < key(sys.argv[2])]
e = vs.get(sys.argv[2])
print(max(lower, key=key) if lower else "-", e.get("build", "-") if isinstance(e, dict) else "-")' "$MANIFEST_FILE" "$VER") \
  || { echo "ERROR: $MANIFEST_FILE is not a readable gxapp_versions.json"; exit 1 }
LOWER=${INFO%% *}
THISBUILD=${INFO##* }

if [[ -z "$BASE" && "$LOWER" != "-" ]]; then
  BASEVER="$LOWER"
  BASE=$(git log --format='%H %s' | awk -v a="PUBLISHED_gx${BASEVER}+" -v b="PUBLISHED_gx${BASEVER}_" \
    'index($2, a) == 1 || index($2, b) == 1 { print $1; exit }')
  [[ -n "$BASE" ]] || { echo "ERROR: $BASEVER is published but no commit subject starts PUBLISHED_gx${BASEVER} -- pass --base <sha> --base-version $BASEVER"; exit 1 }
fi

if [[ "$THISBUILD" == <-> ]]; then
  RELEASED="Released $(date -u -r "$THISBUILD" "+%B %-d, %Y"), build $(date -u -r "$THISBUILD" "+%Y-%m-%d %H:%M") UTC."
else
  RELEASED="Released $(date "+%B %-d, %Y")."
fi

# gui-v2/xregulator.json is plugin/ compiled to base64: the QML diff already says what it changes
DIFF_PATHS=(gx_app/xregulator gx_app/plugin ':(exclude)gx_app/xregulator/gui-v2/xregulator.json')

ABOUT="The XREG-010 GX app runs on a Victron GX device (Venus OS) and ships as one package under one
version number: the app's pages on the GX display and the remote console (QML, under plugin/), and
a background service, the bridge (Python, under xregulator/bridge/), that finds XREG-010 regulators
on the boat's network, connects to the ones the owner picks, and carries their readings, controls
and settings between the regulator and the GX. xregulator/install.sh installs, updates and removes
the app on the GX."

HOUSE="House style:
  - No emojis anywhere.
  - No marketing voice, no \"we are excited\", no exclamation marks.
  - Never describe a limitation as a feature, and do not advertise what the app
    still cannot do.
  - Do not mention internal process: builds, backups, git, snapshots, the release
    publishing pipeline itself, or anything about how the code is developed.
  - Do not mention file names or line counts.
  - Never use \"own\" as filler after a possessive (\"the GX's own screen\", \"the regulator's
    own sensors\"): write \"the GX's screen\".
  - Field Control has two modes: write \"Auto mode\" and \"Manual mode\", never \"PID mode\",
    \"automatic mode\" or \"AUTO\". The voltage loop's CV Gain Source is \"Measured\" or \"Manual\"."

if [[ -z "$BASE" ]]; then
  # First release: nothing to diff against, so the model reads the app as released
  for f in ${(f)"$(git ls-files --cached --others --exclude-standard -- "${DIFF_PATHS[@]}" ':(exclude)gx_app/xregulator/bridge/ext')"}; do
    [[ -f "$f" ]] || continue
    mkdir -p "$WORK/sources/${${f#gx_app/}:h}"
    cp "$f" "$WORK/sources/${f#gx_app/}"
  done
  echo "Changelog: first release $VER, summarized from its sources with model '$MODEL'..."
  LEAD="First release."
  SPAN="first release"
  DETAIL=0

  cat > "$WORK/prompt.txt" <<PROMPT
You are writing the public release notes for version $VER, the first release of the
XREG-010 GX app.

$ABOUT

In this directory:
  sources/       the app as released: sources/plugin/ (the GX pages) and sources/xregulator/
                 (the bridge, the install script and the service files). Grep and read them.

Read enough of the sources to describe the release accurately. Do not guess: if you cannot
tell what something does, leave it out rather than inventing a purpose for it.

Write exactly one file in this directory:

summary.md -- what a boat owner with a Victron GX device reads before deciding to install
the app. Two to four sentences of plain English prose. No headings, no bullets, no version
number, no code identifiers, no jargon. Lead with what the app lets an owner do from the GX.
The entry already says this is the first release; do not repeat it.

$HOUSE

Write the file, then reply with just: DONE
PROMPT
else
  git diff -w "$BASE" -- "${DIFF_PATHS[@]}" > "$WORK/diff.patch"
  git diff --stat "$BASE" -- "${DIFF_PATHS[@]}" > "$WORK/diffstat.txt"
  # Files created since the last xregbackup2 are untracked, and git diff <commit> skips them.
  # Read-only: never git add -N. git diff --no-index exits 1 whenever the files differ.
  for f in ${(f)"$(git ls-files --others --exclude-standard -- "${DIFF_PATHS[@]}")"}; do
    git diff --no-index -w /dev/null "$f" >> "$WORK/diff.patch" || (( $? == 1 ))
    git diff --no-index --stat /dev/null "$f" >> "$WORK/diffstat.txt" || (( $? == 1 ))
  done
  git log --format='- %s' "$BASE..HEAD" -- "${DIFF_PATHS[@]}" > "$WORK/commits.txt" 2>/dev/null || true

  if [[ ! -s "$WORK/diff.patch" ]]; then
    echo "Changelog: no app source changes between $BASEVER and $VER, no entry written"
    exit 0
  fi

  LINES=$(wc -l < "$WORK/diff.patch" | tr -d ' ')
  echo "Changelog: summarizing $BASEVER -> $VER ($LINES diff lines) with model '$MODEL'..."
  LEAD="Changes since $BASEVER."
  SPAN="changes since $BASEVER"
  DETAIL=1

  cat > "$WORK/prompt.txt" <<PROMPT
You are writing the public release notes for version $VER of the XREG-010 GX app, covering
everything that changed since version $BASEVER.

$ABOUT

In this directory:
  diff.patch     the full unified diff of the app's sources (the GX pages under plugin/, and
                 under xregulator/ the bridge, the install script and the service files).
                 $LINES lines -- read it in chunks with Read offset/limit, and grep it for
                 orientation.
  diffstat.txt   per-file change counts
  commits.txt    the commit subjects in the range (often useless placeholder dates -- ignore
                 them unless they actually say something)

Read enough of the diff to describe the release accurately. Do not guess: if you cannot
tell what a change does, leave it out rather than inventing a purpose for it.

Write exactly two files in this directory:

1. summary.md -- what a boat owner with the app on their Victron GX device reads before
   deciding to update. Two to four sentences of plain English prose. No headings, no bullets,
   no version number restated, no code identifiers, no jargon. Lead with the change that
   matters most to someone using the app on the boat. If the release is mostly internal, say
   so plainly.

2. detail.md -- the full breakdown, for someone who wants specifics. Markdown, grouped
   under level-4 headings (####) by area, for example: Pages and display, Regulator settings,
   Plots, Connecting to regulators, GX integration, Updates and installation, Internal. Only
   include headings that actually have content. Bullets under each. Plain English first with
   the technical term in parentheses after it, e.g. "current-limited phase (CC phase)".
   Naming functions, settings and constants is fine and welcome here. Keep each bullet to one
   or two lines.

$HOUSE

Write the two files, then reply with just: DONE
PROMPT
fi

(
  cd "$WORK"
  # File tools only, no MCP: background subagents can keep claude -p from exiting and hang the publish.
  claude -p --model "$MODEL" --output-format json --tools "Read,Grep,Glob,Write,Edit" --strict-mcp-config \
    --dangerously-skip-permissions "$(cat "$WORK/prompt.txt")" < /dev/null > "$WORK/claude.log" 2>&1
) || { echo "ERROR: claude -p failed. Log:"; tail -30 "$WORK/claude.log"; exit 1 }

python3 -c "
import json,sys
try:
    d=json.load(open('$WORK/claude.log'))
    print('Changelog: model run %.1fs, \$%.2f' % (d.get('duration_ms',0)/1000.0, d.get('total_cost_usd',0)))
except Exception:
    pass" 2>/dev/null || true

if [[ ! -s "$WORK/summary.md" || ( $DETAIL == 1 && ! -s "$WORK/detail.md" ) ]]; then
  echo "ERROR: the model did not produce summary.md (and, for a changes entry, detail.md). Log tail:"
  tail -30 "$WORK/claude.log"
  exit 1
fi

# Assemble the page section. The detail block is indented 4 spaces because pymdownx.details
# ("???") takes its body as an indented block.
{
  echo "## $VER"
  echo ""
  echo "*$RELEASED $LEAD*"
  echo ""
  cat "$WORK/summary.md"
  echo ""
  if (( DETAIL )); then
    echo "??? note \"Full detail\""
    echo ""
    sed 's/^/    /; s/[[:space:]]*$//' "$WORK/detail.md"
    echo ""
  fi
  echo "---"
  echo ""
} > "$WORK/entry.md"

if [[ $DRYRUN -eq 1 ]]; then
  echo ""
  cat "$WORK/entry.md"
  echo "Changelog: dry run, nothing written"
  exit 0
fi

WROTE=()
if [[ -n "$ENTRY" ]]; then
  mkdir -p "${ENTRY:h}"
  cp "$WORK/entry.md" "$ENTRY"
  WROTE+=("$ENTRY")
fi

if [[ -f "$PAGE" ]]; then
  # Splice: an existing section for this version is replaced where it stands; a new version
  # goes above the first older one, so the page stays newest-first even for a backfill.
  ACTION=$(python3 - "$PAGE" "$WORK/entry.md" "$VER" <<'PY'
import re, sys
page_path, entry_path, ver = sys.argv[1], sys.argv[2], sys.argv[3]
page = open(page_path, encoding='utf-8').read()
entry = open(entry_path, encoding='utf-8').read()
MARKER = '<!-- CHANGELOG-ENTRIES -->'
if MARKER not in page:
    sys.exit(f"ERROR: marker {MARKER} missing from {page_path}")

head, tail = page.split(MARKER, 1)
tail = tail.lstrip('\n')
key = lambda s: tuple(int(x) for x in s.split('.'))

# Remove an existing section for this exact version (heading through the next "## ").
pat = re.compile(r'^## ' + re.escape(ver) + r'\s*$.*?(?=^## |\Z)', re.M | re.S)
first = pat.search(tail)
if first:
    pos = first.start()
    tail = pat.sub('', tail)
    print('replaced')
else:
    pos = len(tail)
    for m in re.finditer(r'^## (\d+(?:\.\d+)*)\s*$', tail, re.M):
        if key(m.group(1)) < key(ver):
            pos = m.start()
            break
    print('added')

before = tail[:pos]
if before and not before.endswith('\n\n'):
    before = before.rstrip('\n') + '\n\n'
open(page_path, 'w', encoding='utf-8').write(head + MARKER + '\n\n' + before + entry + tail[pos:])
PY
)
  WROTE+=("$PAGE ($ACTION there; run deploy-all to publish it)")
  PAGE_NOTE=""
else
  PAGE_NOTE="; no docs page has it (${PAGE:t} does not exist)"
fi

if (( ${#WROTE} )); then
  echo "Changelog: $VER entry ($SPAN) written to ${(j:, :)WROTE}$PAGE_NOTE"
else
  cat "$WORK/entry.md"
  echo "Changelog: $VER entry ($SPAN) printed above only: no --entry given$PAGE_NOTE"
fi
