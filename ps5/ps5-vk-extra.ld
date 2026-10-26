/* The Vulkan build's addition to RADV's linker script (ps5/link-vk.sh, decision log D34): the end of the code, which
   the fault handler's backtrace bounds the game's code by (ps5-posix.c is_game_code), as ps5-pie.ld provides it. */
PROVIDE(etext = ADDR(.text) + SIZEOF(.text));
