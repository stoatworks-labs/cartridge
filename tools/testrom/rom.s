@ Entry. The cartridge header's branch lands here, so this must be first in
@ .text -- and gbamain is assembled immediately after it, so this falls
@ straight through rather than branching. That is not a micro-optimisation:
@ a branch to a global symbol leaves a relocation behind, and there is no
@ linker in this build to resolve one.
    .arm
    .section .text
    .global _start
_start:
    mov sp, #0x03000000
    add sp, sp, #0x7F00        @ 0x03007F00, top of IWRAM
