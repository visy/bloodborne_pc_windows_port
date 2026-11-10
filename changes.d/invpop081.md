### Fixed
- **a wrong "The invasion is over" box at the title when the host is not ready yet**. An invader that dialled
  before the host was in a world (still at the title, loading, in a cutscene or a boss fight, travelling, dead, or
  the host's game full or already holding an invader) got a modal party notice saying the invasion was over, though
  no invasion had started. Such a refusal is only the host's moment: the invader now shows a quiet status line
  ("Waiting for the host's world: ...") and asks again by itself, after 5, 10, 20, 30, 45, then every 60 seconds
  (a minute at least after "too many attempts"). The notice stays for a final refusal (Allow invaders off, blocked,
  Stay in my world on either side, level range, a failed handshake) and now reads "Can't invade: ...".
  "The invasion is over" is only said to an invader that was in the host's world and was sent home.
  The log says `invasion refused for now (...): asking again in N s` and `refusal notice (modal): ...`.
