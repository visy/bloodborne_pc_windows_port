bbport 0.8.1 - two-PC test session (pc1 = host, pc2 = guest)
=============================================================

What this is: an automated co-op session between two PCs. Each kit holds the 0.8.1 port build, the game files and the test
saves, preconfigured. You unzip, run one .bat on each PC, and press a key. Then nothing is asked until the end.

Sizes: pc1.zip {PC1_SIZE}, pc2.zip {PC2_SIZE} (the game files are inside, not compressed; unzipped each kit is about 29.6 GB, so
have 40 GB free; use 7-Zip or Windows' own extractor, both read zip64).

1. Install (both PCs)
---------------------
 - Unzip pc1.zip on PC 1 and pc2.zip on PC 2 to a SHORT path on an NTFS drive with free space for the unzipped kit plus
   about 10 GB (the game builds its boot image and shader cache there), e.g.  D:\twopc\  . Not into a OneDrive/Dropbox
   folder, not into your own Bloodborne folder. The kit never reads or writes your own save folder: it plays only the
   saves in its own bbport\user\ folder.
 - Both PCs on the same network (LAN) for blocks 1 and 2. Wired or good Wi-Fi.
 - Windows Firewall: the first run asks twice per PC (it can open BEHIND the game windows - press Alt+Tab if the
   session seems stuck): allow  python.exe  (the kit's runner, UDP broadcast on port 47931) and  bbport.exe  (the party,
   UDP/TCP 9307) on PRIVATE networks. If your LAN is marked Public, tick Public too.
 - Close other heavy programs. Each game runs in a 1280x720 window; leave the windows alone (no clicks, no keys).

2. Run
------
 - On PC 1: double-click START_PC1.bat, then on PC 2: START_PC2.bat (either order; each waits for the other). Press a key
   in each window when it asks. That is all the input there is.
 - The session is a list of short game runs ("sessions"), each started and stopped by the runner. Console lines show
   which one runs. Do not close the console windows. To stop early press Ctrl+C in the console: the game is closed and the logs so far are zipped.
 - Test saves only: your own character is not used. Expect the Hunter's Dream / Yharnam scenes and a Cleric Beast fight.

3. Blocks and times (together about 1.5 hours with the first boot)
------------------------------------------------------------------
 Block 1 - LAN, normal network, about 45 min (about 25 on a fast PC)
   1a  seamless travel (party warp to a lamp, to the Dream, a Hunter's Mark), Rejoin in place, a rejoin the moment the
       host travels (about 15 min);  1b  load times per screen, read from the same logs;
   1c  C2: the host dies in the Cleric Beast fight and the boss keeps hitting the guest (about 7 min);
   1d  Chalice dungeon across game regions - PC 2 runs the region patch (about 5 min);
   1e  invader: PC 2 dials before the host is in a world, then the host travels with it inside (about 8 min).
 Block 2 - LAN with simulated DSL on the guest (BB_NET_SIM=dsl), about 20 min: 1a travel and 1c again.
 Block 3 - Nightwatch VPN, about 15 min (needs YOU): when the console says "Connect the Nightwatch VPN now",
   connect the VPN on that PC (the launcher's Nightwatch VPN button, or your OpenVPN profile). Both PCs must be connected.
   The runner finds the tunnel address (10.8.x.x), checks if the two share a /24 (only the masked form a.b.x.x is ever
   printed or saved), PC 1 hosts, PC 2 finds it through the VPN lobby, one party warp. If no VPN is connected within
   15 minutes this block is skipped and the rest still counts. Disconnect the VPN afterwards if you like.

 To skip blocks: give flags to START_PC1.bat only, e.g.  START_PC1.bat --skip 1d,3   or   --only 1a,1c .
 (PC 2 takes PC 1's list.)  List: bbport\python\python.exe -I twopc\runner.py --role pc1 --list

4. What to send back
--------------------
 When the console says "done", each PC has a file  twopc_logs_pc1_<time>.zip  /  twopc_logs_pc2_<time>.zip  next to the
 .bat. Send both. They hold the game logs (addresses already masked a.b.x.x, the same way the game masks them), the
 net_doctor report and the session_score table per session. No saves, no game data.
 Verdict on either PC (or anywhere with both zips):
    bbport\python\python.exe -I twopc\verdict.py twopc_logs_pc1_*.zip twopc_logs_pc2_*.zip
 prints PASS / FAIL per row and checks that no unmasked IP is in either zip.

5. If something goes wrong
--------------------------
 - A session that does not finish ends by itself after its time limit and the runner goes on; the logs are kept.
 - "pc2 never answered": same network? firewall prompt for python.exe on Private networks? try again.
 - Delete the kit folder when you are done; it contains the game files, do not share it.
