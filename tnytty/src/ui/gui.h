/* gui.h — the `tnytty gui` subcommand (docs/cli.md, docs/adr/0005). */
#ifndef TNYTTY_UI_GUI_H
#define TNYTTY_UI_GUI_H

/* argv/argc are the arguments after `gui`. Returns a process exit code;
 * a platform with no window support exits 1 with a clean message. */
int tt_gui_main(int argc, char **argv);

#endif
