"""Outbox coalescing: one person never gets the same tell / ask twice, or once per bug.

A pending row is keyed by (kind, recipient, dm, normalised text). Rows with the same key go out as ONE message that
mentions the person once and lists every bug id; a key already delivered within the window is not sent again."""
import json
import os
import re
import time

import clinic_asks as asks

WINDOW_HOURS = 24.0


def window_seconds():
    try:
        return float(os.environ.get("NW_TELL_WINDOW_HOURS", WINDOW_HOURS)) * 3600
    except ValueError:
        return WINDOW_HOURS * 3600


def norm(text):
    return re.sub(r"\s+", " ", (text or "")).strip().lower()


def key_of(clinic, row):
    """(kind, person, dm, text) for an outbox row, or None when there is nobody to group by."""
    bug = clinic.bug(row["bug_id"])
    if not bug:
        return None
    kind = row.get("kind") or "tell"
    person = bug.get("reporter_id") or asks.reporter_of(clinic, bug)
    if not person or person < 0:
        return None
    text = row["text"]
    if kind == "ask":
        try:
            a = json.loads(text)
            text = (a.get("issue") or "") + "\n" + (a.get("need") or "")
        except ValueError:
            pass
    return (kind, person, bug["source"] == "dm", norm(text))


def shown_id(row):
    """The bug id the message is about (an ask names its own bug, which may differ from the record's)."""
    if (row.get("kind") or "tell") == "ask":
        try:
            return json.loads(row["text"])["bug"]
        except (ValueError, KeyError):
            pass
    return row["bug_id"]


def ids_phrase(ids):
    ids = list(dict.fromkeys(ids))
    return ids[0] if len(ids) == 1 else ", ".join(ids[:-1]) + " and " + ids[-1]


def recent_delivery(clinic, key, now=None, exclude=()):
    """The newest row actually posted within the window with the same key, or None."""
    now = now or time.time()
    for r in reversed(clinic.outbox_delivered_since(now - window_seconds())):
        if r["id"] not in exclude and key_of(clinic, r) == key:
            return r
    return None


def groups(clinic, rows):
    """[[rows]]: pending rows grouped by key, each group in id order; rows without a key stand alone."""
    out, index = [], {}
    for r in rows:
        k = key_of(clinic, r)
        if k is None:
            out.append([r])
        elif k in index:
            index[k].append(r)
        else:
            index[k] = [r]
            out.append(index[k])
    return out


def preview(clinic, bug_id, kind, text, now=None):
    """What would happen to a tell/ask queued now for `bug_id`: {"merge_with": [bug ids with a pending twin],
    "already_told": {"outbox", "bug", "hours_ago"} | None}. Used by the CLI for dry-run output and warnings."""
    now = now or time.time()
    row = {"bug_id": bug_id, "text": text, "kind": kind}
    k = key_of(clinic, row)
    if k is None:
        return {"merge_with": [], "already_told": None}
    twins = [shown_id(r) for r in clinic.outbox_pending() if key_of(clinic, r) == k]
    rec = recent_delivery(clinic, k, now)
    told = None
    if rec:
        told = {"outbox": rec["id"], "bug": shown_id(rec), "hours_ago": round((now - rec["sent"]) / 3600, 1)}
    return {"merge_with": twins, "already_told": told}


def describe(pv):
    """Human lines for a preview."""
    out = []
    if pv["merge_with"]:
        out.append("will be merged into one message with the pending text for " + ids_phrase(pv["merge_with"]))
    if pv["already_told"]:
        t = pv["already_told"]
        out.append(f"WARNING: this person already got this text {t['hours_ago']} h ago (about {t['bug']}, "
                   f"outbox {t['outbox']}); it will not be sent again, only recorded")
    return out
