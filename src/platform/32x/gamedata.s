! Game data blob produced by tools/convert.py (big-endian), linked into ROM.
        .section .rodata.gamedata,"a"
        .align  4
        .global _dk_gamedata
_dk_gamedata:
        .incbin "gen/gamedata_be.bin"
        .align  4
