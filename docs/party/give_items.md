# Give items to a party member (0.7.8, giveitem078, CL-63e235)

A player gives consumables or materials from the own inventory to another party member. Host to guest, guest to host
and guest to guest. Code: `gpu/shim/party/party_give_rules.h` (pure rules, wire, the two state machines; unit test
`tests/test_party_give.cpp`, `ninja -C out/gpu party-give-test`) and `party_give.{h,cpp}` (game side). The receiving
half of the inventory work is `ReadGiveFacts` / `GiveGoodsToOwn` / `TakeGoodsFromOwn` at the end of `party_items.cpp`.

## 1. Player view

* Switch `party_give_items` (System > My party: Your items, the launcher's My Party tab, bbport.ini, the overlay's Party
  tab). **Off by default.** It is per player and has two meanings: with it on you see the give rows and can send; with it
  off other hunters' gives are refused ("does not take gifts") - the consent. `BB_PARTY_GIVE_ITEMS=1` forces it on (tests).
* Insert menu > Party tab > *Give: item / how many / to* and *Give*. A compact list built from the inventory's normal
  section (`InventoryGoodsList`), refreshed by the game tick every 0.5 s while the tab is open (the render thread only
  reads a copy). The game's own inventory screen is not hooked.
* Notices (the party notice corner): giver "Gave Blood Vial x3 to *name*", receiver "Blood Vial x3 from *name*",
  refusals "Give Blood Vial to *name*: *reason*". Names go through `bbname::Display` (name filter). The receiver also
  gets the game's item-get dialog.

## 2. What may be given (`GoodsAllowed`, `GiverVerdict`, `ReceiverVerdict`)

Goods only (weapons, armor and rings are shared by the key / unique rules of items.md section 11 and are not offered).

| Rule | Reason shown |
|---|---|
| EquipParamGoods `goodsType` must be 0 (key items are 1) | key items cannot be given |
| `maxNum` must be above 1 (unique goods: badges, the Eye, tools, keys) | unique items cannot be given |
| goods 800-822 (gestures) and 4000-4999 (the key / story band: keys, hunter badges 4110-4119, Eye 4311, Old Hunter Bell 4312, workshop tools ...) | story items cannot be given |
| unknown param row or count | could not be given |
| a quick-slot item keeps at least one (the slot points at the inventory entry; a removed entry would leave it dangling) | keep one of the item in the quick slot |
| more than held / more than 99 per give | you do not hold that many |

The receiver applies the same goods rules to what the giver names, on its own param data, so a modified peer cannot make
a game take a key or unique item. It cannot check what the giver holds: a modified giver can conjure plain consumables
(Blood Vials, bullets) for a friend; that is within the trust of a private party and not worth a protocol.

The moment (`Gate`): nothing while the world is not up or loading, a party travel / re-join runs, the player is dead or
spectating, a boss health bar is up (fights and fog walk-ins), or a cutscene plays. Checked by the receiver at OFFER and
again at COMMIT, and by the giver at the offer and again right before the removal.

Room (`ReceiverVerdict`): the receiver's inventory up to `maxNum`, then its storage box (`StorageRoom`, the game's own
check); the give is refused with "no room in their inventory and storage" if the whole count does not fit, so nothing
ever lands on the ground and the game's blocking "inventory or storage max" box never opens. The apply goes through the
award's give list (`GiveGoodsViaList`) when it passes the inventory maximum, else `GiveItemDirect` + `ShowItemGet`.

## 3. The transfer (atomic across the link)

```
giver                            host (relay)               receiver
OFFER {id, goods, n}  ---------> sets from = sender ------> gate, goods rules, room
                      <--------- relays back <------------- READY {room} | REFUSE {why}      (nothing moved)
remove n; the count must drop
by exactly n (else put back, ABORT)
COMMIT {id}           ---------> ---------------------->    reservation live? give n, count verified
                      <--------- <----------------------   DONE {got, why}
refund n - got (a short DONE)
```

* The giver removes only after READY, verifies the removal (inventory count before and after; `TakeGoodsFromOwn`), and
  puts back whatever it removed if the count is not exactly n (it removed nothing or too many) and sends ABORT.
* The receiver gives only on a COMMIT of a live reservation (`kReserveS` 20 s), from the member that made it, and
  records the outcome of the last 32 transfers: a repeated COMMIT (a replay after a reconnect) or an ABORT after the
  apply answers the same DONE, a COMMIT after an ABORT or an expired reservation gives nothing (DONE got 0). So a lost
  or late message can never apply twice.
* No DONE within `kDoneWaitS` (10 s): the giver asks (ABORT). The receiver answers with the truth: applied (DONE {got}) or
  "never, and never now" (DONE 0, the id is dead). The giver refunds exactly `n - got`.
* If the receiver cannot be asked - its link is down for `kLinkDownS` (90 s) after COMMIT, or the ABORT stays unanswered
  `kAbortWaitS` (20 s) with the link up - the giver rolls the whole amount back ("no confirmation, the item was put
  back"). Link back in time: the wait starts afresh and the queued DONE arrives.
* The host is the relay: a guest's events go to the host, which sets `from` to the sender's slot (a guest cannot sign for
  another) and forwards to `to`; an offer to a missing member is answered REFUSE at once. Events are reliable and
  ordered per stream, and queued for a member that is reconnecting.
* One transfer per giver at a time; the receiver holds one reservation at a time (Busy otherwise); 1 s between gives.

Residual risks (not covered, by design): a crash of the giver's game between the removal and its next save loses the
removed stack (the game saves on its own events; the removal also asks for a save at COMMIT); a receiver that applied,
saved and then vanished for good before the DONE reached the giver, while the giver's rollback timer ran out, would
leave both with the items (needs the link down for 90 s exactly in that window).

## 4. Tests

* `party-give-test` (unit): allow / deny table, room, the gate, the wire (range checks, malformed input), the giver and
  receiver machines (happy path, refusal, no answer, removal not verified, short DONE, lost COMMIT, lost DONE, ABORT
  unanswered, link down and back, expired reservation, wrong member).
* Smoke (`tools/mp/smoke_party.py`, groups `give_items` = phases `gv` + `gvg`, `give_rollback` = `gvr` + `gvl`,
  `give_netsim` = `gvn`): `gv` host to guest and back, a refused badge, both switches (giver off, receiver off), storage
  overflow, a give of more than held, and conservation of both inventories; `gvg` guest to guest through the relay;
  `gvr` the receiver ignores COMMIT and ABORT (timeout roll-back); `gvl` the giver drops its link right after COMMIT
  (whatever happens, both inventories sum to the same); `gvn` = `gv` under `BB_NET_SIM=dsl`.
* Test aids: `BB_PARTY_GIVE_TEST`, `BB_PARTY_GIVE_FAULT`, `BB_PARTY_GIVE_WATCH` (DEBUG_SWITCHES.md).
