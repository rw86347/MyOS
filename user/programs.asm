; programs.asm — the user programs, embedded into the kernel image.
; There's no file system to load them from yet, so the build assembles each
; user/*.asm to a flat binary and `incbin` copies its bytes in here.

section .rodata

global user_hello, user_hello_end
user_hello:     incbin "build/user/hello.bin"
user_hello_end:

global user_peek, user_peek_end
user_peek:      incbin "build/user/peek.bin"
user_peek_end:
