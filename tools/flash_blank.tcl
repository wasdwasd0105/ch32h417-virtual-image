# OpenOCD script: program a CH32H417 whose flash is blank/corrupt.
# Usage: openocd -f tools/wch-h417.cfg -c "set IMG build/merge.bin" -f tools/flash_blank.tcl
# 1) reset halt, 2) run build/clockup.bin (SystemInit) from SRAM so the core is on
# the PLL clock (the WCH-Link flash loader drops words at the reset clock),
# 3) erase/program/verify. Everything is wrapped in catch and the script ALWAYS
# ends with "reset run; shutdown" so the core is never left halted (which makes
# the WCH-Link unable to reconnect until a power cycle).
init
set ok 0
if {[catch {
    reset halt
    # "reset halt" does not always stop the boot core on this part: insist on a halted state before
    # touching flash (an erase under a core executing from flash crashes the chip mid-session)
    for {set i 0} {$i < 30} {incr i} {
        if {[string match "*halted*" [wch_riscv.cpu.0 curstate]]} { break }
        catch {halt}
        sleep 50
    }
    if {![string match "*halted*" [wch_riscv.cpu.0 curstate]]} { error "core did not halt" }
    echo "core halted"
    if {[info exists HOLD]} { mww $HOLD 0xF1A5F1A5 ; echo "debug hold flag set at $HOLD" }
    load_image build/clockup.bin 0x20140000 bin
    catch {halt}
    catch {reg pc 0x20140000}
    reg pc 0x20140000
    catch {resume}
    catch {wait_halt 3000}
    catch {halt}
    echo "after clockup: pc / RCC CTLR,CFGR0 (expect PLLRDY set, CFGR0 SWS=0x8):"
    reg pc
    mdw 0x40021000 2
    flash erase_sector wch_riscv 0 last
    flash write_image $IMG 0x00000000
    verify_image $IMG 0x00000000
    set ok 1
} err]} { echo "FLASH_BLANK_ERROR: $err" }
if {$ok} { echo "FLASH_BLANK_OK: $IMG programmed and verified" }
if {[info exists HOLD]} {
    # Leaving this set parks the boot core on the next start (black board until a power
    # cycle), so retry and verify rather than firing one best-effort write.
    set cleared 0
    for {set i 0} {$i < 8} {incr i} {
        catch {mww $HOLD 0}
        if {![catch {set dump [capture {mdw $HOLD 1}]}] && [string match "*00000000*" $dump]} { set cleared 1 ; break }
    }
    if {$cleared} { echo "hold flag cleared" } else { echo "HOLD_FLAG_STUCK: power-cycle the board (both cables) or it will boot held" }
}
catch {reset run}
shutdown
