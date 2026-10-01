; programs.asm — the user programs, embedded into the kernel image.
; There's no file system to load them from yet (step 7), so the build turns
; each user/*.asm into an ELF executable and `incbin` copies the whole file
; in here, byte for byte. The kernel's ELF loader reads it from memory.

section .rodata

; PROGRAM hello  ->  user_hello: <bytes of build/user/hello.elf> user_hello_end:
%macro PROGRAM 1
  %defstr  PROG_NAME %1                 ; hello -> "hello"
  %strcat  PROG_FILE "build/user/", PROG_NAME, ".elf"
global user_%1, user_%1_end
user_%1:     incbin PROG_FILE
user_%1_end:
%endmacro

PROGRAM hello
PROGRAM peek
PROGRAM args
PROGRAM fpu
