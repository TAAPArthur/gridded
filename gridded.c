#include <X11/X.h>
#include <X11/keysym.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>

#define LEN(A) (sizeof(A)/sizeof(A[0]))

const char SHELL[] =  "/bin/sh";


#ifndef ENV_PREFIX
#define ENV_PREFIX "GRIDDED_"
#endif

#define MAX_WINDOWS 255
pid_t pids[MAX_WINDOWS]={0};
xcb_window_t windows[MAX_WINDOWS]={0};
int num_windows = 0;

#define MOD_MASK ControlMask
#define IGNORE_MASK Mod2Mask
typedef struct {
    uint8_t  mod;
    uint32_t keysym;
    void (*func)();
    int arg;
    xcb_keycode_t keycode;
} Binding;

xcb_window_t parent;
xcb_window_t embed;
int rows = 0, cols = 0;

int parent_width, parent_height;

xcb_window_t createWindow(xcb_connection_t* dis) {
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator (xcb_get_setup (dis));
    xcb_screen_t* screen = iter.data;

    xcb_window_t win = xcb_generate_id(dis);
    uint32_t values [] = {0, XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY};
    xcb_create_window(dis, XCB_COPY_FROM_PARENT, win, embed? embed: screen->root, 0, 0, 10, 10, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
 screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, &values);
    return win;
}

void setWindowHints(xcb_connection_t* dis, xcb_window_t win) {
    xcb_icccm_wm_hints_t hints;
    xcb_icccm_wm_hints_set_input(&hints, 1);
    xcb_icccm_wm_hints_set_normal(&hints);
    xcb_icccm_set_wm_hints_checked(dis, win, &hints);
}

void die(const char* err) {
    perror(err);
    exit(1);
}

static int spawn(const char* command) {
    int pid = fork();
    if(pid == 0) {
        static char strValue[32];
        sprintf(strValue, "%u", parent);
        setenv(ENV_PREFIX "PARENT_WIN", strValue, 1);
        const char* const args[] = {SHELL, "-c", command, NULL};
        execv(args[0], (char* const*)args);
        die("execv failed");
    }
    else if(pid < 0)
        die("error forking\n");
    return pid;
}

bool initial_mirror;
bool full;
#define GET_ARG atoi(argv[0][2] ? *argv +2 :*++argv);
char ** parse_args (char **argv) {
    for(; argv[0]; ++argv) {
        if (argv[0][0] == '-') {
            switch (argv[0][1]) {
                case 'c':
                    cols = GET_ARG;
                    break;
                case 'f':
                    full = 1;
                    break;
                case 'm':
                    initial_mirror = 1;
                    break;
                case 'p':
                    parent = GET_ARG;
                    break;
                case 'r':
                    rows = GET_ARG;
                    break;
                case 'w':
                    embed = GET_ARG;
                    break;
                case '-':
                    return ++argv;
            }
        }
        else
            break;
    }
    return argv;
}

void resize(xcb_connection_t* dis) {
    xcb_clear_area(dis, 1, parent, 0, 0, parent_width,parent_height);
    int width = cols && !full ? parent_width / cols: parent_width;
    int height = rows && !full  ? parent_height / rows: parent_height;
    for(int r = 0, i=0; (r < rows || !rows) && i < num_windows; r++)
        for(int c = 0; (c < cols || !cols) && i < num_windows; c++, i++) {
            if(windows[i])
                xcb_configure_window(dis, windows[i],
                        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                        (int[4]) {c*width, r*height, width, height});
        }
}

void swap_windows(int i, int j) {
    xcb_window_t temp = windows[i];
    windows[i] =  windows[j];
    windows[j] =  temp;

    pid_t temp_pid = pids[i];
    pids[i] =  pids[j];
    pids[j] =  temp_pid ;
}

void mirror() {
    for(int i = 0; i < num_windows; i+=2) {
        swap_windows(i, i + 1);
    }

}

xcb_atom_t get_atom(xcb_connection_t* dis, const char*name) {
    xcb_intern_atom_reply_t* reply;
    reply = xcb_intern_atom_reply(dis, xcb_intern_atom(dis, 0, strlen(name), name), NULL);
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

int get_int_property(xcb_connection_t*dis, xcb_window_t win, xcb_atom_t atom) {
    xcb_get_property_reply_t* reply;
    xcb_get_property_cookie_t cookie = xcb_get_property(dis, 0, win, atom, XCB_ATOM_CARDINAL, 0, -1);
    int result = 0;
    if((reply = xcb_get_property_reply(dis, cookie, NULL)))
        if(xcb_get_property_value_length(reply) == 4) {
            result = reply ? *(int*)xcb_get_property_value(reply) : 0;
        }
    free(reply);
    return result;
}


void copy_string_property(xcb_connection_t*dis, xcb_window_t src, xcb_window_t dest, xcb_atom_t atom, xcb_atom_t type) {
    xcb_get_property_reply_t* reply;
    xcb_get_property_cookie_t cookie = xcb_get_property(dis, 0, src, atom, type, 0, -1);
    if((reply = xcb_get_property_reply(dis, cookie, NULL)))
        if(xcb_get_property_value_length(reply)) {
            xcb_change_property(dis, XCB_PROP_MODE_REPLACE, dest, atom, type, 8, xcb_get_property_value_length(reply), xcb_get_property_value(reply));
        }
    free(reply);
}

bool add_window(xcb_window_t win, pid_t pid) {
    for(int i = 0; i < MAX_WINDOWS && pids[i]; i++) {
        if(pids[i] == pid) {
            windows[i] = win;
            return 1;
        }
    }
    return 0;
}

bool remove_window(xcb_window_t win) {
    for(int i = 0; i < MAX_WINDOWS && windows[i]; i++) {
        if(windows[i] == win) {
            for (int n = i; n < MAX_WINDOWS && windows[i]; n++) {
                windows[n] = windows[n+1];
                pids[n] = pids[n+1];
            }
            return 1;
        }
    }
    return 0;
}
void toggle_full() {
    full = !full;
}

Binding bindings[] = {
    {MOD_MASK, XK_space, mirror},
    {MOD_MASK, XK_f, toggle_full},
};
void init_grab_bindings(xcb_connection_t* dis) {
    xcb_key_symbols_t * symbols = xcb_key_symbols_alloc(dis);
    for (int i = 0; i < LEN(bindings); i++) {
        xcb_keycode_t * codes = xcb_key_symbols_get_keycode(symbols, bindings[i].keysym);
        bindings[i].keycode = codes[0];
        free(codes);
        xcb_grab_key(dis, 1, parent, bindings[i].mod | IGNORE_MASK, bindings[i].keycode, XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_SYNC);
        xcb_grab_key(dis, 1, parent, bindings[i].mod, bindings[i].keycode, XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_SYNC);
    }
    xcb_key_symbols_free  (symbols);
}

void reapChildfunc(int s) {
    int wstatus;
    pid_t pid;
    while(1) {
        pid = waitpid(-1, &wstatus, s?WNOHANG:0);
        if (pid > 0) {
            if(--num_windows == 0) {
                exit(WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : WIFSIGNALED(wstatus) ? WTERMSIG(wstatus): 127);
            }
        } else break;
    }
}

int main(int argc, char **argv) {
    struct sigaction sa = {.sa_handler = reapChildfunc, .sa_flags = SA_RESTART};
    sigaction(SIGCHLD, &sa, NULL);
    xcb_connection_t* dis = xcb_connect(NULL, NULL);
    xcb_atom_t pid_atom = get_atom(dis, "_NET_WM_PID");
    xcb_atom_t wm_name_atom = get_atom(dis, "_NET_WM_NAME");
    xcb_atom_t utf8_string_atom = get_atom(dis, "UTF8_STRING");

    argv = parse_args(argv + 1);

    if(!parent) {
        parent = createWindow(dis);
        setWindowHints(dis, parent);
        pid_t pid = getpid();
        xcb_change_property(dis, XCB_PROP_MODE_REPLACE, parent, pid_atom, XCB_ATOM_CARDINAL, 32, 1, &pid);
        xcb_map_window(dis, parent);
    }
    init_grab_bindings(dis);

    for(int i = 0; i < MAX_WINDOWS && argv[0]; i++) {
        pids[i] = spawn(*argv++);
        num_windows++;
    }

    if(initial_mirror)
        mirror();

    xcb_generic_event_t* event;
    xcb_flush(dis);
    Window win;
    const static uint32_t child_window_masks[] = { XCB_EVENT_MASK_PROPERTY_CHANGE };
    while((event = xcb_wait_for_event(dis))) {
        switch(event->response_type & 127) {
            case XCB_MAP_NOTIFY:
                win = ((xcb_map_notify_event_t *)event)->window;
                if(add_window(win, get_int_property(dis, win, pid_atom))) {
                    xcb_change_window_attributes (dis, win, XCB_CW_EVENT_MASK, child_window_masks);
                    resize(dis);
                    if(win == windows[0]) {
                        copy_string_property(dis, windows[0], parent, wm_name_atom, utf8_string_atom );
                        copy_string_property(dis, windows[0], parent, XCB_ATOM_WM_NAME, XCB_ATOM_STRING);
                    }
                }
                break;
            case XCB_PROPERTY_NOTIFY :
                if( windows[0] == ((xcb_property_notify_event_t *)event)->window) {
                        xcb_atom_t atom = ((xcb_property_notify_event_t *)event)->atom;
                    if( wm_name_atom == atom )
                        copy_string_property(dis, windows[0], parent, atom, utf8_string_atom );
                    else if( XCB_ATOM_WM_NAME == atom)
                        copy_string_property(dis, windows[0], parent, atom, XCB_ATOM_STRING);
                }
            case XCB_DESTROY_NOTIFY:
                win = ((xcb_destroy_notify_event_t*)event)->window;
                if(win == parent)
                    exit(1);
                else
                    if(remove_window(win))
                        resize(dis);
                break;
            case XCB_KEY_PRESS :
                for (int i = 0; i < LEN(bindings); i++) {
                    xcb_keycode_t detail = ((xcb_key_press_event_t*)event)->detail;
                    uint8_t mod = ((xcb_key_press_event_t*)event)->state & ~IGNORE_MASK;
                    if(bindings[i].keycode == detail && bindings[i].mod == mod) {
                        bindings[i].func();
                        resize(dis);
                        xcb_allow_events(dis, XCB_ALLOW_REPLAY_KEYBOARD, XCB_CURRENT_TIME);
                        break;
                    }
                }
                break;
            case XCB_CONFIGURE_NOTIFY: {
                xcb_configure_notify_event_t* configure_event = (xcb_configure_notify_event_t*)event;
                if (configure_event->window == parent) {
                    parent_width = configure_event->width;
                    parent_height = configure_event->height;
                    resize(dis);
                }
                break;
            }
        }
        free(event);
        xcb_flush(dis);
    }
    return -1;
}
