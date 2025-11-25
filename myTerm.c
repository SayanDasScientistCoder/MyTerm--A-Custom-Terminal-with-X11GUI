#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <err.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>

#define POSX 500
#define POSY 500
#define WIDTH 600
#define HEIGHT 400
#define BORDER 16
#define MAX_LINES 1000
#define VISIBLE_LINES 40
#define MAX_LINE_LEN 256
#define MAX_TABS 100

static int win_width = WIDTH;
static int win_height = HEIGHT;

#define MAX_HISTORY_SIZE 10000
#define HISTORY_FILE ".myterm_history"

static char history[MAX_HISTORY_SIZE][MAX_LINE_LEN];
static int history_count = 0;
static int history_current = 0;
static int search_mode = 0;
static char search_term[MAX_LINE_LEN] = "";
static int search_cursor = 0;

static int selection_mode = 0;
static char **selection_matches = NULL;
static int selection_match_count = 0;
static int original_line = 0;
static char original_command[MAX_LINE_LEN] = "";

static Display *dpy;
static int screen;
static Window root;

/* ---- Tab structure ---- */
typedef struct {
    pid_t shell_pid;
    int pipefd[2];
    char lines[MAX_LINES][MAX_LINE_LEN];
    int isCommand[MAX_LINES];
    int current_line;
    int cursor_pos;
    char command[1000];
    int scroll_y; // <--- vertical scroll offset (in lines)
    int scroll_x; // <--- horizontal scroll offset (in characters)
    char selection_input[10];
    int selection_input_pos;
} Tab;

static Tab tabs[MAX_TABS];
static int current_tab = 0;
static int total_tabs = 1;

volatile sig_atomic_t current_child_pid = -1;

pid_t background_jobs[100];
int bg_count = 0;

static int cursor_visible = 1;
static time_t last_cursor_blink = 0;
#define CURSOR_BLINK_INTERVAL 500000

/* ---- Window + GC ---- */
static Window create_window() {
    XSetWindowAttributes xwa;
    xwa.background_pixel = WhitePixel(dpy, screen);
    xwa.border_pixel = BlackPixel(dpy, screen);
    xwa.event_mask = KeyPressMask | ButtonPressMask | ExposureMask | StructureNotifyMask;
    return XCreateWindow(dpy, root, POSX, POSY, WIDTH, HEIGHT, BORDER,
                         DefaultDepth(dpy, screen),
                         InputOutput,
                         DefaultVisual(dpy, screen),
                         CWBackPixel | CWBorderPixel | CWEventMask,
                         &xwa);
}

static GC create_gc(Window win) {
    XGCValues values;
    values.foreground = BlackPixel(dpy, screen);
    values.background = WhitePixel(dpy, screen);
    values.line_width = 2;
    unsigned long mask = GCForeground | GCBackground | GCLineWidth;
    return XCreateGC(dpy, win, mask, &values);
}

static void draw_tabs(Window win, GC gc) {
    // Get font metrics to calculate proper sizes
    XFontStruct *font = XQueryFont(dpy, XGContextFromGC(gc));
    int font_height = 15; // default fallback
    int char_width = 8;   // default fallback
    
    if (font) {
        font_height = font->ascent + font->descent;
        char_width = font->max_bounds.width;
    }
    
    int x = 10, y = 15 + font_height; // Position tabs lower to account for taller font
    int tab_width = char_width * 8;   // Adjust tab width based on font
    int tab_height = font_height + 4; // Add some padding
    
    for (int i = 0; i < total_tabs; i++) {
        char label[20];
        sprintf(label, "[Tab %d]", i + 1);
        
        if (i == current_tab) {
            // Draw rectangle around current tab - adjust size for font
            XDrawRectangle(dpy, win, gc, x - 5, y - font_height - 2, 
                          tab_width, tab_height);
        }
        
        XDrawString(dpy, win, gc, x, y, label, strlen(label));
        x += tab_width + 10; // Add spacing between tabs
    }
}

static void draw_text(Window win, GC gc, Tab *tab) {
    XClearWindow(dpy, win);
    draw_tabs(win, gc);

    int y_start = 40;
    int line_height = 20;
    int visible_lines = (win_height - y_start) / line_height - 1;

    int first_line = tab->scroll_y;
    int last_line = tab->scroll_y + visible_lines;
    if (last_line > tab->current_line) last_line = tab->current_line;

    // Calculate maximum characters that can fit horizontally
    int char_width = 8; // Approximate character width
    int max_chars = (win_width - 20) / char_width; // 20px margin

    for (int i = first_line; i <= last_line; i++) {
        char display_line[MAX_LINE_LEN + 40];
        if (tab->isCommand[i])
            snprintf(display_line, sizeof(display_line), "user@myterm> %s", tab->lines[i]);
        else
            snprintf(display_line, sizeof(display_line), "%s", tab->lines[i]);

        int len = strlen(display_line);
        
        // Handle horizontal scrolling and truncation if needed
        char *display_start = display_line + tab->scroll_x;
        int display_len = len - tab->scroll_x;
        
        // Truncate if too long for window
        if (display_len > max_chars) {
            display_len = max_chars;
        }
        
        if (display_len > 0) {
            XDrawString(dpy, win, gc, 10, y_start + (i - first_line + 1) * line_height,
                        display_start, display_len);
        }
    }

    XFlush(dpy);
}

/* ---- Tab / Shell initialization ---- */
static void init_tab(Tab *tab) {
    memset(tab, 0, sizeof(Tab));
    for (int i = 0; i < MAX_LINES; i++) tab->isCommand[i] = 1;

    pipe(tab->pipefd);
    tab->shell_pid = fork();
    if (tab->shell_pid == 0) {
        dup2(tab->pipefd[1], STDOUT_FILENO);
        dup2(tab->pipefd[1], STDERR_FILENO);
        close(tab->pipefd[0]);
        execlp("sh", "sh", NULL);
        exit(1);
    }
    close(tab->pipefd[1]);
    tab->scroll_y = 0;
    tab->scroll_x = 0;
    
    // Initialize selection state
    tab->selection_input[0] = '\0';
    tab->selection_input_pos = 0;
}
/* ---- Output handling ---- */
static void draw_output(Window win, GC gc, Tab *tab, const char *output) {
    if (!output || !*output) return;

    char *buf = strdup(output);
    if (!buf) return;

    char *saveptr = NULL;
    char *line = strtok_r(buf, "\n", &saveptr);
    int idx = tab->current_line;

    while (line && idx < MAX_LINES - 1) {
        idx++;
        strncpy(tab->lines[idx], line, MAX_LINE_LEN - 1);
        tab->lines[idx][MAX_LINE_LEN - 1] = '\0';
        tab->isCommand[idx] = 0;
        line = strtok_r(NULL, "\n", &saveptr);
    }

    tab->current_line = idx;
    tab->isCommand[tab->current_line + 1] = 1;
    draw_text(win, gc, tab);
    free(buf);
}

void create_new_tab(int *tab_count, Tab tabs[], int *current_tab) {
    if (*tab_count >= MAX_TABS) return; // avoid overflow

    (*tab_count)++;
    *current_tab = *tab_count - 1;

    Tab *new_tab = &tabs[*current_tab];
    memset(new_tab, 0, sizeof(Tab));
    new_tab->current_line = 0;
    new_tab->cursor_pos = 0;
    new_tab->isCommand[0] = 1;
}
// Execute a piped command string like "ls | wc -l | sort"
void execute_piped_command(char *full_command)
{
    char *commands[20];
    int num_cmds = 0;
    char *saveptr;

    // Split by '|'
    char *token = strtok_r(full_command, "|", &saveptr);
    while (token && num_cmds < 20)
    {
        while (*token == ' ') token++; // trim leading spaces
        commands[num_cmds++] = token;
        token = strtok_r(NULL, "|", &saveptr);
    }

    int pipefd[2 * (num_cmds - 1)];

    // Create required pipes
    for (int i = 0; i < num_cmds - 1; i++)
    {
        if (pipe(pipefd + i * 2) < 0)
        {
            perror("pipe");
            exit(1);
        }
    }

    // Fork for each command
    for (int i = 0; i < num_cmds; i++)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            // --- Input redirection for intermediate commands ---
            if (i > 0)
            {
                dup2(pipefd[(i - 1) * 2], STDIN_FILENO);
            }

            // --- Output redirection for intermediate commands ---
            if (i < num_cmds - 1)
            {
                dup2(pipefd[i * 2 + 1], STDOUT_FILENO);
            }

            // Close all pipes in child
            for (int j = 0; j < 2 * (num_cmds - 1); j++)
                close(pipefd[j]);

            // Execute the command
            execlp("sh", "sh", "-c", commands[i], NULL);
            perror("execlp");
            exit(1);
        }
    }

    // Close all pipes in parent
    for (int i = 0; i < 2 * (num_cmds - 1); i++)
        close(pipefd[i]);

    // Wait for all children
    for (int i = 0; i < num_cmds; i++)
        wait(NULL);
}
// Globals for signal handling
static volatile sig_atomic_t stop_multiwatch = 0;

void handle_sigint_multiwatch(int signo) {
    stop_multiwatch = 1;
}


void multiWatch(Tab *tab, Window win, GC gc, const char *input_line) {
    #define MAX_CMDS 16
    const int REFRESH_INTERVAL = 2; // seconds

    char cmds[MAX_CMDS][512];
    int n = 0;

    // Parse commands from input: multiWatch ["cmd1", "cmd2"]
    const char *start = strchr(input_line, '[');
    const char *end = strrchr(input_line, ']');
    if (!start || !end || start >= end) {
        draw_output(win, gc, tab, "Invalid format. Use: multiWatch [\"cmd1\", \"cmd2\"]\n");
        return;
    }

    char buf[1024];
    size_t lenbuf = (size_t)(end - (start + 1));
    if (lenbuf >= sizeof(buf)) lenbuf = sizeof(buf) - 1;
    memcpy(buf, start + 1, lenbuf);
    buf[lenbuf] = '\0';

    // Parse comma-separated quoted commands
    char *tok = strtok(buf, ",");
    while (tok && n < MAX_CMDS) {
        // Trim spaces and quotes
        while (*tok == ' ' || *tok == '\t' || *tok == '\"') tok++;
        char *p = tok + strlen(tok) - 1;
        while (p >= tok && (*p == ' ' || *p == '\t' || *p == '\"')) { 
            *p = '\0'; 
            p--; 
        }
        if (*tok) {
            strncpy(cmds[n], tok, sizeof(cmds[n]) - 1);
            cmds[n][sizeof(cmds[n]) - 1] = '\0';
            n++;
        }
        tok = strtok(NULL, ",");
    }

    if (n == 0) {
        draw_output(win, gc, tab, "No valid commands provided to multiWatch\n");
        return;
    }

    draw_output(win, gc, tab, "Starting multiWatch. Press Ctrl+C to stop...\n");

    int xfd = ConnectionNumber(dpy);
    int running = 1;
    struct sigaction sa_old, sa_new;

    // Setup signal handler for Ctrl+C
    sa_new.sa_handler = handle_sigint_multiwatch;
    sigemptyset(&sa_new.sa_mask);
    sa_new.sa_flags = 0;
    sigaction(SIGINT, &sa_new, &sa_old);

    // Reset the stop flag
    stop_multiwatch = 0;

    // Move pids and pipefds declaration outside the loop so they're accessible in cleanup
    pid_t pids[MAX_CMDS] = {0};
    int pipefds[MAX_CMDS][2] = {{-1, -1}};

    while (running && !stop_multiwatch) {
        int any_child_running = 0;
        
        // Create pipes and fork processes
        for (int i = 0; i < n; i++) {
            if (pipe(pipefds[i]) < 0) {
                perror("pipe");
                // Cleanup previous pipes
                for (int j = 0; j < i; j++) {
                    close(pipefds[j][0]);
                    close(pipefds[j][1]);
                }
                goto cleanup;
            }

            pids[i] = fork();
            if (pids[i] == 0) {
                // Child process - set new process group
                setpgid(0, 0);
                
                close(pipefds[i][0]); // Close read end
                
                // Redirect stdout and stderr to pipe
                dup2(pipefds[i][1], STDOUT_FILENO);
                dup2(pipefds[i][1], STDERR_FILENO);
                close(pipefds[i][1]);
                
                // Execute command
                execlp("sh", "sh", "-c", cmds[i], NULL);
                perror("execlp");
                _exit(127);
            } else if (pids[i] < 0) {
                perror("fork");
                close(pipefds[i][0]);
                close(pipefds[i][1]);
                // Cleanup previous
                for (int j = 0; j < i; j++) {
                    close(pipefds[j][0]);
                    if (pids[j] > 0) kill(pids[j], SIGTERM);
                }
                goto cleanup;
            } else {
                // Parent process
                any_child_running = 1;
                close(pipefds[i][1]); // Close write end
                // Set non-blocking
                int flags = fcntl(pipefds[i][0], F_GETFL, 0);
                fcntl(pipefds[i][0], F_SETFL, flags | O_NONBLOCK);
            }
        }

        if (!any_child_running) {
            goto cleanup;
        }

        // Setup poll structure
        struct pollfd *pfds = malloc((n + 1) * sizeof(struct pollfd));
        if (!pfds) {
            for (int i = 0; i < n; i++) {
                if (pipefds[i][0] >= 0) close(pipefds[i][0]);
                if (pids[i] > 0) kill(pids[i], SIGTERM);
            }
            goto cleanup;
        }

        for (int i = 0; i < n; i++) {
            pfds[i].fd = pipefds[i][0];
            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }
        pfds[n].fd = xfd;
        pfds[n].events = POLLIN;
        pfds[n].revents = 0;

        time_t cycle_start = time(NULL);
        int data_received = 0;

        while (running && !stop_multiwatch) {
            int timeout_ms = (REFRESH_INTERVAL - (time(NULL) - cycle_start)) * 1000/6;
            if (timeout_ms <= 0) break;

            int rc = poll(pfds, n + 1, timeout_ms);
            if (rc < 0) {
                if (errno == EINTR) {
                    if (stop_multiwatch) break;
                    continue;
                }
                perror("poll");
                break;
            }

            if (rc == 0) break; // Timeout

            // Check for X events (Ctrl+C)
            if (pfds[n].revents & POLLIN) {
                while (XPending(dpy) && running && !stop_multiwatch) {
                    XEvent ev;
                    XNextEvent(dpy, &ev);
                    if (ev.type == KeyPress) {
                        KeySym ks;
                        char kb[32];
                        XLookupString(&ev.xkey, kb, sizeof(kb), &ks, NULL);
                        if ((ev.xkey.state & ControlMask) && (ks == XK_c || ks == XK_C)) {
                            stop_multiwatch = 1;
                            running = 0;
                            break;
                        }
                    }
                }
                if (!running) break;
            }

            // Check for data from commands
            for (int i = 0; i < n; i++) {
                if (pfds[i].revents & POLLIN) {
                    char buffer[4096];
                    ssize_t bytes_read;
                    
                    while ((bytes_read = read(pipefds[i][0], buffer, sizeof(buffer) - 1)) > 0) {
                        data_received = 1;
                        buffer[bytes_read] = '\0';
                        
                        // Format output with timestamp and command name
                        time_t now = time(NULL);
                        char time_str[64];
                        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
                        
                        char formatted[5000];
                        snprintf(formatted, sizeof(formatted),
                                 "\n\"%s\", %s:\n"
                                 "----------------------------------------------------\n"
                                 "%s"
                                 "----------------------------------------------------\n",
                                 cmds[i], time_str, buffer);
                        
                        draw_output(win, gc, tab, formatted);
                    }
                    
                    if (bytes_read == 0) {
                        // EOF - command finished
                        close(pipefds[i][0]);
                        pipefds[i][0] = -1;
                        pfds[i].fd = -1;
                    } else if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        close(pipefds[i][0]);
                        pipefds[i][0] = -1;
                        pfds[i].fd = -1;
                    }
                }
            }
        }

        // Cleanup for this cycle
        free(pfds);
        
        // Kill any remaining child processes for this cycle
        for (int i = 0; i < n; i++) {
            if (pids[i] > 0) {
                kill(pids[i], SIGTERM);
                waitpid(pids[i], NULL, 0);
                pids[i] = 0;
            }
            if (pipefds[i][0] >= 0) {
                close(pipefds[i][0]);
                pipefds[i][0] = -1;
            }
        }

        // If we're stopping, break out
        if (stop_multiwatch) break;

        // Only continue if not stopped
        if (running) {
            sleep(REFRESH_INTERVAL/4);
        }
    }

cleanup:
    // Restore original signal handler
    sigaction(SIGINT, &sa_old, NULL);

    // Kill any remaining processes
    for (int i = 0; i < n; i++) {
        if (pids[i] > 0) {
            kill(pids[i], SIGTERM);
            waitpid(pids[i], NULL, 0);
        }
        if (pipefds[i][0] >= 0) {
            close(pipefds[i][0]);
        }
        if (pipefds[i][1] >= 0) {
            close(pipefds[i][1]);
        }
    }

    draw_output(win, gc, tab, "\nmultiWatch stopped.\n");
    
    // Reset terminal state properly
    tab->current_line++;
    tab->isCommand[tab->current_line] = 1;
    tab->cursor_pos = 0;
    tab->scroll_y = tab->current_line; // Scroll to show the new prompt
    tab->scroll_x = 0;
    
    // Clear any partial command
    tab->command[0] = '\0';
    //tab->lines[tab->current_line][0] = '\0';

    for(int i=tab->current_line;i<MAX_LINES;i++)
    {
        if(tab->lines[i][0]!='\0')
            tab->lines[i][0]='\0';
        //else
        //    break;
    }
    
    draw_text(win, gc, tab);
    
    // Reset the stop flag for next use
    stop_multiwatch = 0;
}

void sigint_handler(int signo) {
    if (current_child_pid > 0) {
        kill(current_child_pid, SIGINT);  // Interrupt the running child
    }
    // Don’t exit the shell itself
}
static void handle_child_and_events(pid_t child, int read_fd, Window win, GC gc, Tab *tab) {
    current_child_pid = child;

    // set pipe read end non-blocking
    int flags = fcntl(read_fd, F_GETFL, 0);
    fcntl(read_fd, F_SETFL, flags | O_NONBLOCK);

    int xfd = ConnectionNumber(dpy);
    char buf[4096];
    int child_status = 0;

    while (1) {
        // 1) check for child exit first (non-blocking)
        pid_t r = waitpid(child, &child_status, WNOHANG);
        if (r == -1) {
            // error - break
            break;
        } else if (r > 0) {
            // child exited
            break;
        }

        // 2) prepare fdsets for select: X connection fd and pipe read fd
        fd_set rfds;
        FD_ZERO(&rfds);
        if (read_fd >= 0) FD_SET(read_fd, &rfds);
        FD_SET(xfd, &rfds);
        int nf = (xfd > read_fd ? xfd : read_fd) + 1;

        // small timeout so we remain responsive
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000; // 200 ms

        int sel = select(nf, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        // 3) If X fd ready, process all pending X events
        if (FD_ISSET(xfd, &rfds)) {
            while (XPending(dpy)) {
                XEvent ev;
                XNextEvent(dpy, &ev);
                if (ev.type == KeyPress) {
                    KeySym ks;
                    char kb[32];
                    XLookupString(&ev.xkey, kb, sizeof(kb), &ks, NULL);
                    if ((ev.xkey.state & ControlMask) && (ks == XK_C || ks == XK_c)) {
                        if (current_child_pid > 0) {
                            // send SIGINT to the child's process group
                            kill(-current_child_pid, SIGINT);
                        }
                    } else {
                        // For other keys during child execution you might want to ignore or queue them.
                        // We ignore other keys here to keep behavior simple while a child runs.
                    }
                } else {
                    // keep other event handling minimal; user Expose etc. will redraw next loop
                    if (ev.type == Expose) draw_text(win, gc, tab);
                }
            }
        }

        // 4) If pipe has data, read and forward it to draw_output
        if (read_fd >= 0 && FD_ISSET(read_fd, &rfds)) {
            ssize_t rr;
            while ((rr = read(read_fd, buf, sizeof(buf) - 1)) > 0) {
                buf[rr] = '\0';
                draw_output(win, gc, tab, buf);
            }
            if (rr == 0) {
                // EOF from child stdout/stderr: close read fd
                close(read_fd);
                read_fd = -1;
            } else if (rr < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                // serious read error
                close(read_fd);
                read_fd = -1;
            }
        }
    }

    // final drain of any remaining data (non-blocking)
    if (read_fd >= 0) {
        ssize_t rr;
        while ((rr = read(read_fd, buf, sizeof(buf) - 1)) > 0) {
            buf[rr] = '\0';
            draw_output(win, gc, tab, buf);
        }
        if (read_fd >= 0) close(read_fd);
    }

    current_child_pid = -1;

    // reap any other children if necessary
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

/* ---- History file handling ---- */
static void load_history() {
    FILE *file = fopen(HISTORY_FILE, "r");
    if (!file) return;
    
    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), file) && history_count < MAX_HISTORY_SIZE) {
        // Remove newline
        line[strcspn(line, "\n")] = '\0';
        if (strlen(line) > 0) {
            strncpy(history[history_count], line, MAX_LINE_LEN - 1);
            history[history_count][MAX_LINE_LEN - 1] = '\0';
            history_count++;
        }
    }
    fclose(file);
    history_current = history_count;
}

static void save_history() {
    FILE *file = fopen(HISTORY_FILE, "w");
    if (!file) return;
    
    for (int i = 0; i < history_count; i++) {
        fprintf(file, "%s\n", history[i]);
    }
    fclose(file);
}

static void add_to_history(const char *command) {
    // Don't add empty commands or duplicates of the last command
    if (strlen(command) == 0 || 
        (history_count > 0 && strcmp(history[history_count - 1], command) == 0)) {
        return;
    }
    
    if (history_count < MAX_HISTORY_SIZE) {
        strncpy(history[history_count], command, MAX_LINE_LEN - 1);
        history[history_count][MAX_LINE_LEN - 1] = '\0';
        history_count++;
    } else {
        // Shift history down to make room
        for (int i = 1; i < MAX_HISTORY_SIZE; i++) {
            strncpy(history[i-1], history[i], MAX_LINE_LEN - 1);
            history[i-1][MAX_LINE_LEN - 1] = '\0';
        }
        strncpy(history[MAX_HISTORY_SIZE - 1], command, MAX_LINE_LEN - 1);
        history[MAX_HISTORY_SIZE - 1][MAX_LINE_LEN - 1] = '\0';
    }
    history_current = history_count;
    save_history();
}

/* ---- History search functions ---- */
static int longest_common_substring(const char *str1, const char *str2) {
    int len1 = strlen(str1);
    int len2 = strlen(str2);
    int max_len = 0;
    
    for (int i = 0; i < len1; i++) {
        for (int j = 0; j < len2; j++) {
            int len = 0;
            while (i + len < len1 && j + len < len2 && str1[i + len] == str2[j + len]) {
                len++;
            }
            if (len > max_len) {
                max_len = len;
            }
        }
    }
    return max_len;
}

static void search_history(Tab *tab, Window win, GC gc) {
    if (strlen(search_term) == 0) {
        draw_output(win, gc, tab, "No search term entered\n");
        return;
    }
    
    // First try exact match
    for (int i = history_count - 1; i >= 0; i--) {
        if (strcmp(history[i], search_term) == 0) {
            char result[MAX_LINE_LEN + 50];
            snprintf(result, sizeof(result), "Found: %s\n", history[i]);
            draw_output(win, gc, tab, result);
            return;
        }
    }
    
    // If no exact match, find best substring match
    int best_match_index = -1;
    int best_match_length = 0;
    
    for (int i = history_count - 1; i >= 0; i--) {
        int match_len = longest_common_substring(search_term, history[i]);
        if (match_len > best_match_length && match_len > 2) {
            best_match_length = match_len;
            best_match_index = i;
        }
    }
    
    if (best_match_index != -1) {
        char result[MAX_LINE_LEN + 100];
        snprintf(result, sizeof(result), "Closest match (substring length %d): %s\n", 
                 best_match_length, history[best_match_index]);
        draw_output(win, gc, tab, result);
    } else {
        draw_output(win, gc, tab, "No match for search term in history\n");
    }
}

/* ---- History command ---- */
static void show_history(Tab *tab, Window win, GC gc) {
    int start = (history_count > 1000) ? history_count - 1000 : 0;
    
    for (int i = start; i < history_count; i++) {
        char line[MAX_LINE_LEN + 20];
        snprintf(line, sizeof(line), "%5d  %s\n", i + 1, history[i]);
        draw_output(win, gc, tab, line);
    }
}
/* ---- Auto-complete functions ---- */
static char* find_common_prefix(char **strings, int count) {
    if (count == 0) return NULL;
    
    static char prefix[MAX_LINE_LEN];
    strncpy(prefix, strings[0], MAX_LINE_LEN - 1);
    prefix[MAX_LINE_LEN - 1] = '\0';
    
    for (int i = 1; i < count; i++) {
        int j = 0;
        while (prefix[j] && strings[i][j] && prefix[j] == strings[i][j]) {
            j++;
        }
        prefix[j] = '\0';
        if (j == 0) break;
    }
    return prefix;
}

static void get_files_starting_with(const char *prefix, char ***matches, int *match_count) {
    DIR *dir = opendir(".");
    if (!dir) return;
    
    struct dirent *entry;
    *matches = NULL;
    *match_count = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        // Skip hidden files and directories
        if (entry->d_name[0] == '.') continue;
        
        if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0) {
            *matches = realloc(*matches, (*match_count + 1) * sizeof(char*));
            (*matches)[*match_count] = strdup(entry->d_name);
            (*match_count)++;
        }
    }
    closedir(dir);
}

static void auto_complete(Tab *tab, Window win, GC gc) {
    // If we're already in selection mode, don't auto-complete again
    if (selection_mode) return;
    
    char *line = tab->lines[tab->current_line];
    int line_len = strlen(line);
    
    // Find the last word in the line (for completion)
    int word_start = line_len - 1;
    while (word_start >= 0 && line[word_start] != ' ' && line[word_start] != '\t') {
        word_start--;
    }
    word_start++; // Move to start of word
    
    char prefix[MAX_LINE_LEN];
    strncpy(prefix, line + word_start, MAX_LINE_LEN - 1);
    prefix[MAX_LINE_LEN - 1] = '\0';
    
    if (strlen(prefix) == 0) return;
    
    char **matches = NULL;
    int match_count = 0;
    get_files_starting_with(prefix, &matches, &match_count);
    
    if (match_count == 0) {
        // No matches - do nothing
        if (matches) free(matches);
        return;
    }
    else if (match_count == 1) {
        // Single match - complete it
        int prefix_len = strlen(prefix);
        int match_len = strlen(matches[0]);
        
        // Append the remaining part of the filename
        if (match_len > prefix_len) {
            strncat(line, matches[0] + prefix_len, MAX_LINE_LEN - line_len - 1);
            tab->cursor_pos = strlen(line);
        }
        
        // Cleanup
        for (int i = 0; i < match_count; i++) {
            free(matches[i]);
        }
        if (matches) free(matches);
    }
    else {
        // Multiple matches - enter selection mode
        selection_mode = 1;
        selection_matches = matches;
        selection_match_count = match_count;
        original_line = tab->current_line;
        strncpy(original_command, line, MAX_LINE_LEN - 1);
        original_command[MAX_LINE_LEN - 1] = '\0';
        
        // Initialize selection input
        tab->selection_input[0] = '\0';
        tab->selection_input_pos = 0;
        
        // Show all matches
        char match_list[MAX_LINE_LEN * 10] = "\n";
        for (int i = 0; i < match_count; i++) {
            char temp[100];
            snprintf(temp, sizeof(temp), "%d. %s  ", i + 1, matches[i]);
            strncat(match_list, temp, sizeof(match_list) - strlen(match_list) - 1);
            
            // Add line breaks for better formatting
            if ((i + 1) % 4 == 0) {
                strncat(match_list, "\n", sizeof(match_list) - strlen(match_list) - 1);
            }
        }

        char prompt[100];
        snprintf(prompt, sizeof(prompt), "\nEnter selection number (1-%d) and press Enter: ", match_count);
        strncat(match_list, prompt, sizeof(match_list) - strlen(match_list) - 1);
        draw_output(win, gc, tab, match_list);

        // Move to new line for selection input
        tab->current_line++;
        strcpy(tab->lines[tab->current_line], "Selection: ");
        tab->isCommand[tab->current_line] = 0;
        tab->cursor_pos = strlen(tab->lines[tab->current_line]);
        return; // Don't cleanup matches yet - we need them for selection
    }
    
    draw_text(win, gc, tab);
}

static void handle_selection_mode(Tab *tab, Window win, GC gc, KeySym ks, char buf) {
    if (!selection_mode) return;
    
    if (ks == XK_Return) {
        if (tab->selection_input_pos > 0) {
            int selection = atoi(tab->selection_input) - 1; // Convert to 0-based index
            
            if (selection >= 0 && selection < selection_match_count) {
                // Apply the selection - go back to original line and append the selected match
                tab->current_line = original_line;
                
                // Find where the partial word ends in the original command
                char *line = tab->lines[tab->current_line];
                int line_len = strlen(line);
                int word_start = line_len - 1;
                while (word_start >= 0 && line[word_start] != ' ' && line[word_start] != '\t') {
                    word_start--;
                }
                word_start++;
                
                // Replace the partial word with the selected match
                strcpy(line + word_start, selection_matches[selection]);
                tab->cursor_pos = strlen(line);
                
                // Add space after completion
                if (strlen(line) < MAX_LINE_LEN - 1) {
                    strcat(line, " ");
                    tab->cursor_pos++;
                }
                
                tab->isCommand[tab->current_line] = 1;
            } else {
                // Invalid selection number - restore original command
                tab->current_line = original_line;
                strcpy(tab->lines[tab->current_line], original_command);
                tab->cursor_pos = strlen(original_command);
                tab->isCommand[tab->current_line] = 1;
                draw_output(win, gc, tab, "Invalid selection number\n");
            }
        } else {
            // No input - restore original command
            tab->current_line = original_line;
            strcpy(tab->lines[tab->current_line], original_command);
            tab->cursor_pos = strlen(original_command);
            tab->isCommand[tab->current_line] = 1;
        }
        
        // Cleanup and exit selection mode
        selection_mode = 0;
        for (int i = 0; i < selection_match_count; i++) {
            free(selection_matches[i]);
        }
        free(selection_matches);
        selection_matches = NULL;
        
        draw_text(win, gc, tab);
        return;
    }
    else if (ks == XK_BackSpace) {
        // Handle backspace in selection input
        if (tab->selection_input_pos > 0) {
            tab->selection_input[--tab->selection_input_pos] = '\0';
            
            // Update the selection line display
            char display_line[MAX_LINE_LEN];
            snprintf(display_line, sizeof(display_line), "Selection: %s", tab->selection_input);
            strcpy(tab->lines[tab->current_line], display_line);
            tab->cursor_pos = strlen(display_line);
            draw_text(win, gc, tab);
        }
    }
    else if (ks == XK_Escape) {
        // Cancel selection on Escape
        selection_mode = 0;
        for (int i = 0; i < selection_match_count; i++) {
            free(selection_matches[i]);
        }
        free(selection_matches);
        selection_matches = NULL;
        
        // Restore original command
        tab->current_line = original_line;
        strcpy(tab->lines[tab->current_line], original_command);
        tab->cursor_pos = strlen(original_command);
        tab->isCommand[tab->current_line] = 1;
        draw_text(win, gc, tab);
    }
    else if (buf >= '0' && buf <= '9' && tab->selection_input_pos < 9) { // Limit to reasonable length
        // Accept digits for selection input
        tab->selection_input[tab->selection_input_pos++] = buf;
        tab->selection_input[tab->selection_input_pos] = '\0';
        
        // Update the selection line display
        char display_line[MAX_LINE_LEN];
        snprintf(display_line, sizeof(display_line), "Selection: %s", tab->selection_input);
        strcpy(tab->lines[tab->current_line], display_line);
        tab->cursor_pos = strlen(display_line);
        draw_text(win, gc, tab);
    }
    // Ignore other keys in selection mode
}


static void run(Window win, GC gc) {
    XEvent ev;
    pid_t child;
    int pipefd[2];
    int readend = 0, writeend = 1, l;
    char output[1000], temp[4], t1[1000];
    int output_bytes;


    total_tabs = 1;
    current_tab = 0;
    init_tab(&tabs[0]);
    tabs[0].command[0] = '\0';

    while (1) {
        XNextEvent(dpy, &ev);
        Tab *tab = &tabs[current_tab];

        switch (ev.type) {
            case ConfigureNotify:
                // Window resize
                if (ev.xconfigure.width != win_width || ev.xconfigure.height != win_height) {
                    win_width = ev.xconfigure.width;
                    win_height = ev.xconfigure.height;
                    draw_text(win, gc, tab); // Redraw with new dimensions
                }
                break;

            case Expose:
                draw_text(win, gc, tab);
                break;

            case KeyPress: {
                KeySym ks;
                char buf[32];
                int len = XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL);
                tab = &tabs[current_tab];


                // ---------- SCROLLING ----------
                if (ks == XK_Up)
                {
                    if (tab->scroll_y > 0)
                        tab->scroll_y--;
                    draw_text(win, gc, tab);
                    continue;
                }
                else if (ks == XK_Down)
                {
                    if (tab->scroll_y < tab->current_line)
                        tab->scroll_y++;
                    draw_text(win, gc, tab);
                    continue;
                }
                else if (ks == XK_Left)
                {
                    if (tab->scroll_x > 0)
                        tab->scroll_x--;
                    draw_text(win, gc, tab);
                    continue;
                }
                else if (ks == XK_Right)
                {
                    tab->scroll_x++;
                    draw_text(win, gc, tab);
                    continue;
                }

                // Handling search mode
                if (search_mode)
                {
                    if (ks == XK_Return)
                    {
                        search_mode = 0;
                        // Clear the search prompt line and show results
                        tab->current_line++;
                        search_history(tab, win, gc);
                        // Reset for next command
                        tab->current_line++;
                        tab->isCommand[tab->current_line] = 1;
                        tab->cursor_pos = 0;
                        tab->command[0] = '\0';
                        // Ensure the current line is visible
                        if (tab->current_line > tab->scroll_y + (HEIGHT - 40) / 20 - 1)
                        {
                            tab->scroll_y = tab->current_line - (HEIGHT - 40) / 20 + 1;
                        }
                        draw_text(win, gc, tab);
                    }
                    else if (ks == XK_BackSpace)
                    {
                        if (search_cursor > 0)
                        {
                            search_term[--search_cursor] = '\0';
                        }
                        // Update the current line with the search prompt (safe version)
                        strncpy(tab->lines[tab->current_line], "Enter search term: ", MAX_LINE_LEN - 1);
                        strncat(tab->lines[tab->current_line], search_term, MAX_LINE_LEN - strlen(tab->lines[tab->current_line]) - 1);
                        tab->lines[tab->current_line][MAX_LINE_LEN - 1] = '\0';
                        tab->cursor_pos = strlen(tab->lines[tab->current_line]);
                        // Ensure cursor is visible horizontally
                        if (tab->cursor_pos > tab->scroll_x + 80)
                        { // Assuming ~80 chars visible
                            tab->scroll_x = tab->cursor_pos - 80;
                        }
                        else if (tab->cursor_pos < tab->scroll_x)
                        {
                            tab->scroll_x = tab->cursor_pos;
                        }
                        draw_text(win, gc, tab);
                    }
                    else if (ks == XK_Escape)
                    {
                        search_mode = 0;
                        // Clear search and return to normal prompt
                        tab->current_line++;
                        tab->isCommand[tab->current_line] = 1;
                        tab->cursor_pos = 0;
                        tab->command[0] = '\0';
                        draw_text(win, gc, tab);
                    }
                    else if (len > 0 && search_cursor < MAX_LINE_LEN - 20)
                    { // Reserve 20 chars for prompt
                        search_term[search_cursor++] = buf[0];
                        search_term[search_cursor] = '\0';
                        // Update the current line with the search prompt (safe version)
                        strncpy(tab->lines[tab->current_line], "Enter search term: ", MAX_LINE_LEN - 1);
                        strncat(tab->lines[tab->current_line], search_term, MAX_LINE_LEN - strlen(tab->lines[tab->current_line]) - 1);
                        tab->lines[tab->current_line][MAX_LINE_LEN - 1] = '\0';
                        tab->cursor_pos = strlen(tab->lines[tab->current_line]);
                        // Ensure cursor is visible horizontally
                        if (tab->cursor_pos > tab->scroll_x + 80)
                        { // Assuming ~80 chars visible
                            tab->scroll_x = tab->cursor_pos - 80;
                        }
                        else if (tab->cursor_pos < tab->scroll_x)
                        {
                            tab->scroll_x = tab->cursor_pos;
                        }
                        draw_text(win, gc, tab);
                    }
                    continue; // Important: skip all other key handling in search mode
                }
                
                // Selection mode handling for autocomplete
                if (selection_mode && len > 0)
                {
                    handle_selection_mode(tab, win, gc, ks, buf[0]);
                    for(int i=tab->current_line+1;i<MAX_LINES;i++)
                    {
                        if(tab->lines[i][0]!='\0')
                            tab->lines[i][0]='\0';
                        else
                            break;
                    }
                    continue;
                }

                // Tab key handling
                if (ks == XK_Tab)
                {
                    if (selection_mode)
                    {
                        // Ignore Tab in selection mode
                    }
                    else if (ev.xkey.state & ControlMask)
                    {
                        // Ctrl+Tab for tab switching
                        current_tab = (current_tab + 1) % total_tabs;
                        draw_text(win, gc, &tabs[current_tab]);
                    }
                    else
                    {
                        // Regular Tab for auto-complete
                        if (current_child_pid > 0)
                        {
                            // Don't auto-complete while command is running
                        }
                        else
                        {
                            auto_complete(tab, win, gc);
                        }
                    }
                    continue;
                }
                // CTRL+R for history search (this activates search mode)
                if ((ev.xkey.state & ControlMask) && (ks == XK_R || ks == XK_r))
                {
                    if (current_child_pid > 0)
                    {
                        draw_output(win, gc, tab, "Cannot search history while command is running\n");
                    }
                    else
                    {
                        search_mode = 1;
                        search_term[0] = '\0';
                        search_cursor = 0;
                        // Set up the search prompt on the current line (safe version)
                        strncpy(tab->lines[tab->current_line], "Enter search term: ", MAX_LINE_LEN - 1);
                        tab->lines[tab->current_line][MAX_LINE_LEN - 1] = '\0';
                        tab->cursor_pos = strlen(tab->lines[tab->current_line]);
                        tab->isCommand[tab->current_line] = 0; // This is not a regular command input
                        draw_text(win, gc, tab);
                    }
                    continue;
                }
                //CTRL+C handling
                if ((ev.xkey.state & ControlMask) && (ks == XK_C || ks == XK_c))
                {
                    if (current_child_pid > 0)
                    {
                        kill(-current_child_pid, SIGINT);
                    } else {
                        draw_text(win, gc,tab);
                    }
                    continue;
                    // continue handling other
                }

                //CTRL+Z handling
                if ((ev.xkey.state & ControlMask) && (ks == XK_Z || ks == XK_z))
                {
                    if (current_child_pid > 0)
                    {
                        // send SIGTSTP to the whole process group
                        kill(-current_child_pid, SIGTSTP);
                        // The monitoring loop will detect the stopped status and break
                    }
                    continue;
                }
                // CTRL+A and CTRL+E line navigation
                if ((ev.xkey.state & ControlMask) && (ks == XK_A || ks == XK_a))
                {
                    tab->cursor_pos = 0; // Move to start of line
                    draw_text(win, gc, tab);
                    continue;
                }
                if ((ev.xkey.state & ControlMask) && (ks == XK_E || ks == XK_e))
                {
                    int cur_len = strlen(tab->lines[tab->current_line]);
                    tab->cursor_pos = (cur_len < MAX_LINE_LEN) ? cur_len : MAX_LINE_LEN - 1;
                    draw_text(win, gc, tab);
                    continue;
                }

                // ---------- TAB SHORTCUTS ----------
                if ((ev.xkey.state & ControlMask) && (ks == XK_t || ks == XK_T)) {
                    if (total_tabs < MAX_TABS) {
                        init_tab(&tabs[total_tabs]);
                        tabs[total_tabs].command[0] = '\0';
                        total_tabs++;
                        current_tab = total_tabs - 1;
                        draw_text(win, gc, &tabs[current_tab]);
                    }
                    continue;
                }

                /*
                if ((ev.xkey.state & ControlMask) && ks == XK_Tab) {
                    current_tab = (current_tab + 1) % total_tabs;
                    draw_text(win, gc, &tabs[current_tab]);
                    break;
                }
                */

                // ---------- COMMAND EXECUTION ----------
                if (ks == XK_Return) {
                    if (tab->current_line < MAX_LINES - 1)
                    {
                        int l = strlen(tab->lines[tab->current_line]);
                        if (l >= 3)
                            strncpy(temp, tab->lines[tab->current_line] + l - 3, 3);
                        else
                            strncpy(temp, tab->lines[tab->current_line], l);
                        temp[(l >= 3) ? 3 : l] = '\0';

                        if (strcmp(temp, "\\n\\") != 0)
                        { // last line of multi-line or single-line input
                            // Append current line to tab->command (without the trailing \n\ if present)
                            if (strlen(tab->command) > 0)
                            {
                                strcat(tab->command, tab->lines[tab->current_line]);
                            }
                            else
                            {
                                strncpy(tab->command, tab->lines[tab->current_line], sizeof(tab->command) - 1);
                                tab->command[sizeof(tab->command) - 1] = '\0';
                            }

                            // Adding command to history
                            add_to_history(tab->command);

                            // Handle built-in history command
                            if (strcmp(tab->command, "history") == 0)
                            {
                                show_history(tab, win, gc);
                                tab->current_line++;
                                tab->isCommand[tab->current_line] = 1;
                                tab->cursor_pos = 0;
                                tab->command[0] = '\0';
                                continue;
                            }
                            // Handle multiWatch command
                            else if (strncmp(tab->command, "multiWatch", 10) == 0)
                            {
                                // Save current state
                                int saved_line = tab->current_line;
                                int saved_cursor = tab->cursor_pos;

                                multiWatch(tab, win, gc, tab->command);

                                // Ensure we're on a fresh command line
                                if (tab->current_line <= saved_line)
                                {
                                    tab->current_line = saved_line + 1;
                                }

                                tab->isCommand[tab->current_line] = 1;
                                tab->cursor_pos = 0;
                                tab->command[0] = '\0';
                                tab->lines[tab->current_line][0] = '\0'; // Clear the line

                                // Make sure the prompt is visible
                                tab->scroll_y = tab->current_line;
                                tab->scroll_x = 0;

                                draw_text(win, gc, tab);
                                continue;
                            }

                            // Handling the "exit" command
                            if (strcmp(tab->command, "exit") == 0)
                            {
                                // Clean up and exit
                                save_history();

                                // Kill all shell processes in all tabs
                                for (int i = 0; i < total_tabs; i++)
                                {
                                    if (tabs[i].shell_pid > 0)
                                    {
                                        kill(tabs[i].shell_pid, SIGTERM);
                                    }
                                }

                                // Cleanup X11 resources
                                XUngrabKeyboard(dpy, CurrentTime);
                                XUnmapWindow(dpy, win);
                                XDestroyWindow(dpy, win);
                                XFreeGC(dpy, gc);
                                XCloseDisplay(dpy);

                                exit(0);
                            }

                            // ---- normal command execution using tab->command ----
                            pipe(pipefd);
                            pid_t child = fork();

                            if (child == 0)
                            {
                                setpgid(0, 0);
                                signal(SIGINT, SIG_DFL);
                                signal(SIGTSTP, SIG_DFL); 

                                close(pipefd[0]);
                                dup2(pipefd[1], STDOUT_FILENO);
                                dup2(pipefd[1], STDERR_FILENO);
                                close(pipefd[1]);

                                if (strchr(tab->command, '|'))
                                {
                                    execute_piped_command(tab->command);
                                    _exit(0);
                                }

                                // Handle redirection
                                char *cmd_copy = strdup(tab->command);
                                char *infile = NULL, *outfile = NULL;
                                char *in_pos = strchr(cmd_copy, '<');
                                char *out_pos = strchr(cmd_copy, '>');

                                if (in_pos)
                                {
                                    *in_pos = '\0';
                                    infile = strtok(in_pos + 1, " \t\n");
                                }
                                if (out_pos)
                                {
                                    *out_pos = '\0';
                                    outfile = strtok(out_pos + 1, " \t\n");
                                }

                                char command_clean[1000];
                                snprintf(command_clean, sizeof(command_clean), "%s", cmd_copy);

                                if (infile)
                                {
                                    int fd_in = open(infile, O_RDONLY);
                                    if (fd_in < 0)
                                        _exit(1);
                                    dup2(fd_in, STDIN_FILENO);
                                    close(fd_in);
                                }

                                if (outfile)
                                {
                                    int fd_out = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                                    if (fd_out < 0)
                                        _exit(1);
                                    dup2(fd_out, STDOUT_FILENO);
                                    close(fd_out);
                                }
                                
                                execlp("sh", "sh", "-c", command_clean, NULL);
                                _exit(1);
                            }
                            // ... inside the KeyPress case, where you fork and execute commands ...
                            else if (child > 0)
                            {
                                setpgid(child, child);
                                current_child_pid = child;
                                close(pipefd[1]);

                                // Set non-blocking mode for child's pipe read-end
                                int flags = fcntl(pipefd[0], F_GETFL, 0);
                                fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

                                int xfd = ConnectionNumber(dpy);
                                char buf[4096];
                                int status;
                                int child_stopped = 0;

                                while (1)
                                {
                                    // 1. Check if child has exited or stopped
                                    pid_t done = waitpid(child, &status, WUNTRACED | WNOHANG);
                                    if (done > 0)
                                    {
                                        if (WIFSTOPPED(status))
                                        {
                                            background_jobs[bg_count++] = child;
                                            child_stopped = 1;
                                            current_child_pid = -1;
                                            draw_output(win, gc, tab, "[Process moved to background]\n");
                                            break;
                                        }
                                        else if (WIFEXITED(status) || WIFSIGNALED(status))
                                        {
                                            current_child_pid = -1;
                                            break;
                                        }
                                    }

                                    // 2. Prepare fd sets for select
                                    fd_set rfds;
                                    FD_ZERO(&rfds);
                                    FD_SET(pipefd[0], &rfds);
                                    FD_SET(xfd, &rfds);
                                    int maxfd = (pipefd[0] > xfd ? pipefd[0] : xfd) + 1;

                                    struct timeval tv = {0, 200000}; // 200ms timeout
                                    int sel = select(maxfd, &rfds, NULL, NULL, &tv);
                                    if (sel < 0 && errno != EINTR)
                                        break;

                                    // 3. Handle X events (detect Ctrl+C and Ctrl+Z)
                                    if (FD_ISSET(xfd, &rfds))
                                    {
                                        while (XPending(dpy))
                                        {
                                            XEvent ev2;
                                            XNextEvent(dpy, &ev2);
                                            if (ev2.type == KeyPress)
                                            {
                                                KeySym ks2;
                                                char kbuf[32];
                                                XLookupString(&ev2.xkey, kbuf, sizeof(kbuf), &ks2, NULL);
                                                if ((ev2.xkey.state & ControlMask) && (ks2 == XK_C || ks2 == XK_c))
                                                {
                                                    if (current_child_pid > 0)
                                                        kill(-current_child_pid, SIGINT);
                                                }
                                                else if ((ev2.xkey.state & ControlMask) && (ks2 == XK_Z || ks2 == XK_z))
                                                {
                                                    if (current_child_pid > 0)
                                                    {
                                                        kill(-current_child_pid, SIGTSTP);
                                                        // The WIFSTOPPED check above will handle this
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    // 4. Read output from child if available
                                    if (FD_ISSET(pipefd[0], &rfds))
                                    {
                                        ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
                                        if (n > 0)
                                        {
                                            buf[n] = '\0';
                                            draw_output(win, gc, tab, buf);
                                        }
                                    }

                                    // If child was stopped via Ctrl+Z, break out
                                    if (child_stopped)
                                        break;
                                }

                                // Only close pipe if child wasn't stopped (if stopped, we might want to resume later)
                                if (!child_stopped)
                                {
                                    current_child_pid = -1;
                                    close(pipefd[0]);
                                }
                            }

                            // Reset tab->command for next command
                            tab->command[0] = '\0';
                            tab->current_line++;
                            tab->isCommand[tab->current_line] = 1;
                            tab->cursor_pos = 0;
                        }
                        else
                        { // multi-line continuation
                            strcat(tab->command, tab->lines[tab->current_line]);
                            strcat(tab->command, "\n"); // preserve the new line
                            tab->current_line++;
                            tab->isCommand[tab->current_line] = 0;
                            tab->cursor_pos = 0;
                        }
                    }
                }
                else if (ks == XK_BackSpace) {
                    if (tab->cursor_pos > 0)
                    {
                        int line_len = strlen(tab->lines[tab->current_line]);
                        // shift characters left
                        for (int i = tab->cursor_pos - 1; i < line_len; i++)
                        {
                            tab->lines[tab->current_line][i] = tab->lines[tab->current_line][i + 1];
                        }
                        tab->cursor_pos--;
                    }
                }
                else if (len > 0 && tab->cursor_pos < MAX_LINE_LEN - 1) {
                    int line_len = strlen(tab->lines[tab->current_line]);
                    if (line_len >= MAX_LINE_LEN - 1)
                        line_len = MAX_LINE_LEN - 2;

                    // shift characters right
                    for (int i = line_len; i >= tab->cursor_pos; i--)
                    {
                        tab->lines[tab->current_line][i + 1] = tab->lines[tab->current_line][i];
                    }

                    // insert new character
                    tab->lines[tab->current_line][tab->cursor_pos] = buf[0];
                    tab->cursor_pos++;
                }
                
                draw_text(win, gc, tab);
                break;
            }

            case ButtonPress: {
                int x = ev.xbutton.x, y = ev.xbutton.y;
                if (y < 30) {
                    int clicked = x / 70;
                    if (clicked < total_tabs) {
                        current_tab = clicked;
                        draw_text(win, gc, &tabs[current_tab]);
                    }
                }
                break;
            }
        }
    }
}
int main() {
    dpy = XOpenDisplay(NULL);
    if (!dpy) errx(1, "Cannot open display");

    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    // Get screen dimensions for better initial size
    win_width = DisplayWidth(dpy, screen) / 2;
    win_height = DisplayHeight(dpy, screen) / 2;

    Window win = create_window();
    GC gc = create_gc(win);
    
    XFontStruct *font = XLoadQueryFont(dpy, "10x20");
    XSetFont(dpy, gc, font->fid);

    XMapWindow(dpy, win);
    XFlush(dpy);
    XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime);

    init_tab(&tabs[0]);
    
    load_history();
    
    struct sigaction sa;

    // SIGINT handler (Ctrl+C)
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);

    // SIGTSTP handler (Ctrl+Z) → ignore in the shell
    sa.sa_handler = SIG_IGN;   // ignore Ctrl+Z in shell
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTSTP, &sa, NULL);

    run(win, gc);  // This will return when exit command is called

    // Cleanup after run() returns
    save_history();

    // Kill all shell processes
    for (int i = 0; i < total_tabs; i++) {
        if (tabs[i].shell_pid > 0) {
            kill(tabs[i].shell_pid, SIGTERM);
        }
    }
    
    // Kill any background jobs
    for (int i = 0; i < bg_count; i++) {
        if (background_jobs[i] > 0) {
            kill(background_jobs[i], SIGTERM);
        }
    }

    XUngrabKeyboard(dpy, CurrentTime);
    XUnmapWindow(dpy, win);
    XDestroyWindow(dpy, win);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    
    return 0;
}