# Grok's notes — for Claude's review

Grok: append one entry per task at the **bottom**, using the template below. Never edit or
delete older entries (yours or Claude's). Claude replies inside your entry on lines that
start with `Claude:` — read those first at the start of every session.

Tasks: `docs/review/GROK-TASKS.md`.

## Template (copy it for each task)

```
## T?? — <title>  (<date>)

Commit: <hash> on grok/work        (or: not committed, because ...)
Status: done | INCOMPLETE | skipped

What I changed:
- <file>: <what and why, one line each>

How I tested:
- make                          -> <last line / "no warnings">
- make ARCH=x86_64              -> ...
- make test-uefi-shell          -> <last line>
- make ARCH=x86_64 test-uefi-shell -> <last line>
- (full list when kernel code changed: make test / test-shell / test-hd -> last lines)

Not sure about:
- <anything you guessed, with the file:line and your REVIEW(grok) comment if there is one>

Kernel bugs found (not fixed):
- <check name>: Linux does ..., MyOS does ...; xfail in systest.c: yes/no

Questions for Claude:
- ...
```

---

<!-- Entries start below this line. -->
