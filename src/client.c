#include "snake.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <stdarg.h>
#include <signal.h>

#define FRAME_BUF_SIZE 65536

typedef struct {
    struct termios orig;
    int orig_flags;
    int active;
} TermGuard;

typedef struct {
    int sock;
    GameState game_state;
    int player_id;
    int in_game;
    char world[WORLD_HEIGHT][WORLD_WIDTH];
    int server_pid;
} client_ctx_t;

static void clear_screen(void) {
    // HOME + clear from cursor to end of screen (fix ghosting / flicker)
    printf("\033[H\033[J");
}

static int connect_to_server(client_ctx_t *C) {
    C->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (C->sock < 0) {
        printf("Chyba: Nepodarilo sa vytvoriť socket\n");
        return 0;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_aton("127.0.0.1", &addr.sin_addr);

    if (connect(C->sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
          printf("Chyba: Nepodarilo sa pripojiť k serveru na localhost:%d\n", PORT);
        close(C->sock);
        C->sock = -1;
        return 0;
    }

    printf("[KLIENT] Pripojené k serveru!\n");
    return 1;
}

static void send_message(client_ctx_t *C, const char* msg) {
    if (!C || C->sock < 0) return;
    send(C->sock, msg, strlen(msg), 0);
}

static void clear_world(client_ctx_t *C) {
    for (int y = 0; y < WORLD_HEIGHT; y++)
        for (int x = 0; x < WORLD_WIDTH; x++)
            C->world[y][x] = ' ';
}

static int appendf(char *dst, int cap, int off, const char *fmt, ...) {
    if (off >= cap) return cap;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst + off, cap - off, fmt, ap);
    va_end(ap);
    if (n < 0) return off;
    if (off + n >= cap) return cap;
    return off + n;
}

static int line_end(int off, char *dst, int cap) {
    return appendf(dst, cap, off, "\033[K\n");
}

static int render_players_info_to_buf(client_ctx_t *C, char *out, int cap, int off) {
    off = appendf(out, cap, off, "Max hráčov: %d", MAX_CLIENTS);
    off = line_end(off, out, cap);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (i < C->game_state.num_players) {
            Player *p = &C->game_state.players[i];

            if (!p->alive) {
                off = appendf(out, cap, off, "Hráč %s je mŕtvy. Skóre: %d", p->name, p->score);
                off = line_end(off, out, cap);
            } else {
                off = appendf(out, cap, off, "Meno: %s", p->name); off = line_end(off, out, cap);
                off = appendf(out, cap, off, "ID: %d", p->id);     off = line_end(off, out, cap);
                off = appendf(out, cap, off, "Skóre: %d", p->score);off = line_end(off, out, cap);
            }
        } else {
            off = appendf(out, cap, off, "______________________________________");
            off = line_end(off, out, cap);
        }

        off = line_end(off, out, cap);
    }

    if (C->player_id >= 0 && C->player_id < C->game_state.num_players) {
        Player *me = &C->game_state.players[C->player_id];
        off = appendf(out, cap, off, "== TY ==");
        off = line_end(off, out, cap);
        off = appendf(out, cap, off, "Meno: %s | ID: %d | Skóre: %d",
                      me->name, me->id, me->score);
        off = line_end(off, out, cap);
        off = line_end(off, out, cap);
    }

    return off;
}

static int render_world_to_buf(client_ctx_t *C, char *out, int cap, int off) {
    off = appendf(out, cap, off, "\n╔");
    for (int x = 0; x < WORLD_WIDTH; x++) off = appendf(out, cap, off, "═");
    off = appendf(out, cap, off, "╗");
    off = line_end(off, out, cap);

    for (int y = 0; y < WORLD_HEIGHT; y++) {
        off = appendf(out, cap, off, "║");
        for (int x = 0; x < WORLD_WIDTH; x++) {
            char c = C->world[y][x];
            if (c == '.') c = ' ';
            off = appendf(out, cap, off, "%c", c);
        }
        off = appendf(out, cap, off, "║");
        off = line_end(off, out, cap);
    }

    off = appendf(out, cap, off, "╚");
    for (int x = 0; x < WORLD_WIDTH; x++) off = appendf(out, cap, off, "═");
    off = appendf(out, cap, off, "╝");
    off = line_end(off, out, cap);

    return off;
}

static const char* skip_next_field(const char* p) {
    const char* q = strchr(p, '|');
    return q ? (q + 1) : NULL;
}

static void parse_game_state(client_ctx_t *C, const char* buffer, int *out_got_state) {
    if (strncmp(buffer, "STATE", 5) != 0) return;

    int parts[12];
    if (sscanf(buffer, "STATE|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|",
               &parts[0], &parts[1], &parts[2], &parts[3], &parts[4], &parts[5],
               &parts[6], &parts[7], &parts[8], &parts[9], &parts[10], &parts[11]) != 12) {
        return;
    }

    C->game_state.id = parts[0];
    C->game_state.width = parts[1];
    C->game_state.height = parts[2];
    C->game_state.num_players = parts[3];
    C->game_state.fruit_x = parts[4];
    C->game_state.fruit_y = parts[5];
    C->game_state.active = parts[6];
    C->game_state.game_over = parts[7];
    C->game_state.num_obstacles = parts[8];
    C->game_state.mode = parts[9];
    C->game_state.world_type = parts[10];
    C->game_state.elapsed_time = parts[11];

    const char* ptr = strchr(buffer, '|');
    for (int i = 0; i < 12 && ptr; i++) ptr = strchr(ptr + 1, '|');
    if (!ptr) return;
    ptr++;

    clear_world(C);

    if (ptr && strncmp(ptr, "M|", 2) == 0) {
        ptr += 2;
        const char* end = strchr(ptr, '|');
        if (!end) return;

        int len = (int)(end - ptr);
        int expected = C->game_state.width * C->game_state.height;

        if (len >= expected) {
            for (int y = 0; y < C->game_state.height && y < WORLD_HEIGHT; y++) {
                for (int x = 0; x < C->game_state.width && x < WORLD_WIDTH; x++) {
                    char c = ptr[y * C->game_state.width + x];
                    if (c == '.') c = ' ';
                    C->world[y][x] = c;
                }
            }
        }

        ptr = end + 1;
    }

    int obs_count = 0;
    while (ptr && strncmp(ptr, "O|", 2) == 0 && obs_count < MAX_OBSTACLES) {
        int ox, oy;
        if (sscanf(ptr, "O|%d|%d|", &ox, &oy) == 2) {
            C->game_state.obstacles[obs_count][0] = ox;
            C->game_state.obstacles[obs_count][1] = oy;
            obs_count++;
        }

        ptr = skip_next_field(ptr);
        ptr = ptr ? skip_next_field(ptr) : NULL;
        ptr = ptr ? skip_next_field(ptr) : NULL;
    }
    C->game_state.num_obstacles = obs_count;

    int pl_count = 0;
    while (ptr && strncmp(ptr, "P|", 2) == 0 && pl_count < 10) {
        Player* p = &C->game_state.players[pl_count];
        int id, alive, score, hx, hy, blen, dir;

        if (sscanf(ptr, "P|%d|%49[^|]|%d|%d|%d|%d|%d|%d|",
                   &id, p->name, &alive, &score, &hx, &hy, &blen, &dir) == 8) {
            p->id = id;
            p->alive = alive;
            p->score = score;
            p->head_x = hx;
            p->head_y = hy;
            p->body_len = blen;
            p->direction = (Direction)dir;
            pl_count++;
        }

        ptr = skip_next_field(ptr);
        for (int k = 0; k < 8 && ptr; k++) ptr = skip_next_field(ptr);
    }
    C->game_state.num_players = pl_count;

    if (out_got_state) *out_got_state = 1;
}

static void receive_game_state(client_ctx_t *C, int *out_got_state) {
    if (!C || C->sock < 0) return;

    static char acc[65536];   // veľký buffer
    static int acc_len = 0;

    char tmp[BUFFER_SIZE];
    int n;

    while ((n = recv(C->sock, tmp, (int)sizeof(tmp) - 1, MSG_DONTWAIT)) > 0) {
        tmp[n] = '\0';

        if (acc_len + n >= (int)sizeof(acc) - 1) {
            // keď pretečie, zahodíme staré (radšej prísť o frame než sa rozbiť)
            acc_len = 0;
        }

        memcpy(acc + acc_len, tmp, (size_t)n);
        acc_len += n;
        acc[acc_len] = '\0';
    }

    // nič nové
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        // socket error
        return;
    }

    char *line_start = acc;
    while (1) {
        char *nl = strchr(line_start, '\n');
        if (!nl) break;

        *nl = '\0';

        if (strncmp(line_start, "ASSIGN|", 7) == 0) {
            int id;
            if (sscanf(line_start, "ASSIGN|%d|", &id) == 1) {
                C->player_id = id;
            }
        } else if (strncmp(line_start, "STATE", 5) == 0) {
            // DEBUG: ukáž, či vôbec prišiel M|
            // fprintf(stderr, "STATE len=%zu hasM=%d\n", strlen(line_start),
            //         (strstr(line_start, "M|") != NULL));
            parse_game_state(C, line_start, out_got_state);
        }

        line_start = nl + 1;
    }

    int remaining = (int)strlen(line_start);
    memmove(acc, line_start, (size_t)remaining);
    acc_len = remaining;
    acc[acc_len] = '\0';
}

static void show_main_menu(void) {
    printf("\n");
    printf("╔════════════════════════════════════════╗\n");
    printf("║         HADIK - HLAVNE MENU            ║\n");
    printf("╚════════════════════════════════════════╝\n\n");
    printf("1. Nová hra (vytvor server)\n");
    printf("2. Pripojiť sa k existujúcej hre\n");
    printf("3. Koniec\n\n");
    printf("Vybrať možnosť: ");
}

static void create_new_game(client_ctx_t *C) {
    printf("\n╔════════════════════════════════════════╗\n");
    printf("║            NOVA HRA                    ║\n");
    printf("╚════════════════════════════════════════╝\n\n");
    printf("Vyber herný režim:\n");
    printf("1. Štandardný (hra pokračuje pokiaľ je aspoň 1 hráč)\n");
    printf("2. Časový (určitý čas)\n");
    printf("Vybrať: ");
 
    int mode_choice;
    scanf("%d", &mode_choice);
    getchar();
 
    int mode = (mode_choice == 2) ? MODE_TIMED : MODE_STANDARD;
    int time_limit = 0;
 
    if (mode == MODE_TIMED) {
        printf("Zadaj čas (v sekundách): ");
        scanf("%d", &time_limit);
        getchar();
    }
 
    printf("\nVyber typ herného sveta:\n");
    printf("1. Bez prekážok (wrap-around na okrajoch)\n");
    printf("2. S náhodne generovanými prekážkami\n");
    printf("Vybrať: ");
 
    int world_choice;
    scanf("%d", &world_choice);
    getchar();
 
    int world_type = (world_choice == 2) ? WORLD_WITH_OBSTACLES : WORLD_NO_OBSTACLES;
 
    printf("\nSpúšťam server v pozadí...\n");
 
    C->server_pid = fork();
    if (C->server_pid == 0) {
        execl("./server", "server", NULL);
        perror("execl server");
        _exit(1);
    } else if (C->server_pid < 0) {
        printf("Chyba: Nepodarilo sa spustiť server\n");
        return;
    }
 
    usleep(200 * 1000); 
 
    int status = 0;
    pid_t r = waitpid(C->server_pid, &status, WNOHANG);
    int server_started = (r == 0);  

    if (!server_started) {
        printf("Server sa nepodarilo spustiť (port %d je už používaný). Pripájam sa do existujúcej hry...\n", PORT);
        C->server_pid = -1; 
    } else {
        usleep(300 * 1000);
    }
 
    if (connect_to_server(C)) {
        printf("Zadaj meno hráča: ");
        char name[50];
        fgets(name, 50, stdin);
        name[strcspn(name, "\n")] = 0;
 
        C->player_id = -1;
 
        if (server_started) {
            char ng[256];
            snprintf(ng, sizeof(ng), "NEW_GAME|%d|%d|%d", mode, world_type, time_limit);
            send_message(C, ng);
            usleep(100000);
        }
 
        char pl[256];
        snprintf(pl, sizeof(pl), "PLAYER|%s", name);
        send_message(C, pl);
 
        C->in_game = 1;
        printf("Hra sa spustila!\n");
        sleep(1);
    } else {
        printf("Chyba: Nepodarilo sa pripojiť k serveru\n");
        if (server_started && C->server_pid > 0) {
            kill(C->server_pid, SIGTERM);
            waitpid(C->server_pid, NULL, 0);
        }
    }
}


static void join_existing_game(client_ctx_t *C) {
    printf("\n╔════════════════════════════════════════╗\n");
    printf("║      PRIPOJIT SA K HRE                 ║\n");
    printf("╚════════════════════════════════════════╝\n\n");
    printf("Pokúšam sa pripojiť na localhost:%d\n", PORT);

    if (connect_to_server(C)) {
        printf("Zadaj meno hráča: ");
        char name[50];
        fgets(name, 50, stdin);
        name[strcspn(name, "\n")] = 0;

        C->player_id = -1;

        char msg[256];
        snprintf(msg, sizeof(msg), "PLAYER|%s", name);
        send_message(C, msg);

        C->in_game = 1;
        printf("Priradený si sa k hre!\n");
        sleep(1);
    } else {
        printf("Chyba: Server nie je dostupný\n");
    }
}

static int term_enable_raw(TermGuard *tg) {
    tg->active = 0;

    if (tcgetattr(STDIN_FILENO, &tg->orig) == -1) return -1;

    tg->orig_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (tg->orig_flags == -1) tg->orig_flags = 0;

    struct termios raw = tg->orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return -1;

    if (fcntl(STDIN_FILENO, F_SETFL, tg->orig_flags | O_NONBLOCK) == -1) {
        // ignore
    }

    tg->active = 1;
    return 0;
}

static void term_restore(TermGuard *tg) {
    if (!tg || !tg->active) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &tg->orig);
    fcntl(STDIN_FILENO, F_SETFL, tg->orig_flags);
    tg->active = 0;
}

static void game_loop(client_ctx_t *C) {
    TermGuard tg;
    int raw_ok = (term_enable_raw(&tg) == 0);

    // clear whole screen + hide cursor once
    printf("\033[2J\033[H\033[?25l");
    fflush(stdout);

    Direction current_dir = RIGHT;
    int game_active = 1;
    int paused = 0;

    while (C->in_game && game_active) {
        int got_state = 0;

        // ---------- WAIT FOR INPUT OR SOCKET (timeout = FPS) ----------
        fd_set rfds;
        FD_ZERO(&rfds);

        int maxfd = STDIN_FILENO;
        FD_SET(STDIN_FILENO, &rfds);

        if (C->sock >= 0) {
            FD_SET(C->sock, &rfds);
            if (C->sock > maxfd) maxfd = C->sock;
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 1000000 / FPS;

        int rv = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        // ---------- RECEIVE STATE ----------
        if (rv > 0 && C->sock >= 0 && FD_ISSET(C->sock, &rfds)) {
            receive_game_state(C, &got_state);
        } else {
            // aj keď select neukázal socket, občas môže zostať niečo v buffri
            // (MSG_DONTWAIT vo vnútri to nezablokuje)
            receive_game_state(C, &got_state);
        }

        // ---------- INPUT ----------
        char input = 0;
        if (rv > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            ssize_t n = read(STDIN_FILENO, &input, 1);
            if (n != 1) input = 0;
        }

        switch (input) {
            case 'W': case 'w': if (current_dir != DOWN)  current_dir = UP;    break;
            case 'S': case 's': if (current_dir != UP)    current_dir = DOWN;  break;
            case 'A': case 'a': if (current_dir != RIGHT) current_dir = LEFT;  break;
            case 'D': case 'd': if (current_dir != LEFT)  current_dir = RIGHT; break;
            case ' ': paused = !paused; break;
            case 'Q': case 'q': {
                C->in_game = 0;
                game_active = 0;
                char qmsg[64];
                snprintf(qmsg, sizeof(qmsg), "QUIT|%d", C->player_id);
                send_message(C, qmsg);
                break;
            }
            default:
                break;
        }

        // ---------- SEND MOVE ----------
        if (C->in_game && game_active && !paused && C->player_id >= 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "MOVE|%d|%d", C->player_id, (int)current_dir);
            send_message(C, msg);
        }

        // ---------- BUILD FRAME ----------
        static char frame[FRAME_BUF_SIZE];
        int off = 0;

        // keď ešte nemáme STATE, aspoň niečo zobraz
        if (!got_state && C->game_state.width == 0 && C->game_state.height == 0) {
            off = appendf(frame, (int)sizeof(frame), off, "Čakám na server... (STATE)");
            off = line_end(off, frame, (int)sizeof(frame));
            off = appendf(frame, (int)sizeof(frame), off, "player_id: %d", C->player_id);
            off = line_end(off, frame, (int)sizeof(frame));
            off = appendf(frame, (int)sizeof(frame), off, "Q=quit, SPACE=pause");
            off = line_end(off, frame, (int)sizeof(frame));
        } else {
            off = render_players_info_to_buf(C, frame, (int)sizeof(frame), off);
            off = render_world_to_buf(C, frame, (int)sizeof(frame), off);

            if (C->player_id >= 0 && C->player_id < C->game_state.num_players) {
                off = appendf(frame, (int)sizeof(frame), off, "Tvoj hadík: %s",
                              C->game_state.players[C->player_id].name);
                off = line_end(off, frame, (int)sizeof(frame));
                off = appendf(frame, (int)sizeof(frame), off, "Tvoje body: %d | Čas: %d s",
                              C->game_state.players[C->player_id].score,
                              C->game_state.elapsed_time);
                off = line_end(off, frame, (int)sizeof(frame));
            }

            off = appendf(frame, (int)sizeof(frame), off,
                          "Smer (W/S/A/D, SPACE=pause, Q=quit): %s",
                          paused ? "[PAUSED]" : "");
            off = line_end(off, frame, (int)sizeof(frame));
        }

        // ---------- DRAW FRAME (VŽDY) ----------
        clear_screen();
        fwrite(frame, 1, (size_t)off, stdout);
        fflush(stdout);

        if (C->game_state.game_over) {
            C->in_game = 0;
            game_active = 0;
        }
    }

    printf("\033[?25h");
    fflush(stdout);

    if (raw_ok) term_restore(&tg);
}

int main(void) {
    client_ctx_t C;
    memset(&C, 0, sizeof(C));
    C.sock = -1;
    C.player_id = -1;
    C.in_game = 0;
    C.server_pid = -1;
    clear_world(&C);

    printf("╔══════════════════════════════╗\n");
    printf("║     VITAJ V HRE HADIK!       ║\n");
    printf("╚══════════════════════════════╝\n\n");

    int choice = 0;
    int running = 1;

    while (running) {
        show_main_menu();
        scanf("%d", &choice);
        getchar();

        switch (choice) {
            case 1:
                create_new_game(&C);
                if (C.in_game) game_loop(&C);
                C.in_game = 0;
                break;
            case 2:
                join_existing_game(&C);
                if (C.in_game) game_loop(&C);
                C.in_game = 0;
                break;
            case 3:
                printf("\nZbohom!\n");
                running = 0;
                break;
            default:
                printf("Neplatná voľba!\n");
        }
    }

    if (C.sock >= 0) close(C.sock);
    if (C.server_pid > 0) {
        kill(C.server_pid, SIGTERM);
        waitpid(C.server_pid, NULL, 0);
    }
    return 0;
}
