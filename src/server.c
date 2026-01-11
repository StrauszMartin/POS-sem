#define _POSIX_C_SOURCE 200809L
#include "snake.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef MAX_CLIENTS
#define MAX_CLIENTS 10
#endif

#ifndef BUFFER_SIZE
#define BUFFER_SIZE 8192
#endif

static void sleep_us(long usec) {
    if (usec <= 0) return;
    struct timespec ts;
    ts.tv_sec = usec / 1000000L;
    ts.tv_nsec = (usec % 1000000L) * 1000L;
    nanosleep(&ts, NULL);
}

static int parse_port(int argc, char **argv) {
    int port = DEFAULT_PORT;
 
    if (argc >= 2) {
        port = atoi(argv[1]);
    }
 
    if (port < 20000 || port > 60000) {
        fprintf(stderr, "[SERVER] Neplatný port %d (povolené 20000-60000)\n", port);
        return -1;
    }
    return port;
}

typedef struct {
    int socket;
    int player_id;
    int in_use;
} Client;

typedef struct {
    GameState game;

    Client clients[MAX_CLIENTS];
    int num_clients;

    pthread_mutex_t mtx;
    int running;
} server_ctx_t;



static int snake_capacity(const Player *p) {
    return (int)(sizeof(p->body_x) / sizeof(p->body_x[0]));
}

static int is_obstacle(const GameState *g, int x, int y) {
    for (int i = 0; i < g->num_obstacles; i++) {
        if (g->obstacles[i][0] == x && g->obstacles[i][1] == y) return 1;
    }
    return 0;
}

static void generate_obstacles_random(GameState *g, int count) {
    g->num_obstacles = 0;
    if (count > MAX_OBSTACLES) count = MAX_OBSTACLES;

    int attempts = 0;
    while (g->num_obstacles < count && attempts < count * 3) {
        int x = rand() % g->width;
        int y = rand() % g->height;

        int valid = 1;
        for (int i = 0; i < g->num_players; i++) {
            if (g->players[i].head_x == x && g->players[i].head_y == y) {
                valid = 0;
                break;
            }
        }

        if (valid && !is_obstacle(g, x, y) && g->num_obstacles < MAX_OBSTACLES) {
            g->obstacles[g->num_obstacles][0] = x;
            g->obstacles[g->num_obstacles][1] = y;
            g->num_obstacles++;
        }

        attempts++;
    }

    fprintf(stderr, "[SERVER] %d prekážok vygenerovaných\n", g->num_obstacles);
}


static int cell_occupied_by_snake(const GameState *g, int x, int y) {
    for (int i = 0; i < g->num_players; i++) {
        const Player *p = &g->players[i];
        if (!p->alive) continue;
        for (int j = 0; j < p->body_len; j++) {
            if (p->body_x[j] == x && p->body_y[j] == y) return 1;
        }
    }
    return 0;
}
 
static int cell_occupied_by_fruit(const GameState *g, int x, int y) {
    for (int i = 0; i < g->num_fruits; i++) {
        if (g->fruits[i][0] == x && g->fruits[i][1] == y) return 1;
    }
    return 0;
}
 
static int alive_snakes(const GameState *g) {
    int c = 0;
    for (int i = 0; i < g->num_players; i++) {
        if (g->players[i].alive) c++;
    }
    return c;
}
 
static void sync_legacy_fruit_xy(GameState *g) {
    if (g->num_fruits > 0) {
        g->fruit_x = g->fruits[0][0];
        g->fruit_y = g->fruits[0][1];
    } else {
        g->fruit_x = -1;
        g->fruit_y = -1;
    }
}
 
static void spawn_fruit_at(GameState *g, int idx) {
    int x, y;
    int tries = 3000;
 
    do {
        x = rand() % g->width;
        y = rand() % g->height;
        tries--;
    } while (
        tries > 0 &&
        (is_obstacle(g, x, y) ||
         cell_occupied_by_snake(g, x, y) ||
         cell_occupied_by_fruit(g, x, y))
    );
 
    if (tries <= 0) { x = 0; y = 0; }
 
    g->fruits[idx][0] = x;
    g->fruits[idx][1] = y;
 
    sync_legacy_fruit_xy(g);
 
    fprintf(stderr, "[SERVER] Ovocie[%d] vygenerované: (%d, %d)\n", idx, x, y);
}
 
static void ensure_fruits_count(GameState *g) {
    int want = alive_snakes(g);
    if (want > MAX_FRUITS) want = MAX_FRUITS;
 
    while (g->num_fruits < want) {
        spawn_fruit_at(g, g->num_fruits);
        g->num_fruits++;
    }
 
    while (g->num_fruits > want) {
        g->num_fruits--;
    }
 
    sync_legacy_fruit_xy(g);
}

static void init_game(GameState *g, int width, int height, GameMode mode, int time_limit, WorldType world_type) {
    g->id = rand() % 10000;
    g->width = width;
    g->height = height;
    g->num_players = 0;
    g->mode = mode;
    g->time_limit = time_limit;
    g->elapsed_time = 0;
    g->active = 0;
    g->game_over = 0;
    g->world_type = world_type;
    g->start_time = time(NULL);

    g->num_obstacles = 0;
    if (world_type == WORLD_WITH_OBSTACLES) {
        int obstacle_count = (width * height) / 8;
        generate_obstacles_random(g, obstacle_count);
    }

    g->num_fruits = 0;
    ensure_fruits_count(g);

    fprintf(stderr, "[SERVER] Hra inicializovaná: %dx%d, režim: %d, svet: %d\n",
            width, height, mode, world_type);
}

static int init_snake(GameState *g, int player_id, const char *name) {
    if (g->num_players >= 10) return -1;

    Player *p = &g->players[g->num_players];
    p->id = player_id;
    p->alive = 1;
    p->score = 0;

    strncpy(p->name, name, sizeof(p->name) - 1);
    p->name[sizeof(p->name) - 1] = '\0';

    p->direction = RIGHT;
    p->next_direction = RIGHT;

    p->head_x = 5 + g->num_players * 5;
    if (p->head_x < 0) p->head_x = 0;
    if (p->head_x >= g->width) p->head_x = g->width / 2;

    p->head_y = g->height / 2;

    int cap = snake_capacity(p);
    p->body_len = INITIAL_SNAKE_LEN;
    if (p->body_len > cap) p->body_len = cap;

    for (int i = 0; i < p->body_len; i++) {
        int bx = p->head_x - i;
        if (bx < 0) bx += g->width;
        if (bx >= g->width) bx %= g->width;

        p->body_x[i] = bx;
        p->body_y[i] = p->head_y;
    }

    g->num_players++;
    if (g->num_players == 1) {
        g->active = 1;
        g->game_over = 0;
        g->start_time = time(NULL);
    }

    fprintf(stderr, "[SERVER] Hadík '%s' vytvorený (ID: %d)\n", p->name, player_id);
    ensure_fruits_count(g);
    return player_id;
}

static void update_snake(GameState *g, Player *p) {
    if (!p->alive) return;

    p->direction = p->next_direction;

    int new_x = p->head_x;
    int new_y = p->head_y;

    switch (p->direction) {
        case UP:    new_y--; break;
        case DOWN:  new_y++; break;
        case LEFT:  new_x--; break;
        case RIGHT: new_x++; break;
        case NONE:  break;
    }

    if (g->world_type == WORLD_NO_OBSTACLES) {
        if (new_x < 0) new_x = g->width - 1;
        if (new_x >= g->width) new_x = 0;
        if (new_y < 0) new_y = g->height - 1;
        if (new_y >= g->height) new_y = 0;
    } else {
        if (new_x < 0 || new_x >= g->width || new_y < 0 || new_y >= g->height) {
            p->alive = 0;
            fprintf(stderr, "[SERVER] Hadík '%s' narazil do okraja!\n", p->name);
            return;
        }
    }

    if (is_obstacle(g, new_x, new_y)) {
        p->alive = 0;
        fprintf(stderr, "[SERVER] Hadík '%s' narazil do prekážky!\n", p->name);
        return;
    }

    for (int i = 0; i < p->body_len - 1; i++) {
        if (p->body_x[i] == new_x && p->body_y[i] == new_y) {
            p->alive = 0;
            fprintf(stderr, "[SERVER] Hadík '%s' narazil sám do seba!\n", p->name);
            return;
        }
      }

    for (int i = 0; i < g->num_players; i++) {
        if (g->players[i].id == p->id) continue;
        if (!g->players[i].alive) continue;
        for (int j = 0; j < g->players[i].body_len; j++) {
            if (g->players[i].body_x[j] == new_x && g->players[i].body_y[j] == new_y) {
                p->alive = 0;
                fprintf(stderr, "[SERVER] Hadík '%s' narazil do iného hadíka!\n", p->name);
                return;
            }
        }
    }

    for (int i = p->body_len - 1; i > 0; i--) {
        p->body_x[i] = p->body_x[i - 1];
        p->body_y[i] = p->body_y[i - 1];
    }

    p->head_x = new_x;
    p->head_y = new_y;
    p->body_x[0] = new_x;
    p->body_y[0] = new_y;

    for (int f = 0; f < g->num_fruits; f++) {
        if (p->head_x == g->fruits[f][0] && p->head_y == g->fruits[f][1]) {
            p->score += 10;
 
            int cap = snake_capacity(p);
            if (p->body_len < cap) p->body_len++;
 
            fprintf(stderr, "[SERVER] Hadík '%s' zjedol ovocie[%d]! Body: %d\n", p->name, f, p->score);
            spawn_fruit_at(g, f);
            break;
        }
    }    
}

static void build_map(const GameState *g, char *out) {
    int k = 0;

    for (int y = 0; y < g->height; y++)
        for (int x = 0; x < g->width; x++)
            out[k++] = '.';

    for (int i = 0; i < g->num_obstacles; i++) {
        int x = g->obstacles[i][0], y = g->obstacles[i][1];
        if (x >= 0 && x < g->width && y >= 0 && y < g->height)
            out[y * g->width + x] = '#';
    }

    for (int i = 0; i < g->num_fruits; i++) {
        int fx = g->fruits[i][0], fy = g->fruits[i][1];
        if (fx >= 0 && fx < g->width && fy >= 0 && fy < g->height)
            out[fy * g->width + fx] = '*';
    }  


    for (int i = 0; i < g->num_players; i++) {
        const Player *p = &g->players[i];
        if (!p->alive) continue;

        for (int j = 1; j < p->body_len; j++) {
            int x = p->body_x[j], y = p->body_y[j];
            if (x >= 0 && x < g->width && y >= 0 && y < g->height)
                out[y * g->width + x] = '~';
        }

        if (p->head_x >= 0 && p->head_x < g->width && p->head_y >= 0 && p->head_y < g->height)
            out[p->head_y * g->width + p->head_x] = '@';
    }

    out[k] = '\0';
}

void send_game_state(server_ctx_t* S, int client_socket) {
    pthread_mutex_lock(&S->mtx);

    char response[8192];
    int off = 0;

    GameState * g = &S->game;

    off += snprintf(response + off, (int)sizeof(response) - off,
        "STATE|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|",
        g->id,
        g->width,
        g->height,
        g->num_players,
        g->fruit_x,
        g->fruit_y,
        g->active,
        g->game_over,
        g->num_obstacles,
        g->mode,
        g->world_type,
        g->elapsed_time
    );

    // ---------- MAPA (VŽDY) ----------
    char mapbuf[WORLD_WIDTH * WORLD_HEIGHT + 1];
    build_map(g, mapbuf);

    off += snprintf(response + off, (int)sizeof(response) - off,
        "M|%s|", mapbuf);

    // ---------- PREKÁŽKY ----------
    for (int i = 0; i < g->num_obstacles; i++) {
        if (off > (int)sizeof(response) - 64) break;
        off += snprintf(response + off, (int)sizeof(response) - off,
            "O|%d|%d|",
            g->obstacles[i][0],
            g->obstacles[i][1]
        );
    }

    // ---------- HRÁČI ----------
    for (int i = 0; i < g->num_players; i++) {
        if (off > (int)sizeof(response) - 256) break;
        Player *p = &g->players[i];
        off += snprintf(response + off, (int)sizeof(response) - off,
            "P|%d|%s|%d|%d|%d|%d|%d|%d|",
            p->id,
            p->name,
            p->alive,
            p->score,
            p->head_x,
            p->head_y,
            p->body_len,
            p->direction
        );
    }

    // ---------- KONIEC RIADKU ----------
    if (off < (int)sizeof(response) - 2) {
        response[off++] = '\n';
        response[off] = '\0';
    } else {
        response[sizeof(response) - 2] = '\n';
        response[sizeof(response) - 1] = '\0';
        off = (int)sizeof(response) - 1;
    }

    pthread_mutex_unlock(&S->mtx);

    send(client_socket, response, (size_t)off, 0);
}
static void* game_loop(void *arg) {
    server_ctx_t *S = (server_ctx_t*)arg;

    while (S->running) {
        int sockets[MAX_CLIENTS];
        int sock_count = 0;

        pthread_mutex_lock(&S->mtx);

        if (S->game.active && S->game.num_players > 0 && !S->game.game_over) {
            S->game.elapsed_time = (int)(time(NULL) - S->game.start_time);

            if (S->game.mode == MODE_TIMED && S->game.elapsed_time >= S->game.time_limit) {
                S->game.active = 0;
                S->game.game_over = 1;
                fprintf(stderr, "[SERVER] Čas vypršal! KONIEC HRY!\n");
            } else {
                for (int i = 0; i < S->game.num_players; i++) {
                    if (S->game.players[i].alive) {
                        update_snake(&S->game, &S->game.players[i]);
                    }
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (S->clients[i].in_use) sockets[sock_count++] = S->clients[i].socket;
        }

        pthread_mutex_unlock(&S->mtx);

        for (int i = 0; i < sock_count; i++) {
            send_game_state(S, sockets[i]);
        }

        sleep_us(1000000L / FPS);
    }

    return NULL;
}

static int find_client_index_by_socket(server_ctx_t *S, int s) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (S->clients[i].in_use && S->clients[i].socket == s) return i;
    }
    return -1;
}

typedef struct {
    server_ctx_t *S;
    int client_socket;
} handler_arg_t;

static void* client_handler(void *arg) {
    handler_arg_t *H = (handler_arg_t*)arg;
    server_ctx_t *S = H->S;
    int client_socket = H->client_socket;
    free(H);

    char buffer[BUFFER_SIZE];

    while (S->running) {
        int n = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) break;
        buffer[n] = '\0';

        pthread_mutex_lock(&S->mtx);

          int cidx = find_client_index_by_socket(S, client_socket);
        
        if (strncmp(buffer, "NEW_GAME", 8) == 0) {
            int mode, world_type, time_limit, w, h;
 
            int nparsed = sscanf(buffer, "NEW_GAME|%d|%d|%d|%d|%d", &mode, &world_type, &time_limit, &w, &h);
            
            if (nparsed == 5) {
                init_game(&S->game, w, h, (GameMode)mode, time_limit, (WorldType)world_type);
            }
        } else if (strncmp(buffer, "PLAYER", 6) == 0) {
            // ak klient pošle PLAYER bez NEW_GAME, sprav default init
            if (S->game.width <= 0 || S->game.height <= 0) {
                init_game(&S->game, WORLD_WIDTH, WORLD_HEIGHT, MODE_TIMED, 365 * 24 * 3600, WORLD_NO_OBSTACLES);
            }

            char name[50];
            sscanf(buffer, "PLAYER|%49[^|]", name);

            int assigned = init_snake(&S->game, S->game.num_players, name);
            if (cidx >= 0) S->clients[cidx].player_id = assigned;

            if (assigned >= 0) {
                char msg[64];
                snprintf(msg, sizeof(msg), "ASSIGN|%d|\n", assigned);
                (void)send(client_socket, msg, strlen(msg), 0);
            }

        } else if (strncmp(buffer, "MOVE", 4) == 0) {
            int pid_from_client, dir;
            sscanf(buffer, "MOVE|%d|%d", &pid_from_client, &dir);

            int pid = (cidx >= 0) ? S->clients[cidx].player_id : -1;
            if (pid >= 0 && pid < S->game.num_players && S->game.players[pid].alive) {
                S->game.players[pid].next_direction = (Direction)dir;
            }

        } else if (strncmp(buffer, "QUIT", 4) == 0) {
            int pid = (cidx >= 0) ? S->clients[cidx].player_id : -1;
            if (pid >= 0 && pid < S->game.num_players) {
                S->game.players[pid].alive = 0;
                ensure_fruits_count(&S->game);
            }
        }

        pthread_mutex_unlock(&S->mtx);
    }

    close(client_socket);


    pthread_mutex_lock(&S->mtx);
 
    int cidx = find_client_index_by_socket(S, client_socket);
    if (cidx >= 0) {
        int pid = S->clients[cidx].player_id;
 
        // odregistruj klienta
        S->clients[cidx].in_use = 0;
        S->clients[cidx].socket = -1;
        S->clients[cidx].player_id = -1;
        if (S->num_clients > 0) S->num_clients--;
 
        // zabi hráča (ak bol priradený)
        if (pid >= 0 && pid < S->game.num_players) {
            S->game.players[pid].alive = 0;
            ensure_fruits_count(&S->game);
        }
    } 

    pthread_mutex_unlock(&S->mtx);

    fprintf(stderr, "[SERVER] Klient odpojený, aktívni klienti: %d\n", S->num_clients);
    return NULL;
}

int main(int argc, char **argv) {
    int port = parse_port(argc,argv);
    if (port < 0) return 1;

    fprintf(stderr, "SERVER HADIK - port %d\n", port);


    srand((unsigned)time(NULL));

    server_ctx_t S;
    memset(&S, 0, sizeof(S));
    pthread_mutex_init(&S.mtx, NULL);
    S.running = 1;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        S.clients[i].socket = -1;
        S.clients[i].in_use = 0;
        S.clients[i].player_id = -1;
    }

  

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_sock);
        return 1;
    }

    if (listen(server_sock, MAX_CLIENTS) < 0) {
        perror("listen");
        close(server_sock);
        return 1;
    }

    fprintf(stderr, "[SERVER] Čaká sa na klientov...\n");

    pthread_t game_thread;
    pthread_create(&game_thread, NULL, game_loop, &S);

    while (S.running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) continue;

        pthread_mutex_lock(&S.mtx);

        if (S.num_clients >= MAX_CLIENTS) {
            const char *full_msg = "SERVER_FULL\n";
            (void)send(client_socket, full_msg, strlen(full_msg), 0);
            close(client_socket);
            pthread_mutex_unlock(&S.mtx);
            fprintf(stderr, "[SERVER] Pripojenie odmietnuté, MAX_CLIENTS=%d\n", MAX_CLIENTS);
            continue;
        }

        int idx = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!S.clients[i].in_use) { idx = i; break; }
        }

        if (idx == -1) {
            const char *full_msg = "SERVER_FULL\n";
            (void)send(client_socket, full_msg, strlen(full_msg), 0);
            close(client_socket);
            pthread_mutex_unlock(&S.mtx);
            continue;
        }

        S.clients[idx].socket = client_socket;
        S.clients[idx].in_use = 1;
        S.clients[idx].player_id = -1;
        S.num_clients++;

        fprintf(stderr, "[SERVER] Klient #%d sa pripojil: %s:%d (aktívni: %d)\n",
                idx, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), S.num_clients);

        pthread_mutex_unlock(&S.mtx);

        handler_arg_t *H = (handler_arg_t*)malloc(sizeof(handler_arg_t));
        H->S = &S;
        H->client_socket = client_socket;

        pthread_t thread;
        pthread_create(&thread, NULL, client_handler, H);
        pthread_detach(thread);
    }

    S.running = 0;
    pthread_join(game_thread, NULL);
    close(server_sock);

    pthread_mutex_destroy(&S.mtx);
    fprintf(stderr, "[SERVER] Server sa vypína...\n");
    return 0;
}
