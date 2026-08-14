; Minimal CP/M 2.2 command processor
; Assemble with Digital Research ASM.COM:
;   ASM MINICCP
;   LOAD MINICCP
;
; This is intentionally 8080-compatible source so it can be built by
; the supplied CP/M assembler. It uses the emulator's native BDOS calls.

        ORG     0100H

BDOS    EQU     0005H
CONIN   EQU     1
CONOUT  EQU     2
PRINT   EQU     9
READBUF EQU     10
SELECT  EQU     14
SEARCH  EQU     17
OPEN    EQU     15
CLOSE   EQU     16
READSEQ EQU    20
WRITESEQ EQU   21
MAKE    EQU     22
DMASET  EQU     26
DELETE  EQU     19
EXECUTE EQU    60
RENAME  EQU    23
SETUSER EQU     32
STD_FCB EQU     005CH

START:  LXI     SP,STACK+64
        MVI     C,SELECT
        MVI     E,0
        CALL    BDOS

PROMPT: LXI     D,PROMPT_TEXT
        MVI     C,PRINT
        CALL    BDOS

        LXI     D,COMMAND_BUFFER
        MVI     C,READBUF
        CALL    BDOS

        LDA     COMMAND_BUFFER+1
        CPI     0
        JZ      PROMPT

        LXI     H,COMMAND_BUFFER+2
        CALL    SKIP_SPACES
        CALL    MATCH_DIR
        JZ      DO_DIR

        LXI     H,COMMAND_BUFFER+2
        CALL    MATCH_TYPE
        JZ      DO_TYPE

        LXI     H,COMMAND_BUFFER+2
        CALL    MATCH_ERA
        JZ      DO_ERA

        LXI     H,COMMAND_BUFFER+2
        CALL    MATCH_RUN
        JZ      DO_RUN

        LXI     H,COMMAND_BUFFER+2
        CALL    MATCH_REN
        JZ      DO_REN

        LXI     H,COMMAND_BUFFER+2
        CALL    MATCH_SAVE
        JZ      DO_SAVE

        LXI     H,COMMAND_BUFFER+2
        CALL    MATCH_USER
        JZ      DO_USER

        JMP     DO_EXEC

DO_DIR: LXI     H,FCB
        MVI     M,0
        INX     H
        LXI     D,ALL_NAME
        MVI     B,11
COPY_FCB:
        LDAX    D
        MOV     M,A
        INX     D
        INX     H
        DCR     B
        JNZ     COPY_FCB

        LXI     D,DMA
        MVI     C,26
        CALL    BDOS

        LXI     D,FCB
        MVI     C,SEARCH
        CALL    BDOS
        CPI     0FFH
        JZ      NO_FILES

DIR_LOOP:
        LXI     H,DMA+1
        CALL    PRINT_ENTRY_NAME
        LXI     D,CRLF
        MVI     C,PRINT
        CALL    BDOS

        LXI     D,FCB
        MVI     C,18
        CALL    BDOS
        CPI     0FFH
        JNZ     DIR_LOOP
        JMP     PROMPT

DO_TYPE:
        LXI     H,FCB
        MVI     M,0
        INX     H
        LXI     D,COMMAND_BUFFER+7
        MVI     B,11
COPY_TYPE:
        LDAX    D
        MOV     M,A
        INX     D
        INX     H
        DCR     B
        JNZ     COPY_TYPE

        LXI     D,DMA
        MVI     C,DMASET
        CALL    BDOS
        LXI     D,FCB
        MVI     C,OPEN
        CALL    BDOS
        CPI     0FFH
        JZ      NO_FILES

TYPE_RECORD:
        LXI     D,FCB
        MVI     C,READSEQ
        CALL    BDOS
        CPI     0
        JNZ     PROMPT
        LXI     H,DMA
        MVI     D,128
TYPE_BYTE:
        MOV     A,M
        PUSH    D
        MOV     E,A
        MVI     C,CONOUT
        CALL    BDOS
        POP     D
        INX     H
        DCR     D
        JNZ     TYPE_BYTE
        JMP     TYPE_RECORD

DO_ERA:
        LXI     H,FCB
        MVI     M,0
        INX     H
        LXI     D,COMMAND_BUFFER+6
        MVI     B,11
COPY_ERA:
        LDAX    D
        MOV     M,A
        INX     D
        INX     H
        DCR     B
        JNZ     COPY_ERA

        LXI     D,FCB
        MVI     C,DELETE
        CALL    BDOS
        CPI     0FFH
        JZ      NO_FILES
        LXI     D,ERASED_TEXT
        MVI     C,PRINT
        CALL    BDOS
        JMP     PROMPT

DO_RUN:
        LXI     H,COMMAND_BUFFER+6
        CALL    BUILD_EXEC_FCB
        LXI     H,COMMAND_BUFFER+6
        CALL    PREPARE_COM_FCB
        LXI     D,FCB
        MVI     C,EXECUTE
        CALL    BDOS
        CPI     0FFH
        JZ      NO_FILES
        JMP     PROMPT

; Implicit program launch: the whole command line is "progname [args]",
; with no RUN keyword required.
DO_EXEC:
        LXI     H,COMMAND_BUFFER+2
        CALL    BUILD_EXEC_FCB
        LXI     H,COMMAND_BUFFER+2
        CALL    PREPARE_COM_FCB
        LXI     D,FCB
        MVI     C,EXECUTE
        CALL    BDOS
        CPI     0FFH
        JZ      NO_FILES
        JMP     PROMPT

; HL -> program name text. Builds the EXECUTE FCB properly (name/extension
; parsed via BUILD_FCB_NAME, replacing the previous raw fixed-width copy
; that could embed a literal '.' into the FCB and misidentify files).
BUILD_EXEC_FCB:
        PUSH    H
        LXI     H,FCB
        CALL    CLEAR_FCB
        POP     H
        LXI     D,FCB+1
        CALL    BUILD_FCB_NAME
        RET

; HL -> decimal digit(s) (1 or 2), terminated by any non-digit which is
; left unconsumed. Returns the value (0-99) in A.
PARSE_DECIMAL:
        MOV     A,M
        SUI     '0'
        MOV     B,A
        INX     H
        MOV     A,M
        CPI     '0'
        JC      PARSE_DECIMAL_ONE
        CPI     3AH
        JNC     PARSE_DECIMAL_ONE
        SUI     '0'
        MOV     C,A
        INX     H
        MOV     A,B
        CPI     0
        JZ      PARSE_DECIMAL_NOTENS
        MVI     A,0
PARSE_DECIMAL_TENSLOOP:
        ADI     10
        DCR     B
        JNZ     PARSE_DECIMAL_TENSLOOP
        JMP     PARSE_DECIMAL_ADDONES
PARSE_DECIMAL_NOTENS:
        MVI     A,0
PARSE_DECIMAL_ADDONES:
        ADD     C
        RET
PARSE_DECIMAL_ONE:
        MOV     A,B
        RET

DO_SAVE:
        LXI     H,COMMAND_BUFFER+7
        CALL    SKIP_SPACES
        CALL    PARSE_DECIMAL
        ADD     A
        STA     SAVE_RECORDS
        CALL    SKIP_SPACES
        PUSH    H
        LXI     H,FCB
        CALL    CLEAR_FCB
        POP     H
        LXI     D,FCB+1
        CALL    BUILD_FCB_NAME
        LXI     D,FCB
        MVI     C,MAKE
        CALL    BDOS
        CPI     0FFH
        JZ      NO_FILES

        LXI     H,0100H
        SHLD    SAVE_DMA

SAVE_LOOP:
        LDA     SAVE_RECORDS
        CPI     0
        JZ      SAVE_DONE

        LHLD    SAVE_DMA
        XCHG
        MVI     C,DMASET
        CALL    BDOS

        LXI     D,FCB
        MVI     C,WRITESEQ
        CALL    BDOS

        LHLD    SAVE_DMA
        LXI     D,128
        DAD     D
        SHLD    SAVE_DMA

        LDA     SAVE_RECORDS
        DCR     A
        STA     SAVE_RECORDS
        JMP     SAVE_LOOP

SAVE_DONE:
        LXI     D,FCB
        MVI     C,CLOSE
        CALL    BDOS
        LXI     D,SAVED_TEXT
        MVI     C,PRINT
        CALL    BDOS
        JMP     PROMPT

DO_USER:
        LXI     H,COMMAND_BUFFER+7
        CALL    SKIP_SPACES
        CALL    PARSE_DECIMAL
        MOV     E,A
        MVI     C,SETUSER
        CALL    BDOS
        JMP     PROMPT

DO_REN:
        LXI     H,FCB
        CALL    CLEAR_FCB
        LXI     H,COMMAND_BUFFER+6
        LXI     D,FCB+1
        CALL    BUILD_FCB_NAME
        CALL    SKIP_REN_SEPARATOR
        LXI     D,FCB+17
        CALL    BUILD_FCB_NAME
        LXI     D,FCB
        MVI     C,RENAME
        CALL    BDOS
        CPI     0FFH
        JZ      NO_FILES
        LXI     D,RENAMED_TEXT
        MVI     C,PRINT
        CALL    BDOS
        JMP     PROMPT

; Build the standard CP/M command FCB at 005Ch from the argument after
; the executable name in RUN command lines. HL must already point at the
; start of the program name text (caller's responsibility).
PREPARE_COM_FCB:
FIND_RUN_ARG:
        MOV     A,M
        CPI     0
        JZ      USE_RUN_NAME
        CPI     ' '
        JZ      RUN_ARG_SPACES
        INX     H
        JMP     FIND_RUN_ARG
RUN_ARG_SPACES:
        CALL    SKIP_SPACES
        MOV     A,M
        CPI     0
        JZ      USE_RUN_NAME
        LXI     D,STD_FCB
        INX     D
        MVI     B,8
COPY_ARG_NAME:
        MOV     A,M
        CPI     '.'
        JZ      PAD_ARG_NAME
        CPI     0
        JZ      PAD_ARG_NAME
        STAX    D
        INX     D
        INX     H
        DCR     B
        JNZ     COPY_ARG_NAME
PAD_ARG_NAME:
        MVI     A,' '
        STAX    D
        INX     D
        DCR     B
        JNZ     PAD_ARG_NAME
        MOV     A,M
        CPI     '.'
        JNZ     PAD_ARG_EXT
        INX     H
        MVI     B,3
COPY_ARG_EXT:
        MOV     A,M
        CPI     0
        JZ      PAD_ARG_EXT
        STAX    D
        INX     D
        INX     H
        DCR     B
        JNZ     COPY_ARG_EXT
        JMP     ARG_FCB_DONE
PAD_ARG_EXT:
        MVI     A,' '
        STAX    D
        INX     D
        DCR     B
        JNZ     PAD_ARG_EXT
ARG_FCB_DONE:
        RET
USE_RUN_NAME:
        LXI     H,STD_FCB
        CALL    CLEAR_FCB
        RET

NO_FILES:
        LXI     D,NO_FILES_TEXT
        MVI     C,PRINT
        CALL    BDOS
        JMP     PROMPT

; HL points to the command text. Set Z if it begins with DIR and is
; followed by a space or NUL.
MATCH_DIR:
        MOV     A,M
        CPI     'D'
        RNZ
        INX     H
        MOV     A,M
        CPI     'I'
        RNZ
        INX     H
        MOV     A,M
        CPI     'R'
        RNZ
        INX     H
        MOV     A,M
        CPI     0
        RZ
        CPI     ' '
        RET

MATCH_TYPE:
        MOV     A,M
        CPI     'T'
        RNZ
        INX     H
        MOV     A,M
        CPI     'Y'
        RNZ
        INX     H
        MOV     A,M
        CPI     'P'
        RNZ
        INX     H
        MOV     A,M
        CPI     'E'
        RNZ
        INX     H
        MOV     A,M
        CPI     ' '
        RET

MATCH_ERA:
        MOV     A,M
        CPI     'E'
        RNZ
        INX     H
        MOV     A,M
        CPI     'R'
        RNZ
        INX     H
        MOV     A,M
        CPI     'A'
        RNZ
        INX     H
        MOV     A,M
        CPI     ' '
        RET

MATCH_RUN:
        MOV     A,M
        CPI     'R'
        RNZ
        INX     H
        MOV     A,M
        CPI     'U'
        RNZ
        INX     H
        MOV     A,M
        CPI     'N'
        RNZ
        INX     H
        MOV     A,M
        CPI     ' '
        RET

MATCH_REN:
        MOV     A,M
        CPI     'R'
        RNZ
        INX     H
        MOV     A,M
        CPI     'E'
        RNZ
        INX     H
        MOV     A,M
        CPI     'N'
        RNZ
        INX     H
        MOV     A,M
        CPI     ' '
        RET

MATCH_SAVE:
        MOV     A,M
        CPI     'S'
        RNZ
        INX     H
        MOV     A,M
        CPI     'A'
        RNZ
        INX     H
        MOV     A,M
        CPI     'V'
        RNZ
        INX     H
        MOV     A,M
        CPI     'E'
        RNZ
        INX     H
        MOV     A,M
        CPI     ' '
        RET

MATCH_USER:
        MOV     A,M
        CPI     'U'
        RNZ
        INX     H
        MOV     A,M
        CPI     'S'
        RNZ
        INX     H
        MOV     A,M
        CPI     'E'
        RNZ
        INX     H
        MOV     A,M
        CPI     'R'
        RNZ
        INX     H
        MOV     A,M
        CPI     ' '
        RET

SKIP_REN_SEPARATOR:
        MOV     A,M
        CPI     0
        RZ
        CPI     ' '
        JZ      SKIP_REN_SEPARATOR_NEXT
        CPI     '='
        RNZ
SKIP_REN_SEPARATOR_NEXT:
        INX     H
        JMP     SKIP_REN_SEPARATOR

CLEAR_FCB:
        MVI     B,36
CLEAR_FCB_LOOP:
        MVI     M,0
        INX     H
        DCR     B
        JNZ     CLEAR_FCB_LOOP
        RET

; Convert dotted 8.3 text at HL into the name/extension field at DE.
BUILD_FCB_NAME:
        MVI     B,8
BUILD_FCB_NAME_LOOP:
        MOV     A,M
        CPI     '.'
        JZ      BUILD_FCB_NAME_PAD
        CPI     ' '
        JZ      BUILD_FCB_NAME_PAD
        CPI     '='
        JZ      BUILD_FCB_NAME_PAD
        CPI     0
        JZ      BUILD_FCB_NAME_PAD
        STAX    D
        INX     D
        INX     H
        DCR     B
        JNZ     BUILD_FCB_NAME_LOOP
BUILD_FCB_NAME_PAD:
        MOV     A,B
        CPI     0
        JZ      BUILD_FCB_NAME_PAD_DONE
BUILD_FCB_NAME_PAD_LOOP:
        MVI     A,' '
        STAX    D
        INX     D
        DCR     B
        JNZ     BUILD_FCB_NAME_PAD_LOOP
BUILD_FCB_NAME_PAD_DONE:
        MOV     A,M
        CPI     '.'
        JNZ     BUILD_FCB_EXT_PAD
        INX     H
        MVI     B,3
BUILD_FCB_EXT_LOOP:
        MOV     A,M
        CPI     ' '
        JZ      BUILD_FCB_EXT_PAD
        CPI     '='
        JZ      BUILD_FCB_EXT_PAD
        CPI     0
        JZ      BUILD_FCB_EXT_PAD
        STAX    D
        INX     D
        INX     H
        DCR     B
        JNZ     BUILD_FCB_EXT_LOOP
        JMP     BUILD_FCB_DONE
BUILD_FCB_EXT_PAD:
        MOV     A,B
        CPI     0
        JZ      BUILD_FCB_DONE
BUILD_FCB_EXT_PAD_LOOP:
        MVI     A,' '
        STAX    D
        INX     D
        DCR     B
        JNZ     BUILD_FCB_EXT_PAD_LOOP
BUILD_FCB_DONE:
        RET

SKIP_SPACES:
        MOV     A,M
        CPI     ' '
        RNZ
        INX     H
        JMP     SKIP_SPACES

; Print 8-character name, '.', and 3-character extension from a
; standard CP/M directory entry. HL points at entry byte 1.
PRINT_ENTRY_NAME:
        MVI     D,8
PRINT_NAME:
        MOV     A,M
        CPI     ' '
        JZ      PRINT_NAME_SKIP
        MOV     E,A
        MVI     C,CONOUT
        CALL    BDOS
PRINT_NAME_SKIP:
        INX     H
        DCR     D
        JNZ     PRINT_NAME

        MVI     E,'.'
        MVI     C,CONOUT
        CALL    BDOS

        MVI     D,3
PRINT_EXT:
        MOV     A,M
        CPI     ' '
        JZ      PRINT_EXT_SKIP
        MOV     E,A
        MVI     C,CONOUT
        CALL    BDOS
PRINT_EXT_SKIP:
        INX     H
        DCR     D
        JNZ     PRINT_EXT
        RET

; Console output expands a lone CR to CRLF, so these strings use CR only.
PROMPT_TEXT:
        DB      13,'A> $'
UNKNOWN_TEXT:
        DB      13,'? $'
NO_FILES_TEXT:
        DB      13,'No files$'
ERASED_TEXT:
        DB      13,'Erased$'
RENAMED_TEXT:
        DB      13,'Renamed$'
SAVED_TEXT:
        DB      13,'Saved$'
CRLF:
        DB      13,'$'
ALL_NAME:
        DB      '???????????'
COMMAND_BUFFER:
        DB      31,0
        DS      31
FCB:
        DS      36
SAVE_RECORDS:
        DS      1
SAVE_DMA:
        DS      2
DMA:
        DS      128
STACK:
        DS      64

        END
