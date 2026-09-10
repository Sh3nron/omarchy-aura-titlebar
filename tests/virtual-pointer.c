#include <wayland-client.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "pointer.h"
static struct zwlr_virtual_pointer_manager_v1* manager;
static void global(void* data, struct wl_registry* registry, uint32_t id, const char* interface, uint32_t version) {
    if (!strcmp(interface,"zwlr_virtual_pointer_manager_v1"))
        manager=wl_registry_bind(registry,id,&zwlr_virtual_pointer_manager_v1_interface,1);
}
static void removed(void* data, struct wl_registry* registry, uint32_t id) {}
int main() {
    struct wl_display* display=wl_display_connect(NULL);
    if (!display) return 1;
    struct wl_registry* registry=wl_display_get_registry(display);
    const struct wl_registry_listener listener={global,removed};
    wl_registry_add_listener(registry,&listener,NULL);wl_display_roundtrip(display);
    if (!manager) return 2;
    struct zwlr_virtual_pointer_v1* pointer=zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager,NULL);
    char line[128];
    while(fgets(line,sizeof(line),stdin)) {
        struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
        uint32_t ms=t.tv_sec*1000+t.tv_nsec/1000000;
        unsigned x,y,b;
        if(sscanf(line,"move %u %u",&x,&y)==2)
            zwlr_virtual_pointer_v1_motion_absolute(pointer,ms,x,y,1280,720);
        else if(sscanf(line,"button %u",&b)==1)
            zwlr_virtual_pointer_v1_button(pointer,ms,272,b);
        zwlr_virtual_pointer_v1_frame(pointer);
        if(wl_display_roundtrip(display)<0) return 3;
        puts("ok");fflush(stdout);
    }
    zwlr_virtual_pointer_v1_destroy(pointer);wl_display_flush(display);wl_display_disconnect(display);
}
