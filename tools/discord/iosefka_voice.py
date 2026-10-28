"""Iosefka's voice: every public text the clinic bot writes. Short, warm, kind (the real Iosefka of the
clinic, never the Imposter). Pure functions; each result fits one Discord message (<= 2000 chars).
Personal data is a wound left open: names and addresses are taken inside, never repeated in public."""
try:
    import clinic_logic as _logic
except ImportError:  # loaded by path without tools/discord on sys.path
    import importlib.util
    from pathlib import Path
    _spec = importlib.util.spec_from_file_location("clinic_logic", Path(__file__).with_name("clinic_logic.py"))
    _logic = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(_logic)

LIMIT = 2000
QUOTE_FLOOR = 40


def _fit(text: str, limit: int = LIMIT) -> str:
    return text if len(text) <= limit else text[:limit - 1].rstrip() + "…"


def _quote(text: str, room: int) -> str:
    """The author's words, IPs masked, mass pings defused, as a block quote of at most `room` chars."""
    text = _logic.mask_ips(text or "").strip()
    text = text.replace("@everyone", "@​everyone").replace("@here", "@​here")
    if not text or room < QUOTE_FLOOR:
        return ""
    lines = ["> " + ln if ln.strip() else ">" for ln in text.splitlines()]
    out = "\n".join(lines)
    if len(out) > room:
        out = out[:room - 1].rstrip() + "…"
    return out


def replacement(mention: str, bug_id: str, text: str, media_note: str = "") -> str:
    head = (f"{mention} brought a log to my clinic. I've taken it inside, where patients aren't exposed to "
            f"infection: names and addresses stay off the open streets. It's being triaged as {bug_id}. "
            f"Thank you, truly. Now, go. And good hunting.")
    media_note = media_note[:300]  # our own short note; never let it push the message over the limit
    tail = f"\n\n{media_note}" if media_note else ""
    room = LIMIT - len(head) - len(tail) - 2
    quote = _quote(text, room)
    return head + (f"\n\n{quote}" if quote else "") + tail


def replacement_many(mention: str, bug_id: str, texts: list, media_note: str = "") -> str:
    """One note for a run of messages by the same author about the same bug. One message: exactly
    `replacement`. Several: "brought N logs", then each non-empty masked text as its own quote block, the
    room shared fairly between them so the whole note stays <= 2000 chars."""
    n = len(texts)
    if n <= 1:
        return replacement(mention, bug_id, texts[0] if texts else "", media_note)
    head = (f"{mention} brought {n} logs to my clinic. I've taken them inside, where patients aren't exposed to "
            f"infection: names and addresses stay off the open streets. They're being triaged as {bug_id}. "
            f"Thank you, truly. Now, go. And good hunting.")
    media_note = media_note[:300]
    tail = ("\n\n" + media_note) if media_note else ""
    full = [_quote(t, LIMIT) for t in texts]
    full = [q for q in full if q]
    room = LIMIT - len(head) - len(tail) - 2 * len(full)
    lens = [len(q) for q in full]
    take = [0] * len(full)
    left, k = room, len(full)
    for i in sorted(range(len(full)), key=lambda j: lens[j]):  # short quotes whole, long ones share the rest
        take[i] = min(lens[i], max(left, 0) // k)
        left -= take[i]
        k -= 1
    quotes = []
    for t, tk in zip([t for t in texts if _quote(t, LIMIT)], take):
        q = _quote(t, LIMIT if tk >= len(_quote(t, LIMIT)) else tk)  # whole when it fits (the floor is for cuts)
        if q:
            quotes.append(q)
    out = head + "".join("\n\n" + q for q in quotes) + tail
    return _fit(out)


def bug_filed(bug_id: str, missing: list[str]) -> str:
    text = f"Thank you. I've written this down as {bug_id}, and the developers will look at it."
    asks = []
    if "host" in missing:
        asks.append("your host's log")
    if "guest" in missing:
        asks.append("the other guests' logs")
    if asks:
        text += (" To treat this properly I need the other half of that night: could you bring me "
                 + " and ".join(asks) + " from that session too? I'll keep them safe.")
    text += " This night is long, but morning always comes."
    return _fit(text)


def added_to(bug_id: str) -> str:
    return _fit(f"These details are added to {bug_id}, which I already have on the table. Thank you for coming back.")


def network_help(diagnosis: str) -> str:
    tail = "\n\nTell me what you tried, and I'll do what I can."
    head = "I looked at your log. Connections can be stubborn things; here is what I found:\n\n"
    room = LIMIT - len(head) - len(tail)
    body = _logic.mask_ips(diagnosis or "").strip() or "Nothing obvious is wrong, but I may be missing something."
    return head + _fit(body, room) + tail


def dm_ack(bug_id: str) -> str:
    return _fit(f"I have your log, and it is safe with me. I've filed it as {bug_id}; the developers will see "
                f"it. If there is more to tell, write to me any time. I pray for your safety.")


def told(text: str) -> str:
    """A developer's note to the player (`--tell`), spoken by Iosefka. IPs masked."""
    body = _logic.mask_ips(text or "").strip()
    head = "A word from the developers, passed on from my clinic:\n\n"
    return head + _fit(body, LIMIT - len(head))


def announcement() -> str:
    text = (
        "**Iosefka's clinic is open.**\n\n"
        "I have finished my first rounds of this server. Logs are a kind of wound: they carry names and "
        "addresses, and I don't want any of you left exposed to infection. So from now on, every log "
        "(and file like it) posted anywhere on this server is taken into my clinic automatically, where it "
        "is kept safe, and I leave a short "
        "note in its place.\n\n"
        "**What happens**\n"
        "- Every report gets an **NW id**, and the developers triage it. Nothing is lost; it is only "
        "kept indoors.\n"
        "- Trouble connecting? I look at your log first and tell you what I can see.\n"
        "- If half of a session is missing (the host's log, or the guests'), I will ask for the other "
        "players' logs too. I'll keep them safe as well.\n"
        "- Pictures and videos stay with the report, so the developers see what you saw.\n\n"
        "**Talking to me**\n"
        "Mention me or write to me privately, any time, about a log or a problem. You don't have to "
        "wait for a clean answer; bring what you have.\n\n"
        "This night is long, but morning always comes. I pray for your safety."
    )
    assert len(text) <= LIMIT
    return text
