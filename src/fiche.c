/*
Fiche - Command line pastebin for sharing terminal output.

-------------------------------------------------------------------------------

License: MIT (http://www.opensource.org/licenses/mit-license.php)
Repository: https://github.com/solusipse/fiche/
Live example: http://termbin.com

-------------------------------------------------------------------------------

usage: fiche [-DepbsdolBuw].
             [-D] [-e] [-d domain] [-p port] [-s slug size]
             [-o output directory] [-B buffer size] [-u user name]
             [-l log file] [-b banlist] [-w whitelist]
-D option is for daemonizing fiche
-e option is for using an extended character set for the URL

Compile with Makefile or manually with -O2 and -pthread flags.

To install use `make install` command.

Use netcat to push text - example:
$ cat fiche.c | nc localhost 9999

-------------------------------------------------------------------------------
*/

#include "fiche.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#include <pwd.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include <fcntl.h>
#include <netdb.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/in.h>


/******************************************************************************
 * Various declarations
 */
const char *Fiche_Symbols = "abcdefghijklmnopqrstuvwxyz0123456789";

/* Initial chunk size for the dynamic receive buffer */
#define FICHE_CHUNK (65536U)

/* Delete token */
#define TOKEN_LEN 32
#define TOKEN_SYMBOLS "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"


/******************************************************************************
 * Inner structs
 */

struct fiche_connection {
    int socket;
    struct sockaddr_in address;

    Fiche_Settings *settings;
};


/******************************************************************************
 * Static function declarations
 */

// Settings-related

/**
 * @brief Sets domain name
 * @warning settings.domain has to be freed after using this function!
 */
static int set_domain_name(Fiche_Settings *settings);

/**
 * @brief Changes user running this program to requested one
 * @warning Application has to be run as root to use this function
 */
static int perform_user_change(const Fiche_Settings *settings);


// Server-related

/**
 * @brief Starts server with settings provided in Fiche_Settings struct
 */
static int start_server(Fiche_Settings *settings);

/**
 * @brief Dispatches incoming connections by spawning threads
 */
static void dispatch_connection(int socket, Fiche_Settings *settings);

/**
 * @brief Handles connections
 * @remarks Is being run by dispatch_connection in separate threads
 * @arg args Struct fiche_connection containing connection details
 */
static void *handle_connection(void *args);

// Server-related utils


/**
 * @brief Generates a slug that will be used for paste creation
 * @warning output has to be freed after using!
 *
 * @arg output pointer to output string containing full path to directory
 * @arg length default or user-requested length of a slug
 * @arg extra_length additional length that was added to speed-up the
 *      generation process
 *
 * This function is used in connection with create_directory function
 * It generates strings that are used to create a directory for
 * user-provided data. If directory already exists, we ask this function
 * to generate another slug with increased size.
 */
static void generate_slug(char **output, uint8_t length, uint8_t extra_length);


/**
 * @brief Creates a directory at requested path using requested slug
 * @returns 0 if succeded, 1 if failed or dir already existed
 *
 * @arg output_dir root directory for all pastes
 * @arg slug directory name for a particular paste
 */
static int create_directory(char *output_dir, char *slug);


/**
 * @brief Saves data to file at requested path
 *
 * @arg data Buffer with data received from the user
 * @arg path Path at which file containing data from the buffer will be created
 */
static int save_to_file(const Fiche_Settings *s, uint8_t *data, size_t data_len, char *slug);


// Logging-related

/**
 * @brief Displays error messages
 */
static void print_error(const char *format, ...);


/**
 * @brief Displays status messages
 */
static void print_status(const char *format, ...);


/**
 * @brief Displays horizontal line
 */
static void print_separator();


/**
 * @brief Saves connection entry to the logfile
 */
static void log_entry(const Fiche_Settings *s, const char *ip,
        const char *hostname, const char *slug);


/**
 * @brief Returns string containing current date
 * @warning Output has to be freed!
 */
static void get_date(char *buf);


/**
 * @brief Generates a random delete token
 */
static void generate_token(char *out);

/**
 * @brief Saves delete token to .token file in slug directory
 */
static int save_token(const Fiche_Settings *s, const char *token, const char *slug);

/**
 * @brief Handles a delete connection
 */
static void *handle_delete(void *args);

/**
 * @brief Runs the delete service on a separate thread
 */
static void *run_delete_server(void *arg);

/**
 * @brief Time seed
 */
unsigned int seed;

/******************************************************************************
 * Public fiche functions
 */

void fiche_init(Fiche_Settings *settings) {

    // Initialize everything to default values
    // or to NULL in case of pointers

    struct Fiche_Settings def = {
        // domain
        "example.com",
        // output dir
        "code",
	// listen_addr
	"0.0.0.0",
        // port
        9999,
        // slug length
        4,
        // https
        false,
        // buffer length
        32768,
        // user name
        NULL,
        // path to log file
        NULL,
        // path to banlist
        NULL,
        // path to whitelist
        NULL,
        // delete port
        0
    };

    // Copy default settings to provided instance
    *settings = def;
}

int fiche_run(Fiche_Settings settings) {

    seed = time(NULL);

    // Display welcome message
    {
        char date[64];
        get_date(date);
        print_status("Starting fiche on %s...", date);
    }

    // Try to set requested user
    if ( perform_user_change(&settings) != 0) {
        print_error("Was not able to change the user!");
        return -1;
    }

    // Check if output directory is writable
    // - First we try to create it
    {
        mkdir(
            settings.output_dir_path,
            S_IRWXU | S_IRGRP | S_IROTH | S_IXOTH | S_IXGRP
        );
        // - Then we check if we can write there
        if ( access(settings.output_dir_path, W_OK) != 0 ) {
            print_error("Output directory not writable!");
            return -1;
        }
    }

    // Check if log file is writable (if set)
    if ( settings.log_file_path ) {

        // Create log file if it doesn't exist
        FILE *f = fopen(settings.log_file_path, "a+");
        fclose(f);

        // Then check if it's accessible
        if ( access(settings.log_file_path, W_OK) != 0 ) {
            print_error("Log file not writable!");
            return -1;
        }

    }

    // Try to set domain name
    if ( set_domain_name(&settings) != 0 ) {
        print_error("Was not able to set domain name!");
        return -1;
    }

    // Start delete service on a separate thread if port is configured
    if (settings.delete_port > 0) {
        Fiche_Settings *sp = malloc(sizeof(*sp));
        if (!sp) {
            print_error("Couldn't allocate delete service settings!");
            return -1;
        }
        *sp = settings;
        pthread_t del_tid;
        if (pthread_create(&del_tid, NULL, run_delete_server, sp) != 0) {
            print_error("Couldn't start delete server!");
            free(sp);
        } else {
            pthread_detach(del_tid);
            print_status("Delete service listening on: %s:%d.",
                         settings.listen_addr, settings.delete_port);
            print_separator();
        }
    }

    // Main loop in this method
    start_server(&settings);

    // Perform final cleanup

    // This is allways allocated on the heap
    free(settings.domain);

    return 0;

}


/******************************************************************************
 * Static functions below
 */

static void print_error(const char *format, ...) {
    va_list args;
    va_start(args, format);

    printf("[Fiche][ERROR] ");
    vprintf(format, args);
    printf("\n");

    va_end(args);
}


static void print_status(const char *format, ...) {
    va_list args;
    va_start(args, format);

    printf("[Fiche][STATUS] ");
    vprintf(format, args);
    printf("\n");

    va_end(args);
}


static void print_separator() {
    printf("============================================================\n");
}


static void log_entry(const Fiche_Settings *s, const char *ip,
    const char *hostname, const char *slug)
{
    // Logging to file not enabled, finish here
    if (!s->log_file_path) {
        return;
    }

    FILE *f = fopen(s->log_file_path, "a");
    if (!f) {
        print_status("Was not able to save entry to the log!");
        return;
    }

    char date[64];
    get_date(date);

    // Write entry to file
    fprintf(f, "%s -- %s -- %s (%s)\n", slug, date, ip, hostname);
    fclose(f);
}


static void get_date(char *buf) {
    struct tm curtime;
    time_t ltime;

    ltime=time(&ltime);
    localtime_r(&ltime, &curtime);

    // Save data to provided buffer
    if (asctime_r(&curtime, buf) == 0) {
        // Couldn't get date, setting first byte of the
        // buffer to zero so it won't be displayed
        buf[0] = 0;
        return;
    }

    // Remove newline char
    buf[strlen(buf)-1] = 0;
}


static int set_domain_name(Fiche_Settings *settings) {

    char *prefix = "";
    if (settings->https) {
        prefix = "https://";
    } else {
        prefix = "http://";
    }
    const int len = strlen(settings->domain) + strlen(prefix) + 1;

    char *b = malloc(len);
    if (!b) {
        return -1;
    }

    strcpy(b, prefix);
    strcat(b, settings->domain);

    settings->domain = b;

    print_status("Domain set to: %s.", settings->domain);

    return 0;
}


static int perform_user_change(const Fiche_Settings *settings) {

    // User change wasn't requested, finish here
    if (settings->user_name == NULL) {
        return 0;
    }

    // Check if root, if not - finish here
    if (getuid() != 0) {
        print_error("Run as root if you want to change the user!");
        return -1;
    }

    // Get user details
    const struct passwd *userdata = getpwnam(settings->user_name);

    const int uid = userdata->pw_uid;
    const int gid = userdata->pw_gid;

    if (uid == -1 || gid == -1) {
        print_error("Could find requested user: %s!", settings->user_name);
        return -1;
    }

    if (setgid(gid) != 0) {
        print_error("Couldn't switch to requested user: %s!", settings->user_name);
    }

    if (setuid(uid) != 0) {
        print_error("Couldn't switch to requested user: %s!", settings->user_name);
    }

    print_status("User changed to: %s.", settings->user_name);

    return 0;
}


static int start_server(Fiche_Settings *settings) {

    // Perform socket creation
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        print_error("Couldn't create a socket!");
        return -1;
    }

    // Set socket settings
    if ( setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 } , sizeof(int)) != 0 ) {
        print_error("Couldn't prepare the socket!");
        return -1;
    }

    // Prepare address and port handler
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(settings->listen_addr);
    address.sin_port = htons(settings->port);

    // Bind to port
    if ( bind(s, (struct sockaddr *) &address, sizeof(address)) != 0) {
        print_error("Couldn't bind to the port: %d!", settings->port);
        return -1;
    }

    // Start listening
    if ( listen(s, 128) != 0 ) {
        print_error("Couldn't start listening on the socket!");
        return -1;
    }

    print_status("Server started listening on: %s:%d.",
		    settings->listen_addr, settings->port);
    print_separator();

    // Run dispatching loop
    while (1) {
        dispatch_connection(s, settings);
    }

    // Give some time for all threads to finish
    // NOTE: this code is reached only in testing environment
    // There is currently no way to kill the main thread from any thread
    // Something like this can be done for testing purpouses:
    // int i = 0;
    // while (i < 3) {
    //     dispatch_connection(s, settings);
    //     i++;
    // }

    sleep(5);

    return 0;
}


static void dispatch_connection(int socket, Fiche_Settings *settings) {

    // Create address structs for this socket
    struct sockaddr_in address;
    socklen_t addlen = sizeof(address);

    // Accept a connection and get a new socket id
    const int s = accept(socket, (struct sockaddr *) &address, &addlen);
    if (s < 0 ) {
        print_error("Error on accepting connection!");
        return;
    }

    // Set timeout for accepted socket
    const struct timeval timeout = { 5, 0 };

    if ( setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ) {
        print_error("Couldn't set a timeout!");
    }

    if ( setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ) {
        print_error("Couldn't set a timeout!");
    }

    // Create an argument for the thread function
    struct fiche_connection *c = malloc(sizeof(*c));
    if (!c) {
        print_error("Couldn't allocate memory!");
        return;
    }
    c->socket = s;
    c->address = address;
    c->settings = settings;

    // Spawn a new thread to handle this connection
    pthread_t id;

    if ( pthread_create(&id, NULL, &handle_connection, c) != 0 ) {
        print_error("Couldn't spawn a thread!");
        return;
    }

    // Detach thread if created succesfully
    // TODO: consider using pthread_tryjoin_np
    pthread_detach(id);

}


static void *handle_connection(void *args) {

    // Cast args to it's previous type
    struct fiche_connection *c = (struct fiche_connection *) args;

    // Get client's IP
    const char *ip = inet_ntoa(c->address.sin_addr);

    // Get client's hostname
    char hostname[1024];

    if (getnameinfo((struct sockaddr *)&c->address, sizeof(c->address),
            hostname, sizeof(hostname), NULL, 0, 0) != 0 ) {

        // Couldn't resolve a hostname
        strcpy(hostname, "n/a");
    }

    // Print status on this connection
    {
        char date[64];
        get_date(date);
        print_status("%s", date);

        print_status("Incoming connection from: %s (%s).", ip, hostname);
    }

    // Allocate an initial receive buffer; grow dynamically up to buffer_len.
    // This avoids reserving the full -B allocation (e.g. 10 MB) for every
    // connection regardless of actual upload size.
    size_t buf_size = (FICHE_CHUNK < c->settings->buffer_len)
                      ? FICHE_CHUNK : c->settings->buffer_len;

    uint8_t *buffer = malloc(buf_size);
    if (!buffer) {
        print_error("Couldn't allocate buffer!");
        print_separator();
        close(c->socket);
        free(c);
        pthread_exit(NULL);
        return NULL;
    }

    // Read in a loop, growing the buffer as needed up to buffer_len.
    ssize_t r = 0, chunk;
    while (1) {
        // Buffer full: try to grow before the next read
        if ((size_t)r == buf_size) {
            size_t new_size = buf_size * 2;
            if (new_size > c->settings->buffer_len)
                new_size = c->settings->buffer_len;

            if (new_size == buf_size) {
                // Hard limit reached — stop reading
                print_error("Upload size limit reached (%u bytes).",
                            c->settings->buffer_len);
                break;
            }

            uint8_t *tmp = realloc(buffer, new_size);
            if (!tmp) {
                print_error("Couldn't grow receive buffer!");
                free(buffer);
                close(c->socket);
                free(c);
                pthread_exit(NULL);
                return NULL;
            }
            buffer  = tmp;
            buf_size = new_size;
        }

        chunk = recv(c->socket, buffer + r, buf_size - (size_t)r, 0);
        if (chunk <= 0) break;
        r += chunk;
    }

    if (r <= 0) {
        print_error("No data received from the client!");
        print_separator();

        // Close the socket
        close(c->socket);

        // Cleanup
        free(buffer);
        free(c);
        pthread_exit(NULL);

        return NULL;
    }

    // - Check if request was performed with a known protocol
    // TODO

    // - Check if on whitelist
    // TODO

    // - Check if on banlist
    // TODO

    // Generate slug and use it to create an url
    char *slug;
    uint8_t extra = 0;

    do {

        // Generate slugs until it's possible to create a directory
        // with generated slug on disk
        generate_slug(&slug, c->settings->slug_len, extra);

        // Something went wrong in slug generation, break here
        if (!slug) {
            break;
        }

        // Increment counter for additional letters needed
        ++extra;

        // If i was incremented more than 128 times, something
        // for sure went wrong. We are closing connection and
        // killing this thread in such case
        if (extra > 128) {
            print_error("Couldn't generate a valid slug!");
            print_separator();

            // Cleanup
            free(buffer);
            free(slug);
            close(c->socket);
            free(c);
            pthread_exit(NULL);
            return NULL;
        }

    }
    while(create_directory(c->settings->output_dir_path, slug) != 0);


    // Slug generation failed, we have to finish here
    if (!slug) {
        print_error("Couldn't generate a slug!");
        print_separator();

        close(c->socket);

        // Cleanup
        free(buffer);
        free(c);
        pthread_exit(NULL);
        return NULL;
    }


    // Save to file failed, we have to finish here
    if ( save_to_file(c->settings, buffer, (size_t)r, slug) != 0 ) {
        print_error("Couldn't save a file!");
        print_separator();

        close(c->socket);

        // Cleanup
        free(buffer);
        free(c);
        free(slug);
        pthread_exit(NULL);
        return NULL;
    }

    // Generate and save delete token (best-effort; non-fatal if it fails)
    char token[TOKEN_LEN + 1];
    int has_token = 0;
    if (c->settings->delete_port > 0) {
        generate_token(token);
        has_token = (save_token(c->settings, token, slug) == 0);
        if (!has_token) {
            print_error("Couldn't save delete token for: %s.", slug);
        }
    }

    // Write a response to the user
    {
        const char *domain = c->settings->domain;

        if (has_token) {
            // URL + delete instructions
            // Format:
            //   https://domain.com/slug\n
            //   \n
            //   To delete this paste:\n
            //   echo -e "slug\\ntoken" | nc domain.com PORT\n
            char host[256];
            // Extract bare hostname from domain (strip scheme if present)
            const char *h = strstr(domain, "://");
            h = h ? h + 3 : domain;
            snprintf(host, sizeof(host), "%s", h);
            // Strip trailing slash if any
            size_t hlen = strlen(host);
            if (hlen > 0 && host[hlen - 1] == '/') host[hlen - 1] = '\0';

            size_t len = strlen(domain) + strlen(slug)
                         + strlen(host) + strlen(slug) + strlen(token)
                         + 64;
            char *resp = malloc(len);
            if (resp) {
                snprintf(resp, len,
                    "%s/%s\n"
                    "\n"
                    "To delete this paste:\n"
                    "echo -e \"%s\\\\n%s\" | nc %s %d\n",
                    domain, slug,
                    slug, token, host, c->settings->delete_port);
                write(c->socket, resp, strlen(resp));
                free(resp);
            }
        } else {
            // Plain URL only
            const size_t len = strlen(domain) + strlen(slug) + 3;
            char url[len];
            snprintf(url, len, "%s/%s\n", domain, slug);
            write(c->socket, url, len);
        }
    }

    print_status("Received %zd bytes, saved to: %s.", r, slug);
    print_separator();

    // Log connection
    // TODO: log unsuccessful and rejected connections
    log_entry(c->settings, ip, hostname, slug);

    // Close the connection
    close(c->socket);

    // Perform cleanup of values used in this thread
    free(buffer);
    free(slug);
    free(c);

    // Return freed memory to the OS immediately.
    // By default glibc holds freed heap memory in its arena; for a server
    // that may allocate large buffers per connection this prevents the
    // process RSS from growing unboundedly.
    malloc_trim(0);

    pthread_exit(NULL);

    return NULL;
}


/******************************************************************************
 * Token helpers
 */

static void generate_token(char *out) {
    static const char syms[] = TOKEN_SYMBOLS;
    const size_t nsyms = sizeof(syms) - 1;
    for (int i = 0; i < TOKEN_LEN; i++) {
        out[i] = syms[rand_r(&seed) % nsyms];
    }
    out[TOKEN_LEN] = '\0';
}

static int save_token(const Fiche_Settings *s, const char *token, const char *slug) {
    // Path: <output_dir>/<slug>/.token
    size_t len = strlen(s->output_dir_path) + strlen(slug) + 9; // /.token + null
    char *path = malloc(len);
    if (!path) return -1;
    snprintf(path, len, "%s/%s/.token", s->output_dir_path, slug);

    FILE *f = fopen(path, "w");
    free(path);
    if (!f) return -1;
    fprintf(f, "%s", token);
    fclose(f);
    return 0;
}


/******************************************************************************
 * Delete service
 * Listens on delete_port.
 * Client sends: <slug>\n<token>\n
 * Server verifies token, deletes paste directory if match.
 */

static void *handle_delete(void *args) {
    struct fiche_connection *c = (struct fiche_connection *)args;

    // Read slug\ntoken\n in one shot — client sends both lines at once
    char buf[128];
    memset(buf, 0, sizeof(buf));
    ssize_t n = recv(c->socket, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        close(c->socket); free(c); pthread_exit(NULL); return NULL;
    }

    // Split on first \n to get slug and token
    char slug[65];
    char token_in[TOKEN_LEN + 4];
    memset(slug, 0, sizeof(slug));
    memset(token_in, 0, sizeof(token_in));

    char *nl = memchr(buf, '\n', (size_t)n);
    if (!nl) {
        write(c->socket, "invalid request\n", 16);
        close(c->socket); free(c); pthread_exit(NULL); return NULL;
    }

    size_t slug_len = (size_t)(nl - buf);
    if (slug_len == 0 || slug_len >= sizeof(slug)) {
        write(c->socket, "invalid slug\n", 13);
        close(c->socket); free(c); pthread_exit(NULL); return NULL;
    }
    memcpy(slug, buf, slug_len);
    // Strip \r if present
    if (slug[slug_len - 1] == '\r') slug[slug_len - 1] = '\0';

    char *token_start = nl + 1;
    size_t token_raw_len = (size_t)(buf + n - token_start);
    if (token_raw_len == 0 || token_raw_len >= sizeof(token_in)) {
        write(c->socket, "invalid token\n", 14);
        close(c->socket); free(c); pthread_exit(NULL); return NULL;
    }
    memcpy(token_in, token_start, token_raw_len);
    // Strip trailing \n and \r
    for (int i = 0; token_in[i]; i++) {
        if (token_in[i] == '\n' || token_in[i] == '\r') { token_in[i] = '\0'; break; }
    }

    // Validate slug: only alphanumeric, no path traversal
    for (int i = 0; slug[i]; i++) {
        char ch = slug[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) {
            write(c->socket, "invalid slug\n", 13);
            close(c->socket); free(c); pthread_exit(NULL); return NULL;
        }
    }

    // Build path to .token file
    size_t plen = strlen(c->settings->output_dir_path) + strlen(slug) + 9;
    char *token_path = malloc(plen);
    if (!token_path) {
        close(c->socket); free(c); pthread_exit(NULL); return NULL;
    }
    snprintf(token_path, plen, "%s/%s/.token", c->settings->output_dir_path, slug);

    // Read stored token
    FILE *f = fopen(token_path, "r");
    free(token_path);
    if (!f) {
        write(c->socket, "not found\n", 10);
        close(c->socket); free(c); pthread_exit(NULL); return NULL;
    }
    char token_stored[TOKEN_LEN + 4];
    memset(token_stored, 0, sizeof(token_stored));
    fgets(token_stored, sizeof(token_stored), f);
    fclose(f);
    // Strip trailing \n and \r left by fgets
    for (int i = 0; token_stored[i]; i++) {
        if (token_stored[i] == '\n' || token_stored[i] == '\r') {
            token_stored[i] = '\0'; break;
        }
    }

    // Constant-time comparison to avoid timing attacks
    int match = 1;
    if (strlen(token_in) != strlen(token_stored)) {
        match = 0;
    } else {
        for (size_t i = 0; i < strlen(token_stored); i++) {
            if (token_in[i] != token_stored[i]) match = 0;
        }
    }

    if (!match) {
        write(c->socket, "unauthorized\n", 13);
        close(c->socket); free(c); pthread_exit(NULL); return NULL;
    }

    // Delete paste directory: remove index.txt, .token, then the directory
    size_t dlen = strlen(c->settings->output_dir_path) + strlen(slug) + 2;
    char *dir_path = malloc(dlen + 12); // extra for /index.txt
    if (!dir_path) {
        close(c->socket); free(c); pthread_exit(NULL); return NULL;
    }

    snprintf(dir_path, dlen + 12, "%s/%s/index.txt", c->settings->output_dir_path, slug);
    unlink(dir_path);

    snprintf(dir_path, dlen + 12, "%s/%s/.token", c->settings->output_dir_path, slug);
    unlink(dir_path);

    snprintf(dir_path, dlen, "%s/%s", c->settings->output_dir_path, slug);
    if (rmdir(dir_path) == 0) {
        write(c->socket, "deleted\n", 8);
        print_status("Paste deleted: %s.", slug);
    } else {
        write(c->socket, "error\n", 6);
    }
    free(dir_path);

    close(c->socket);
    free(c);
    pthread_exit(NULL);
    return NULL;
}


static void *run_delete_server(void *arg) {
    Fiche_Settings *settings = (Fiche_Settings *)arg;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        print_error("Delete: couldn't create socket!");
        free(settings); pthread_exit(NULL); return NULL;
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));

    struct sockaddr_in addr;
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr(settings->listen_addr);
    addr.sin_port        = htons(settings->delete_port);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        print_error("Delete: couldn't bind to port %d!", settings->delete_port);
        close(s); free(settings); pthread_exit(NULL); return NULL;
    }
    if (listen(s, 128) != 0) {
        print_error("Delete: couldn't listen!");
        close(s); free(settings); pthread_exit(NULL); return NULL;
    }

    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cs = accept(s, (struct sockaddr *)&caddr, &clen);
        if (cs < 0) continue;

        const struct timeval timeout = {5, 0};
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        struct fiche_connection *conn = malloc(sizeof(*conn));
        if (!conn) { close(cs); continue; }
        conn->socket   = cs;
        conn->address  = caddr;
        conn->settings = settings;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_delete, conn) != 0) {
            close(cs); free(conn);
        } else {
            pthread_detach(tid);
        }
    }

    close(s);
    free(settings);
    pthread_exit(NULL);
    return NULL;
}


static void generate_slug(char **output, uint8_t length, uint8_t extra_length) {

    // Realloc buffer for slug when we want it to be bigger
    // This happens in case when directory with this name already
    // exists. To save time, we don't generate new slugs until
    // we spot an available one. We add another letter instead.

    if (extra_length > 0) {
        free(*output);
    }

    // Create a buffer for slug with extra_length if any
    *output = calloc(length + 1 + extra_length, sizeof(char));

    if (*output == NULL) {
        return;
    }

    // Take n-th symbol from symbol table and use it for slug generation
    for (int i = 0; i < length + extra_length; i++) {
        int n = rand_r(&seed) % strlen(Fiche_Symbols);
        *(output[0] + sizeof(char) * i) = Fiche_Symbols[n];
    }

}


static int create_directory(char *output_dir, char *slug) {
    if (!slug) {
        return -1;
    }

    // Additional byte is for the slash
    size_t len = strlen(output_dir) + strlen(slug) + 2;

    // Generate a path
    char *path = malloc(len);
    if (!path) {
        return -1;
    }
    snprintf(path, len, "%s%s%s", output_dir, "/", slug);

    // Create output directory, just in case
    mkdir(output_dir, S_IRWXU | S_IRGRP | S_IROTH | S_IXOTH | S_IXGRP);

    // Create slug directory
    const int r = mkdir(
        path,
        S_IRWXU | S_IRGRP | S_IROTH | S_IXOTH | S_IXGRP
    );

    free(path);

    return r;
}


static int save_to_file(const Fiche_Settings *s, uint8_t *data, size_t data_len, char *slug) {
    char *file_name = "index.txt";

    // Additional 2 bytes are for 2 slashes
    size_t len = strlen(s->output_dir_path) + strlen(slug) + strlen(file_name) + 3;

    // Generate a path
    char *path = malloc(len);
    if (!path) {
        return -1;
    }

    snprintf(path, len, "%s%s%s%s%s", s->output_dir_path, "/", slug, "/", file_name);

    // Attempt file saving
    FILE *f = fopen(path, "w");
    if (!f) {
        free(path);
        return -1;
    }

    // Write exactly data_len bytes; fwrite handles binary data and null bytes
    // correctly, unlike fprintf("%s") which would stop at the first null byte.
    if ( fwrite(data, 1, data_len, f) != data_len ) {
        fclose(f);
        free(path);
        return -1;
    }

    fclose(f);
    free(path);

    return 0;
}