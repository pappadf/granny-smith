// Framebuffer-object stubs for unit tests.
//
// display_class.c builds `machine.video{,.framebuffer}` out of the object
// model, which unit suites do not link (object.c pulls in the whole class
// registry).  The chips that attach one -- civic.c, dafb.c, ariel.c,
// control.c -- do so as a side effect of their init, so a suite that links
// one of them needs these two symbols and nothing behind them.  Doing
// nothing is correct here: the suites exercise the chips through their C
// structs, not through the object tree, exactly as stub_system.c's
// machine_object() already assumes.

#include <stddef.h>

struct object;
struct display_fb_node;

struct object *display_attach_video_node(struct display_fb_node *node, const char *label) {
    (void)node;
    (void)label;
    return NULL;
}

void display_detach_video_node(struct object *video) {
    (void)video;
}
