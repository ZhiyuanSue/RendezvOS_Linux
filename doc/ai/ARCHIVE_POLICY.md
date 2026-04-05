# Assist History Archive Policy

This policy keeps `doc/ai/ASSIST_HISTORY.md` compact while preserving full history.

## Rotation Trigger

Rotate when either condition is met:

- `ASSIST_HISTORY.md` has more than 50 entries, or
- file size exceeds 400 KB.

## Rotation Rules

1. Keep recent entries (about last 30-50) in `ASSIST_HISTORY.md`.
2. Move older entries into monthly archive files:
   - `doc/ai/archive/ASSIST_HISTORY_YYYY-MM.md`
3. Do not rewrite historical meaning; only allow factual typo fixes.
4. Add/update an index section in `ASSIST_HISTORY.md` linking archives.
5. Rotation should be done in the same commit that crosses threshold (or immediately next commit).

## Archive File Format

Each archive file should contain:

- Title with month range.
- Entry blocks copied from `ASSIST_HISTORY.md` unchanged.
- Optional short note explaining why rotation occurred.

## Minimal Rotation Checklist

- [ ] Threshold met and checked.
- [ ] Older entries moved into correct month file.
- [ ] `ASSIST_HISTORY.md` archive index updated.
- [ ] Latest entries preserved in root history file.

