"""Iosefka's voice: every public text the clinic bot writes. Short, warm, kind (the real Iosefka of the
clinic, never the Imposter). Pure functions; each result fits one Discord message (<= 2000 chars).
Personal data is a wound left open: names and addresses are taken inside, never repeated in public."""
import re
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


SELF_ID = None  # Iosefka's own Discord user id; set once the client is ready (tests set it directly)
_SELF_NAME = re.compile(r"@\s*Net\s*Doctor\s*Iosefka", re.I)


def strip_self(text: str, self_id=None) -> str:
    """The text without mentions of Iosefka herself; "" when nothing meaningful (a letter or digit) is left."""
    text = text or ""
    sid = self_id or SELF_ID
    if sid:
        text = re.sub(rf"<@!?{int(sid)}>", "", text)
    text = _SELF_NAME.sub("", text)
    return text.strip() if re.search(r"\w", text) else ""


def _quote(text: str, room: int) -> str:
    """The author's words, IPs masked, mass pings defused, as a block quote of at most `room` chars."""
    text = _logic.mask_ips(strip_self(text)).strip()
    text = text.replace("@everyone", "@​everyone").replace("@here", "@​here")
    if not text or room < QUOTE_FLOOR:
        return ""
    lines = ["> " + ln if ln.strip() else ">" for ln in text.splitlines()]
    out = "\n".join(lines)
    if len(out) > room:
        out = out[:room - 1].rstrip() + "…"
    return out


SAVE_COMFORT = "I'll keep them safe; the developers use them to see what happened."


def _what(what: str, n: int = 1) -> tuple[str, str, str]:
    """(what was brought, "it"/"them", reassurance) for `what` = log / save / both."""
    if what == "save":
        return "their saves", "them", " " + SAVE_COMFORT
    if what == "both":
        return "logs and saves", "them", " " + SAVE_COMFORT
    return ("a log", "it", "") if n <= 1 else (f"{n} logs", "them", "")


def replacement(mention: str, bug_id: str, text: str, media_note: str = "", what: str = "log") -> str:
    thing, it, comfort = _what(what)
    head = (f"{mention} brought {thing} to my clinic. I've taken {it} inside, where patients aren't exposed to "
            f"infection: names and addresses stay off the open streets.{comfort} It's being triaged as {bug_id}. "
            f"Thank you, truly. Now, go. And good hunting.")
    media_note = media_note[:300]  # our own short note; never let it push the message over the limit
    tail = f"\n\n{media_note}" if media_note else ""
    room = LIMIT - len(head) - len(tail) - 2
    quote = _quote(text, room)
    return head + (f"\n\n{quote}" if quote else "") + tail


def replacement_short(mention: str, bug_id: str, text: str, media_note: str = "", what: str = "log") -> str:
    """One line for a repeat log of the same author on the same bug, or from a dev / staff poster."""
    if what == "log":
        variants = (f"Another log from {mention} taken into my clinic with {bug_id}. Thank you.",
                    f"{mention}, one more log, safely inside with {bug_id}. Thank you.",
                    f"Another of {mention}'s logs is now in my care, filed under {bug_id}. Thank you.")
    else:
        noun = "saves" if what == "save" else "logs and saves"
        variants = (f"Another of {mention}'s {noun} taken into my clinic with {bug_id}. Thank you.",
                    f"{mention}, more {noun}, safely inside with {bug_id}. Thank you.",
                    f"More {noun} from {mention} are now in my care, filed under {bug_id}. Thank you.")
    head = variants[sum(map(ord, bug_id)) % len(variants)]
    media_note = media_note[:300]
    tail = "\n\n" + media_note if media_note else ""
    quote = _quote(text, LIMIT - len(head) - len(tail) - 2)
    return head + ("\n\n" + quote if quote else "") + tail


def replacement_many(mention: str, bug_id: str, texts: list, media_note: str = "", what: str = "log") -> str:
    """One note for a run of messages by the same author about the same bug. One message: exactly
    `replacement`. Several: "brought N logs", then each non-empty masked text as its own quote block, the
    room shared fairly between them so the whole note stays <= 2000 chars."""
    n = len(texts)
    if n <= 1:
        return replacement(mention, bug_id, texts[0] if texts else "", media_note, what)
    thing, it, comfort = _what(what, n)
    head = (f"{mention} brought {thing} to my clinic. I've taken {it} inside, where patients aren't exposed to "
            f"infection: names and addresses stay off the open streets.{comfort} They're being triaged as {bug_id}. "
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


def bug_filed(bug_id: str, missing: list[str], what: str = "log") -> str:
    text = f"Thank you. I've written this down as {bug_id}, and the developers will look at it."
    if what == "save":  # saves alone: no host / guest logs to ask for
        return _fit(text + " Your saves are safe with me; the developers use them to see what happened. "
                    "This night is long, but morning always comes.")
    if what == "both":
        text += " Your saves are safe with me; the developers use them to see what happened."
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


def retest_ask(mention: str, bug_id: str, version: str) -> str:
    """After a release: the reporter of a fixed bug is asked to try the new version."""
    mention = (mention + " ") if mention else ""
    return _fit(f"{mention}The developers say what you brought me as {bug_id} is mended in {version}. "
                f"Would you try it, when you have a moment, and tell me if it still happens? "
                f"If it does, say so here and I will reopen your case. Thank you for your patience.")


def logs_nudge(mention: str, bug_id: str) -> str:
    """A bug has waited for logs for two weeks: one gentle word to its reporter."""
    mention = (mention + " ") if mention else ""
    return _fit(f"{mention}I still have {bug_id} on my table, waiting for a few more logs from that night "
                f"(from the host and the guests, if you can). Bring them whenever you are able and I will take "
                f"them in. There is no hurry; I only did not want you to think I had forgotten.")


def announcement() -> str:
    text = (
        "**Iosefka's clinic is open.**\n\n"
        "I have finished my first rounds of this server. Logs are a kind of wound: they carry names and "
        "addresses, and I don't want any of you left exposed to infection. So from now on, every log "
        "(and file like it) posted anywhere on this server is taken into my clinic automatically, where it "
        "is kept safe, and I leave a short "
        "note in its place.\n\n"
        "Forgive the flurry of notes in the threads today: on my first rounds I left one everywhere a log was "
        "taken in, and some of you saw a great many of them at once. It won't happen like that again.\n\n"
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
