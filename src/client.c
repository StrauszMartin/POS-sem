#include "snake.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>

typedef struct {
    struct termios orig;
    int orig_flags;
    int active;
} TermGuard;

int sock = -1;
GameState game_state;
int player_id = -1;
int in_game = 0;
char world[WORLD_HEIGHT][WORLD_WIDTH];
int server_pid = -1;

void clear_screen() {
    printf("\033[2J\033[1;1H");
    fflush(stdout);
}

int connect_to_server() {
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("Chyba: Nepodarilo sa vytvoriť socket\n");
        return 0;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_aton("127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("Chyba: Nepodarilo sa pripojiť k serveru na localhost:%d\n", PORT);
        close(sock);
        sock = -1;
        return 0;
    }

    printf("[KLIENT] Pripojené k serveru!\n");
    return 1;
}

void send_message(const char* msg) {
    if (sock < 0) return;
    send(sock, msg, strlen(msg), 0);
}

void clear_world() {
    for (int y = 0; y < WORLD_HEIGHT; y++)
        for (int x = 0; x < WORLD_WIDTH; x++)
            world[y][x] = ' ';
}

void render_players_info() {
    printf("Max hráčov: %d\n", MAX_CLIENTS);

    // pre každý možný slot hráča
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (i < game_state.num_players) {
            Player *p = &game_state.players[i];

            if (!p->alive) {
                printf("Hráč %s je mŕtvy. Skóre: %d\n", p->name, p->score);
            } else {
                printf("Meno: %s\n", p->name);
                printf("ID: %d\n", p->id);
                printf("Skóre: %d\n", p->score);
            }
        } else {
            // slot bez hráča (nikdy nebol alebo sa odpojil)
            printf("______________________________________\n");
        }

        printf("\n");
    }

    // zvýraznenie seba samého (ak chceš extra riadok)
    if (player_id >= 0 && player_id < game_state.num_players) {
        Player *me = &game_state.players[player_id];
        printf("== TY ==\n");
        printf("Meno: %s | ID: %d | Skóre: %d\n\n",
               me->name, me->id, me->score);
    }
}



void render_world() {
    printf("\n╔");
    for (int x = 0; x < WORLD_WIDTH; x++) printf("═");
    printf("╗\n");

    for (int y = 0; y < WORLD_HEIGHT; y++) {
        printf("║");
        for (int x = 0; x < WORLD_WIDTH; x++) {
            char c = world[y][x];
            if (c == '.') c = ' ';
            printf("%c", c);
        }
        printf("║\n");
    }

    printf("╚");
    for (int x = 0; x < WORLD_WIDTH; x++) printf("═");
    printf("╝\n");
}

static const char* skip_next_field(const char* p) {
    const char* q = strchr(p, '|');
    return q ? (q + 1) : NULL;
}

void parse_game_state(const char* buffer) {
    if (strncmp(buffer, "STATE", 5) != 0) return;

    int parts[12];
    if (sscanf(buffer, "STATE|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|",
&parts[0], &parts[1], &parts[2], &parts[3], &parts[4], &parts[5],
&parts[6], &parts[7], &parts[8], &parts[9], &parts[10], &parts[11]) != 12) {
        return;
    }

    game_state.id = parts[0];
    game_state.width = parts[1];
    game_state.height = parts[2];
    game_state.num_players = parts[3];
    game_state.fruit_x = parts[4];
    game_state.fruit_y = parts[5];
    game_state.active = parts[6];
    game_state.game_over = parts[7];
    game_state.num_obstacles = parts[8];
    game_state.mode = parts[9];
    game_state.world_type = parts[10];
    game_state.elapsed_time = parts[11];

    // posuň ptr ZA 12. číselné pole (elapsed_time) a jeho '|'
    const char* ptr = strchr(buffer, '|');
    for (int i = 0; i < 12 && ptr; i++) ptr = strchr(ptr + 1, '|');
    if (!ptr) return;
    ptr++; // teraz ptr ukazuje na začiatok ďalšieho segmentu (M| alebo O| alebo P|)

    // --- M|<mapa>| (800 znakov) ---
    // Vymaž world a naplň ho mapou zo servera.
    clear_world();

    if (ptr && strncmp(ptr, "M|", 2) == 0) {
        ptr += 2;
        const char* end = strchr(ptr, '|');
        if (end) {
            int len = (int)(end - ptr);
            int expected = game_state.width * game_state.height; // typicky 800

            if (len >= expected) {
                for (int y = 0; y < game_state.height; y++) {
                    for (int x = 0; x < game_state.width; x++) {
                        char c = ptr[y * game_state.width + x];
                        if (c == '.') c = ' ';
                        world[y][x] = c;
                    }
                }
            }

            ptr = end + 1; // za ukončovací '|'
        } else {
            return;
        }
    }

    // --- O|x|y| ... ---
    int obs_count = 0;
    while (ptr && strncmp(ptr, "O|", 2) == 0 && obs_count < MAX_OBSTACLES) {
        int ox, oy;
        if (sscanf(ptr, "O|%d|%d|", &ox, &oy) == 2) {
            game_state.obstacles[obs_count][0] = ox;
            game_state.obstacles[obs_count][1] = oy;
            obs_count++;
        }

        // preskoč 3 polia: O|x|y|
        ptr = skip_next_field(ptr); // za "O"
        ptr = ptr ? skip_next_field(ptr) : NULL; // za x
        ptr = ptr ? skip_next_field(ptr) : NULL; // za y
    }
    game_state.num_obstacles = obs_count;

    // --- P|id|name|alive|score|hx|hy|blen|dir| ... ---
    int pl_count = 0;
    while (ptr && strncmp(ptr, "P|", 2) == 0 && pl_count < 10) {
        Player* p = &game_state.players[pl_count];
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

        // preskoč 9 polí: P|id|name|alive|score|hx|hy|blen|dir|
        ptr = skip_next_field(ptr); // za P
        for (int k = 0; k < 8 && ptr; k++) ptr = skip_next_field(ptr);
    }
    game_state.num_players = pl_count;
}

void receive_game_state() {
    if (sock < 0) return;

    char buffer[BUFFER_SIZE];
    int n = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (n > 0) {
        buffer[n] = '\0';

        if (strncmp(buffer, "ASSIGN|", 7) == 0) {
            int id;
            if (sscanf(buffer, "ASSIGN|%d|", &id) == 1) {
                player_id = id;
            }
            return;
        }

        parse_game_state(buffer);
    }
}

void show_main_menu() {
    printf("\n");
    printf("╔════════════════════════════════════════╗\n");
    printf("║         HADIK - HLAVNE MENU            ║\n");
    printf("╚════════════════════════════════════════╝\n\n");
    printf("1. Nová hra (vytvor server)\n");
    printf("2. Pripojiť sa k existujúcej hre\n");
    printf("3. Koniec\n\n");
    printf("Vybrať možnosť: ");
}

void create_new_game() {
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

    server_pid = fork();
    if (server_pid == 0) {
        chdir("/src");
        execl("./server", "server", NULL);
        perror("execl server");
        exit(1);
    } else if (server_pid < 0) {
        printf("Chyba: Nepodarilo sa spustiť server\n");
        return;
    }

    sleep(2);

    if (connect_to_server()) {
        printf("Zadaj meno hráča: ");
        char name[50];
        fgets(name, 50, stdin);
        name[strcspn(name, "\n")] = 0;

        player_id = -1;

        char msg[256];
        snprintf(msg, 256, "NEW_GAME|%d|%d|%d", mode, world_type, time_limit);
        send_message(msg);

        usleep(100000);

        snprintf(msg, 256, "PLAYER|%s", name);
        send_message(msg);

        in_game = 1;
        printf("Hra sa spustila!\n");
        sleep(1);
    } else {
        printf("Chyba: Nepodarilo sa pripojiť k serveru\n");
        if (server_pid > 0) {
            kill(server_pid, SIGTERM);
        }
    }
}

void join_existing_game() {
    printf("\n╔════════════════════════════════════════╗\n");
    printf("║      PRIPOJIT SA K HRE                 ║\n");
    printf("╚════════════════════════════════════════╝\n\n");
    printf("Pokúšam sa pripojiť na localhost:%d\n", PORT);

    if (connect_to_server()) {
        printf("Zadaj meno hráča: ");
        char name[50];
        fgets(name, 50, stdin);
        name[strcspn(name, "\n")] = 0;

        player_id = -1;

        char msg[256];
        snprintf(msg, 256, "PLAYER|%s", name);
        send_message(msg);

        in_game = 1;
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
    raw.c_lflag &= ~(ICANON | ECHO);   // bez Enter, bez echo
    raw.c_cc[VMIN] = 0;               // neblokujúci read
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return -1;

    // stdin non-blocking (nepovinné, ale praktické)
    if (fcntl(STDIN_FILENO, F_SETFL, tg->orig_flags | O_NONBLOCK) == -1) {
        // aj keby to nešlo, raw režim stále funguje; nechaj to tak
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

void game_loop() {
    TermGuard tg;
    int raw_ok = (term_enable_raw(&tg) == 0);

    Direction current_dir = RIGHT;
    int game_active = 1;
    int paused = 0;

    while (in_game && game_active) {
        // 1) počkaj max 100ms na socket alebo kláves
        fd_set rfds;
        FD_ZERO(&rfds);

        int maxfd = STDIN_FILENO;
        FD_SET(STDIN_FILENO, &rfds);

        if (sock >= 0) {
            FD_SET(sock, &rfds);
            if (sock > maxfd) maxfd = sock;
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms tick

        int rv = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        // 2) dáta zo servera
        if (rv > 0 && sock >= 0 && FD_ISSET(sock, &rfds)) {
            receive_game_state();
        }

        // 3) kláves
        char input = 0;
        if (rv > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            ssize_t n = read(STDIN_FILENO, &input, 1);
            if (n != 1) input = 0;
        }

        // 4) spracovanie kláves (zakázané otočenie o 180°)
        switch (input) {
            case 'W': case 'w':
                if (current_dir != DOWN) current_dir = UP;
                break;
            case 'S': case 's':
                if (current_dir != UP) current_dir = DOWN;
                break;
            case 'A': case 'a':
                if (current_dir != RIGHT) current_dir = LEFT;
                break;
            case 'D': case 'd':
                if (current_dir != LEFT) current_dir = RIGHT;
                break;

            case ' ': // pause toggle
                paused = !paused;
                break;

            case 'Q': case 'q': {
                in_game = 0;
                game_active = 0;

                char qmsg[100];
                snprintf(qmsg, sizeof(qmsg), "QUIT|%d", player_id);
                send_message(qmsg);
                break;
            }

            default:
                break;
        }

        // 5) kontinuálny pohyb: posielaj MOVE každý tick, iba keď nie je pauza
        if (in_game && game_active && !paused) {
            char msg[100];
            snprintf(msg, sizeof(msg), "MOVE|%d|%d", player_id, (int)current_dir);
            send_message(msg);
        }

        // 6) render
        clear_screen();
        render_players_info();
        render_world();

        if (game_state.num_players > player_id) {
            printf("Tvoj hadík: %s\n", game_state.players[player_id].name);
            printf("Tvoje body: %d | Čas: %d s\n",
                   game_state.players[player_id].score,
                   game_state.elapsed_time);
        }

        printf("Smer (W/S/A/D, SPACE=pause, Q=quit): %s\n", paused ? "[PAUSED]" : "");
        fflush(stdout);

        // 7) game over
        if (game_state.game_over) {
            in_game = 0;
            game_active = 0;

            printf("\n=== KONIEC HRY ===\n");
            if (game_state.num_players > player_id) {
                printf("Tvoje finálne body: %d\n",
                       game_state.players[player_id].score);
                printf("Čas v hre: %d sekúnd\n", game_state.elapsed_time);
            }

            usleep(500000);
        }
    }

    if (raw_ok) term_restore(&tg);
}

int main() {
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
                create_new_game();
                if (in_game) game_loop();
                in_game = 0;
                break;
            case 2:
                join_existing_game();
                if (in_game) game_loop();
                in_game = 0;
                break;
            case 3:
                printf("\nZbohom!\n");
                running = 0;
                break;
            default:
                printf("Neplatná voľba!\n");
        }
    }

    if (sock >= 0) close(sock);
    if (server_pid > 0) {
        kill(server_pid, SIGTERM);
        waitpid(server_pid, NULL, 0);
    }
    return 0;
}
