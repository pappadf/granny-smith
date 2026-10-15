-- Converted from test.script by scripts/script-to-lua.py
-- Integration test: Apple Lisa 2 — AppleTalk (LocalTalk) on Serial B.
--
-- Serial B is the Lisa's AppleBus port, and the machine plugs the SCC into the
-- emulated AppleTalk network (lisa.c).  The SCC model passes channel B's SDLC
-- frames whole: a frame is the bytes written to the data register up to the
-- WR0 "reset Tx underrun/EOM" command, and a received frame is read back
-- followed by two CRC bytes, with RR1 reporting the end of frame.
--
-- With no disk the rev-H boot ROM settles into its boot-menu COPS input wait
-- ($FE2DC6), a stable state.  The test loads the routine below at $500 and
-- jumps to it.  With interrupts masked it programs channel B for SDLC (node
-- $42, address search, hunt), sends a lapENQ to node 33 -- the emulated
-- network's own node, which answers an ENQ for its address with a lapACK --
-- polls RR0 for the reply and stores the bytes it reads at $580 and RR1 at
-- $5A0, then loops at $558.  The SCC's channel B is at $FCD241 (control) and
-- $FCD245 (data) in the boot ROM's I/O space.
--
--   500  ori.w  #$0700,sr              ; poll, no interrupts
--   504  lea    $FCD241,a0             ; channel B control
--   50A  lea    $FCD245,a1             ; channel B data
--   510  lea    table(pc),a2
--   514  move.b (a2)+,d0               ; register, $FF ends the table
--        cmp.b  #$FF,d0 / beq.s send
--        move.b d0,(a0) / move.b (a2)+,(a0) / bra.s $514
--   522  move.b #$21,(a1)              ; lapENQ: dst = node 33
--        move.b #$42,(a1)              ;         src = node $42
--        move.b #$81,(a1)              ;         type = ENQ
--   52E  move.b #$C0,(a0)              ; WR0: reset Tx underrun/EOM (end of frame)
--   532  lea    $580,a3
--        move.l #100000,d1             ; poll budget
--   53C  btst   #0,(a0) / bne.s read   ; RR0: Rx character available
--        subq.l #1,d1 / bne.s $53C / bra.s done
--   548  move.b (a1),(a3)+             ; read the frame and its CRC trailer
--        btst   #0,(a0) / bne.s $548
--   550  move.b #1,(a0) / move.b (a0),$5A0   ; RR1
--   558  bra.s  $558                   ; done
--   55A  table: WR9 $40 (reset channel B), WR4 $20 (x1, SDLC),
--        WR10 $E0 (FM0, CRC preset), WR6 $42 (node), WR7 $7E (flag),
--        WR3 $DD (Rx 8 bits, hunt, CRC, address search, enable),
--        WR5 $6B (Tx 8 bits, enable, RTS, CRC), $FF

debug.breakpoints.add(0x00FE2DC6)
scheduler.run(40000000)
assert(machine.id == "lisa", "machine.id != lisa")
assert(machine.cpu.pc == 0x00FE2DC6, "did not reach the boot-menu input wait")
debug.breakpoints.clear()

-- The cable is plugged in and nothing has crossed it yet.
assert(appletalk.stats.llap_rx == 0, "frames on the wire before the routine ran")
assert(appletalk.stats.llap_tx == 0, "frames sent to the Lisa before it asked")

machine.memory.poke.l(0x0500, 0x007c0700)
machine.memory.poke.l(0x0504, 0x41f900fc)
machine.memory.poke.l(0x0508, 0xd24143f9)
machine.memory.poke.l(0x050C, 0x00fcd245)
machine.memory.poke.l(0x0510, 0x45fa0048)
machine.memory.poke.l(0x0514, 0x101a0c00)
machine.memory.poke.l(0x0518, 0x00ff6706)
machine.memory.poke.l(0x051C, 0x1080109a)
machine.memory.poke.l(0x0520, 0x60f212bc)
machine.memory.poke.l(0x0524, 0x002112bc)
machine.memory.poke.l(0x0528, 0x004212bc)
machine.memory.poke.l(0x052C, 0x008110bc)
machine.memory.poke.l(0x0530, 0x00c047f8)
machine.memory.poke.l(0x0534, 0x0580223c)
machine.memory.poke.l(0x0538, 0x000186a0)
machine.memory.poke.l(0x053C, 0x08100000)
machine.memory.poke.l(0x0540, 0x66065381)
machine.memory.poke.l(0x0544, 0x66f66010)
machine.memory.poke.l(0x0548, 0x16d10810)
machine.memory.poke.l(0x054C, 0x000066f8)
machine.memory.poke.l(0x0550, 0x10bc0001)
machine.memory.poke.l(0x0554, 0x11d005a0)
machine.memory.poke.l(0x0558, 0x60fe0940)
machine.memory.poke.l(0x055C, 0x04200ae0)
machine.memory.poke.l(0x0560, 0x0642077e)
machine.memory.poke.l(0x0564, 0x03dd056b)
machine.memory.poke.l(0x0568, 0xff000000)
debug.breakpoints.add(0x00000558)
machine.cpu.pc = 0x00000500
scheduler.run(4000000)
assert(machine.cpu.pc == 0x00000558, "the routine did not finish")

-- The network received the lapENQ from the Lisa and answered it.
assert(appletalk.stats.llap_rx == 1, "the network did not receive the Lisa's lapENQ")
assert(appletalk.stats.llap_tx == 1, "the network did not answer with a lapACK")

-- The Lisa received the lapACK: dst = $42 (us), src = 33, type = ACK ($82),
-- then the two CRC bytes, with RR1 reporting the end of the frame.
assert(machine.memory.peek.b(0x580) == 0x42, "ACK dst != our node $42")
assert(machine.memory.peek.b(0x581) == 0x21, "ACK src != node 33")
assert(machine.memory.peek.b(0x582) == 0x82, "frame type != lapACK")
assert((machine.memory.peek.b(0x5A0) & 0x80) ~= 0, "RR1 does not report end of frame")

quit()
