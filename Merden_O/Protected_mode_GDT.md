[[Protected_mode_GDT]]
#protected_mode
#код 
```assembly nasm
reloadSegments:
   ; Reload CS register containing code selector:
   JMP   0x08:.reload_CS ; 0x08 is a stand-in for your code segment
.reload_CS:
   ; Reload data segment registers:
   MOV   AX, 0x10 ; 0x10 is a stand-in for your data segment
   MOV   DS, AX
   MOV   ES, AX
   MOV   FS, AX
   MOV   GS, AX
   MOV   SS, AX
   RET
```
початок у [[GDT]]
реальний(направний) режим [[Real_Mode_GDT]]
