; Read-only BDOS compatibility test
; Run with: RUN BDOSTEST.COM BIOS.ASM

        ORG     0100H

BDOS    EQU     0005H
CONOUT  EQU     2
PRINT   EQU     9
OPEN    EQU     15
CLOSE   EQU     16
READ    EQU     20
RANDOM  EQU     33
SIZE    EQU     35
SETRND  EQU     36
DMASET  EQU     26

START:  LXI     SP,STACK+64
        LXI     D,START_TEXT
        MVI     C,PRINT
        CALL    BDOS

        LXI     D,DMA
        MVI     C,DMASET
        CALL    BDOS

        LXI     D,FCB
        MVI     C,OPEN
        CALL    BDOS
        CPI     0FFH
        JZ      OPEN_FAIL

        LXI     D,OPEN_OK_TEXT
        MVI     C,PRINT
        CALL    BDOS

        LXI     D,FCB
        MVI     C,SIZE
        CALL    BDOS
        CPI     0FFH
        JZ      SIZE_FAIL

        LXI     D,SIZE_OK_TEXT
        MVI     C,PRINT
        CALL    BDOS

        LXI     D,FCB
        MVI     C,SETRND
        CALL    BDOS
        CPI     0FFH
        JZ      RANDOM_SETUP_FAIL

        LXI     D,FCB
        MVI     C,RANDOM
        CALL    BDOS
        CPI     0
        JNZ     RANDOM_READ_FAIL

        LXI     D,RANDOM_OK_TEXT
        MVI     C,PRINT
        CALL    BDOS

        LXI     D,FCB
        MVI     C,CLOSE
        CALL    BDOS

        LXI     D,PASS_TEXT
        MVI     C,PRINT
        CALL    BDOS
        JMP     EXIT

OPEN_FAIL:
        LXI     D,OPEN_FAIL_TEXT
        JMP     REPORT_FAIL
SIZE_FAIL:
        LXI     D,SIZE_FAIL_TEXT
        JMP     REPORT_FAIL
RANDOM_SETUP_FAIL:
        LXI     D,RANDOM_SETUP_FAIL_TEXT
        JMP     REPORT_FAIL
RANDOM_READ_FAIL:
        LXI     D,RANDOM_READ_FAIL_TEXT

REPORT_FAIL:
        MVI     C,PRINT
        CALL    BDOS
        LXI     D,FAIL_TEXT
        MVI     C,PRINT
        CALL    BDOS

EXIT:   MVI     C,0
        CALL    BDOS

START_TEXT:
        DB      13,10,'BDOS TEST BIOS.ASM',13,10,'$'
OPEN_OK_TEXT:
        DB      'OPEN OK',13,10,'$'
SIZE_OK_TEXT:
        DB      'SIZE OK',13,10,'$'
RANDOM_OK_TEXT:
        DB      'RANDOM READ OK',13,10,'$'
PASS_TEXT:
        DB      'BDOS TEST PASS',13,10,'$'
OPEN_FAIL_TEXT:
        DB      'OPEN FAIL',13,10,'$'
SIZE_FAIL_TEXT:
        DB      'SIZE FAIL',13,10,'$'
RANDOM_SETUP_FAIL_TEXT:
        DB      'SET RANDOM FAIL',13,10,'$'
RANDOM_READ_FAIL_TEXT:
        DB      'RANDOM READ FAIL',13,10,'$'
FAIL_TEXT:
        DB      'BDOS TEST FAILED',13,10,'$'

FCB:    DS      36
DMA:    DS      128
STACK:  DS      64

        END
