#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#define PREFERRED_SUBSTR "Sony_Interactive_Entertainment_Wireless_Headset"
#define APP_NAME "Headset Recorder"

typedef struct {
    char name[512];
    char state[32];
} Source;

typedef struct {
    GtkWidget *window;
    GtkWidget *combo;
    GtkWidget *folder_btn;
    GtkWidget *name_entry;
    GtkWidget *record_btn;
    GtkWidget *status;
    GtkWidget *level;

    atomic_int running;
    pthread_t thread;
    pid_t ff_pid;
    int ff_in;          /* write 'q' here to stop ffmpeg cleanly */
    int ff_err;         /* ffmpeg stderr */
    char source_name[512];
    char path[1024];
    double seconds;
} App;

static char *suggested_basename(void);
static char *config_path(void);
static char *default_save_folder(void);
static void load_config(App *app);
static void save_config(App *app);

/* ---- source list ------------------------------------------------------- */

static GList *list_sources(void)
{
    FILE *p = popen("pactl list short sources 2>/dev/null", "r");
    if (!p)
        return NULL;

    GList *out = NULL;
    char line[1024];
    while (fgets(line, sizeof line, p)) {
        char idx[32], name[512], driver[128], fmt[128], state[32];
        if (sscanf(line, "%31s %511s %127s %127s %31s",
                   idx, name, driver, fmt, state) < 5)
            continue;
        Source *s = g_new0(Source, 1);
        g_strlcpy(s->name, name, sizeof s->name);
        g_strlcpy(s->state, state, sizeof s->state);
        out = g_list_append(out, s);
    }
    pclose(p);
    return out;
}

static void free_sources(GList *list)
{
    g_list_free_full(list, g_free);
}

static void reload_sources(App *app, gboolean keep_sel)
{
    char *prev = NULL;
    if (keep_sel) {
        const char *cur = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app->combo));
        if (cur)
            prev = g_strdup(cur);
    }

    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->combo));

    GList *srcs = list_sources();
    int preferred = -1, first_mon = -1, i = 0;
    for (GList *l = srcs; l; l = l->next, i++) {
        Source *s = l->data;
        char label[640];
        g_snprintf(label, sizeof label, "%s  [%s]", s->name, s->state);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->combo),
                                  s->name, label);
        if (preferred < 0 && strstr(s->name, PREFERRED_SUBSTR) &&
            strstr(s->name, ".monitor"))
            preferred = i;
        if (first_mon < 0 && strstr(s->name, ".monitor") &&
            g_ascii_strcasecmp(s->state, "RUNNING") == 0)
            first_mon = i;
    }

    int pick = 0;
    if (prev) {
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->combo), prev))
            pick = preferred >= 0 ? preferred : (first_mon >= 0 ? first_mon : 0);
        else
            pick = -1;
        g_free(prev);
    } else {
        pick = preferred >= 0 ? preferred : (first_mon >= 0 ? first_mon : 0);
    }
    if (pick >= 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->combo), pick);

    free_sources(srcs);
}

/* ---- UI pump ----------------------------------------------------------- */

typedef struct {
    App *app;
    char *msg;
    double seconds;
    gboolean done;
    gboolean ok;
} UiUpdate;

static gboolean apply_ui(gpointer data)
{
    UiUpdate *u = data;
    App *app = u->app;

    if (u->msg)
        gtk_label_set_text(GTK_LABEL(app->status), u->msg);

    if (u->seconds >= 0.0) {
        double tick = fmod(u->seconds, 1.0);
        gtk_level_bar_set_value(GTK_LEVEL_BAR(app->level),
                                tick < 0.5 ? 0.35 : 0.7);
    }

    if (u->done) {
        gtk_button_set_label(GTK_BUTTON(app->record_btn), "Record");
        gtk_widget_set_sensitive(app->combo, TRUE);
        gtk_widget_set_sensitive(app->folder_btn, TRUE);
        gtk_widget_set_sensitive(app->name_entry, TRUE);
        gtk_style_context_remove_class(
            gtk_widget_get_style_context(app->record_btn), "destructive-action");
        gtk_style_context_add_class(
            gtk_widget_get_style_context(app->record_btn), "suggested-action");
        gtk_level_bar_set_value(GTK_LEVEL_BAR(app->level), 0.0);
        if (u->ok) {
            char buf[1280];
            g_snprintf(buf, sizeof buf,
                       "Saved %s  (%.1f s, 48 kHz stereo float32 WAV via ffmpeg)",
                       app->path, u->seconds);
            gtk_label_set_text(GTK_LABEL(app->status), buf);
            g_autofree char *next = suggested_basename();
            gtk_entry_set_text(GTK_ENTRY(app->name_entry), next);
        }
    }

    g_free(u->msg);
    g_free(u);
    return G_SOURCE_REMOVE;
}

static void push_ui(App *app, const char *msg, double seconds,
                    gboolean done, gboolean ok)
{
    UiUpdate *u = g_new0(UiUpdate, 1);
    u->app = app;
    u->msg = msg ? g_strdup(msg) : NULL;
    u->seconds = seconds;
    u->done = done;
    u->ok = ok;
    g_idle_add(apply_ui, u);
}

static double parse_ffmpeg_time(const char *line)
{
    const char *p = strstr(line, "time=");
    if (!p)
        return -1.0;
    int h = 0, m = 0;
    float s = 0.f;
    if (sscanf(p + 5, "%d:%d:%f", &h, &m, &s) == 3)
        return h * 3600.0 + m * 60.0 + s;
    return -1.0;
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void reap_ffmpeg(App *app, int send_q)
{
    if (app->ff_pid <= 0)
        return;

    if (send_q && app->ff_in >= 0) {
        ssize_t ign = write(app->ff_in, "q\n", 2);
        (void)ign;
        close_fd(&app->ff_in);
    }

    int status = 0;
    for (int i = 0; i < 50; i++) {
        pid_t r = waitpid(app->ff_pid, &status, WNOHANG);
        if (r == app->ff_pid) {
            app->ff_pid = -1;
            close_fd(&app->ff_err);
            return;
        }
        usleep(40000);
    }

    kill(app->ff_pid, SIGINT);
    usleep(200000);
    if (waitpid(app->ff_pid, &status, WNOHANG) != app->ff_pid) {
        kill(app->ff_pid, SIGKILL);
        waitpid(app->ff_pid, &status, 0);
    }
    app->ff_pid = -1;
    close_fd(&app->ff_in);
    close_fd(&app->ff_err);
}

static int spawn_ffmpeg(App *app)
{
    int inpipe[2] = {-1, -1};
    int errpipe[2] = {-1, -1};
    if (pipe(inpipe) != 0 || pipe(errpipe) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        dup2(inpipe[0], STDIN_FILENO);
        dup2(errpipe[1], STDERR_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        close(inpipe[0]);
        close(inpipe[1]);
        close(errpipe[0]);
        close(errpipe[1]);
        execlp("ffmpeg", "ffmpeg",
               "-hide_banner",
               "-loglevel", "info",
               "-y",
               "-f", "pulse",
               "-i", app->source_name,
               "-ac", "2",
               "-ar", "48000",
               "-c:a", "pcm_f32le",
               app->path,
               (char *)NULL);
        _exit(127);
    }

    close(inpipe[0]);
    close(errpipe[1]);
    app->ff_pid = pid;
    app->ff_in = inpipe[1];
    app->ff_err = errpipe[0];
    fcntl(app->ff_err, F_SETFL, O_NONBLOCK);
    return 0;
}

static void *record_thread(void *arg)
{
    App *app = arg;
    char chunk[1024];
    char line[2048];
    size_t llen = 0;
    double seconds = 0.0;
    time_t last_ui = 0;
    gboolean saw_error = FALSE;
    char errbuf[256] = {0};

    if (spawn_ffmpeg(app) != 0) {
        push_ui(app, "Failed to start ffmpeg.", 0.0, TRUE, FALSE);
        atomic_store(&app->running, 0);
        return NULL;
    }

    while (atomic_load(&app->running) || app->ff_pid > 0) {
        if (!atomic_load(&app->running) && app->ff_pid > 0)
            reap_ffmpeg(app, TRUE);

        ssize_t n = -1;
        if (app->ff_err >= 0)
            n = read(app->ff_err, chunk, sizeof chunk);

        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                char c = chunk[i];
                if (c == '\r')
                    c = '\n';
                if (llen + 1 < sizeof line)
                    line[llen++] = c;
                if (c == '\n' || llen + 1 >= sizeof line) {
                    line[llen] = 0;
                    double t = parse_ffmpeg_time(line);
                    if (t >= 0.0)
                        seconds = t;
                    if (strcasestr(line, "error") ||
                        strstr(line, "Unknown input format") ||
                        strstr(line, "Connection refused")) {
                        saw_error = TRUE;
                        g_strlcpy(errbuf, line, sizeof errbuf);
                        g_strstrip(errbuf);
                    }
                    llen = 0;
                }
            }
        } else if (n == 0) {
            /* stderr closed — process exiting */
            if (app->ff_pid > 0) {
                int st = 0;
                waitpid(app->ff_pid, &st, 0);
                app->ff_pid = -1;
            }
            close_fd(&app->ff_err);
            close_fd(&app->ff_in);
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }

        if (app->ff_pid > 0) {
            int st = 0;
            pid_t r = waitpid(app->ff_pid, &st, WNOHANG);
            if (r == app->ff_pid) {
                app->ff_pid = -1;
                /* drain remaining stderr on next loops */
            }
        }

        time_t now = time(NULL);
        if (now != last_ui && atomic_load(&app->running)) {
            last_ui = now;
            app->seconds = seconds;
            char m[128];
            int sec = (int)seconds;
            g_snprintf(m, sizeof m, "Recording  %02d:%02d  (ffmpeg → float32 WAV)",
                       sec / 60, sec % 60);
            push_ui(app, m, seconds, FALSE, TRUE);
        }

        if (app->ff_pid <= 0 && (app->ff_err < 0 || n == 0))
            break;

        usleep(30000);
    }

    reap_ffmpeg(app, FALSE);
    atomic_store(&app->running, 0);
    app->seconds = seconds;

    if (saw_error && seconds < 0.2) {
        char m[320];
        g_snprintf(m, sizeof m, "ffmpeg failed: %s",
                   errbuf[0] ? errbuf : "see terminal");
        push_ui(app, m, seconds, TRUE, FALSE);
    } else {
        push_ui(app, NULL, seconds, TRUE, TRUE);
    }
    return NULL;
}

/* ---- settings / naming ------------------------------------------------- */

static char *config_path(void)
{
    return g_build_filename(g_get_user_config_dir(),
                            "headset-recorder", "config.ini", NULL);
}

static char *default_save_folder(void)
{
    const char *home = g_get_home_dir();
    char *music = g_build_filename(home, "Music", NULL);
    if (g_file_test(music, G_FILE_TEST_IS_DIR))
        return music;
    g_free(music);
    return g_strdup(home);
}

static char *suggested_basename(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return g_strdup_printf("recording-%04d%02d%02d-%02d%02d%02d",
                           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                           tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void load_config(App *app)
{
    g_autofree char *path = config_path();
    g_autofree char *folder = NULL;
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
        folder = g_key_file_get_string(kf, "settings", "save_folder", NULL);
    g_key_file_free(kf);

    if (!folder || !g_file_test(folder, G_FILE_TEST_IS_DIR)) {
        g_free(folder);
        folder = default_save_folder();
    }
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(app->folder_btn), folder);
}

static void save_config(App *app)
{
    g_autofree char *folder =
        gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(app->folder_btn));
    if (!folder)
        return;

    g_autofree char *path = config_path();
    g_autofree char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);

    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_key_file_set_string(kf, "settings", "save_folder", folder);
    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
}

static void on_folder_set(GtkFileChooserButton *btn, gpointer data)
{
    (void)btn;
    save_config(data);
}

static char *sanitize_basename(const char *raw)
{
    if (!raw)
        return NULL;
    g_autofree char *base = g_path_get_basename(raw);
    g_strstrip(base);
    for (char *p = base; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == '\n' || *p == '\r' || *p == '\t')
            *p = '-';
    }
    if (!base[0] || g_strcmp0(base, ".") == 0 || g_strcmp0(base, "..") == 0)
        return NULL;
    if (g_str_has_suffix(base, ".wav") || g_str_has_suffix(base, ".WAV"))
        return g_strdup(base);
    return g_strconcat(base, ".wav", NULL);
}

static gboolean confirm_overwrite(App *app, const char *path)
{
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE,
        "“%s” already exists. Overwrite it?",
        path);
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
                           "_Cancel", GTK_RESPONSE_CANCEL,
                           "_Overwrite", GTK_RESPONSE_ACCEPT,
                           NULL);
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    return resp == GTK_RESPONSE_ACCEPT;
}

static void stop_recording(App *app)
{
    if (!atomic_load(&app->running) && app->ff_pid <= 0)
        return;
    atomic_store(&app->running, 0);
    pthread_join(app->thread, NULL);
}

static gboolean ffmpeg_present(void)
{
    gchar *p = g_find_program_in_path("ffmpeg");
    if (!p)
        return FALSE;
    g_free(p);
    return TRUE;
}

static void on_record(GtkButton *btn, gpointer data)
{
    App *app = data;
    (void)btn;

    if (atomic_load(&app->running)) {
        stop_recording(app);
        return;
    }

    if (!ffmpeg_present()) {
        gtk_label_set_text(GTK_LABEL(app->status),
                           "ffmpeg not found on PATH.");
        return;
    }

    const char *src = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app->combo));
    if (!src || !*src) {
        gtk_label_set_text(GTK_LABEL(app->status), "No source selected.");
        return;
    }

    g_autofree char *folder =
        gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(app->folder_btn));
    if (!folder || !g_file_test(folder, G_FILE_TEST_IS_DIR)) {
        gtk_label_set_text(GTK_LABEL(app->status),
                           "Choose a save folder in Settings.");
        return;
    }
    if (access(folder, W_OK) != 0) {
        gtk_label_set_text(GTK_LABEL(app->status),
                           "Save folder is not writable.");
        return;
    }

    g_autofree char *base =
        sanitize_basename(gtk_entry_get_text(GTK_ENTRY(app->name_entry)));
    if (!base) {
        gtk_label_set_text(GTK_LABEL(app->status),
                           "Enter a recording name before you start.");
        gtk_widget_grab_focus(app->name_entry);
        return;
    }

    g_autofree char *chosen = g_build_filename(folder, base, NULL);
    if (g_file_test(chosen, G_FILE_TEST_EXISTS) &&
        !confirm_overwrite(app, chosen))
        return;

    g_strlcpy(app->path, chosen, sizeof app->path);
    g_strlcpy(app->source_name, src, sizeof app->source_name);

    app->ff_pid = -1;
    app->ff_in = -1;
    app->ff_err = -1;
    app->seconds = 0.0;
    atomic_store(&app->running, 1);
    if (pthread_create(&app->thread, NULL, record_thread, app) != 0) {
        atomic_store(&app->running, 0);
        gtk_label_set_text(GTK_LABEL(app->status), "Could not start record thread.");
        return;
    }

    gtk_button_set_label(GTK_BUTTON(app->record_btn), "Stop");
    gtk_widget_set_sensitive(app->combo, FALSE);
    gtk_widget_set_sensitive(app->folder_btn, FALSE);
    gtk_widget_set_sensitive(app->name_entry, FALSE);
    gtk_style_context_remove_class(
        gtk_widget_get_style_context(app->record_btn), "suggested-action");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(app->record_btn), "destructive-action");
    gtk_label_set_text(GTK_LABEL(app->status), "Recording…");
}

static void on_refresh(GtkButton *btn, gpointer data)
{
    (void)btn;
    App *app = data;
    if (atomic_load(&app->running))
        return;
    reload_sources(app, TRUE);
    gtk_label_set_text(GTK_LABEL(app->status), "Source list refreshed.");
}

static gboolean on_close(GtkWidget *w, GdkEvent *e, gpointer data)
{
    (void)w;
    (void)e;
    App *app = data;
    stop_recording(app);
    return FALSE;
}

static void activate(GtkApplication *ga, gpointer data)
{
    (void)data;
    App *app = g_new0(App, 1);
    atomic_store(&app->running, 0);
    app->ff_pid = -1;
    app->ff_in = -1;
    app->ff_err = -1;

    app->window = gtk_application_window_new(ga);
    gtk_window_set_title(GTK_WINDOW(app->window), APP_NAME);
    gtk_window_set_icon_name(GTK_WINDOW(app->window), "headset-recorder");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 640, 320);
    gtk_container_set_border_width(GTK_CONTAINER(app->window), 16);
    g_signal_connect(app->window, "delete-event", G_CALLBACK(on_close), app);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(app->window), box);

    GtkWidget *hint = gtk_label_new(
        "Uses ffmpeg -f pulse to record the selected monitor to 48 kHz stereo 32-bit float WAV.\n"
        "Pick the headset .monitor source so you capture what the headphones hear.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.f);
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    app->combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(app->combo, TRUE);
    gtk_box_pack_start(GTK_BOX(row), app->combo, TRUE, TRUE, 0);

    GtkWidget *refresh = gtk_button_new_with_label("Refresh");
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh), app);
    gtk_box_pack_start(GTK_BOX(row), refresh, FALSE, FALSE, 0);

    GtkWidget *frame = gtk_frame_new("Settings");
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    gtk_container_add(GTK_CONTAINER(frame), grid);

    GtkWidget *folder_lbl = gtk_label_new("Save folder");
    gtk_widget_set_halign(folder_lbl, GTK_ALIGN_START);
    app->folder_btn = gtk_file_chooser_button_new(
        "Choose folder", GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    gtk_widget_set_hexpand(app->folder_btn, TRUE);
    g_signal_connect(app->folder_btn, "file-set", G_CALLBACK(on_folder_set), app);
    gtk_grid_attach(GTK_GRID(grid), folder_lbl, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->folder_btn, 1, 0, 2, 1);

    GtkWidget *name_lbl = gtk_label_new("Recording name");
    gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);
    app->name_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->name_entry),
                                   "Name this take before you record");
    gtk_widget_set_hexpand(app->name_entry, TRUE);
    GtkWidget *ext = gtk_label_new(".wav");
    gtk_grid_attach(GTK_GRID(grid), name_lbl, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->name_entry, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ext, 2, 1, 1, 1);

    load_config(app);
    g_autofree char *stamp = suggested_basename();
    gtk_entry_set_text(GTK_ENTRY(app->name_entry), stamp);

    app->level = gtk_level_bar_new();
    gtk_level_bar_set_min_value(GTK_LEVEL_BAR(app->level), 0.0);
    gtk_level_bar_set_max_value(GTK_LEVEL_BAR(app->level), 1.0);
    gtk_box_pack_start(GTK_BOX(box), app->level, FALSE, FALSE, 0);

    app->record_btn = gtk_button_new_with_label("Record");
    gtk_style_context_add_class(gtk_widget_get_style_context(app->record_btn),
                                "suggested-action");
    gtk_widget_set_size_request(app->record_btn, -1, 40);
    gtk_widget_set_can_default(app->record_btn, TRUE);
    gtk_window_set_default(GTK_WINDOW(app->window), app->record_btn);
    gtk_entry_set_activates_default(GTK_ENTRY(app->name_entry), TRUE);
    g_signal_connect(app->record_btn, "clicked", G_CALLBACK(on_record), app);
    gtk_box_pack_start(GTK_BOX(box), app->record_btn, FALSE, FALSE, 0);

    const char *ready = ffmpeg_present()
        ? "Ready. Name the take, then hit Record. Folder is remembered."
        : "ffmpeg not found on PATH — install it or fix PATH before recording.";
    app->status = gtk_label_new(ready);
    gtk_label_set_xalign(GTK_LABEL(app->status), 0.f);
    gtk_label_set_line_wrap(GTK_LABEL(app->status), TRUE);
    gtk_box_pack_start(GTK_BOX(box), app->status, FALSE, FALSE, 0);

    reload_sources(app, FALSE);
    gtk_widget_show_all(app->window);
}

int main(int argc, char **argv)
{
    GtkApplication *ga = gtk_application_new("za.delarey.headsetrecorder",
                                             G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(ga, "activate", G_CALLBACK(activate), NULL);
    int st = g_application_run(G_APPLICATION(ga), argc, argv);
    g_object_unref(ga);
    return st;
}
