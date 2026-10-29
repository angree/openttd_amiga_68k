/* MorphOS PPC only. The 68k assembly c2p routines do not exist here and are
 * never reached: MorphOS has no Amiga chipset, so amigagfx_has_aga() is 0, no
 * planar mode is offered, and every screen is RTG or a Workbench window. They
 * are still referenced by amiga_gfx.c, so the linker needs a definition. */
void c2p_rect_asm(void *a) { (void)a; }
void c2p6_bm_asm(void *a) { (void)a; }

/* The stack comes from __stack in os/unix/unix.cpp (OpenTTD's own MorphOS code). */
