# Copies a DOL image to its load addresses and jumps to its entry point.
# Runs from MEM2 after the system is shut down; position independent and
# without any calls, so it survives the program it replaces being overwritten.
#   r3 = DOL file in memory (header at offset 0)
    .text
    .globl dolboot
dolboot:
    mr      31, 3             # r31 = DOL base
    li      30, 0              # r30 = section index * 4
section:
    lwzx    5, 31, 30        # file offset
    addi    6, 30, 0x48
    lwzx    6, 31, 6         # load address
    addi    7, 30, 0x90
    lwzx    7, 31, 7         # size
    cmpwi   7, 0
    beq     next
    add     5, 31, 5         # source
    mr      8, 6              # destination
    addi    9, 7, 3
    srwi    9, 9, 2           # words
    mtctr   9
copy:
    lwz     10, 0(5)
    stw     10, 0(8)
    addi    5, 5, 4
    addi    8, 8, 4
    bdnz    copy
    mr      4, 6
    mr      5, 7
    bl      flush
next:
    addi    30, 30, 4
    cmpwi   30, 18 * 4
    blt     section
    # clear BSS
    lwz     6, 0xD8(31)
    lwz     7, 0xDC(31)
    cmpwi   7, 0
    beq     go
    mr      8, 6
    addi    9, 7, 3
    srwi    9, 9, 2
    mtctr   9
    li      10, 0
zero:
    stw     10, 0(8)
    addi    8, 8, 4
    bdnz    zero
    mr      4, 6
    mr      5, 7
    bl      flush
go:
    sync
    isync
    lwz     3, 0xE0(31)
    mtctr   3
    bctr

# write the data cache back and invalidate the instruction cache
#   r4 = start, r5 = length (local leaf routine, uses r11/r12 only)
flush:
    rlwinm  11, 4, 0, 0, 26   # align down to 32
    add     12, 4, 5
fl_loop:
    dcbst   0, 11
    sync
    icbi    0, 11
    addi    11, 11, 32
    cmplw   11, 12
    blt     fl_loop
    sync
    isync
    blr
