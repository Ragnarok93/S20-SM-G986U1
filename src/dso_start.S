.section .text.startup, "ax", %progbits
.globl _start
.hidden _start
.type _start, %function
_start:
  ldr     x0, [sp]                       /* argc */
  add     x1, sp, #8                     /* argv */
  add     x2, x1, x0, lsl #3
  add     x2, x2, #8                     /* envp */
  bl      main
  bl      exit
.size _start, . - _start
