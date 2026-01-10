#include "snake.h"
#include <termios.h>


void send_game_state(int client_socket);

typedef struct {
    int socket;
    int player_id;
    int in_use;
    char name[50];
} Client;

GameState game;
Client clients[MAX_CLIENTS];
int num_clients = 0;
pthread_mutex_t game_mutex = PTHREAD_MUTEX_INITIALIZER;
int running = 1;

static int is_obstacle(int x, int y) {
    for (int i = 0; i < game.num_obstacles; i++) {
        if (game.obstacles[i][0] == x && game.obstacles[i][1] == y) return 1;
    }
    return 0;
}

void generate_obstacles_random(int count) {
    game.num_obstacles = 0;
    if (count > MAX_OBSTACLES) count = MAX_OBSTACLES;
    int attempts = 0;

    while (game.num_obstacles < count && attempts < count * 3) {
        int x = rand() % game.width;
        int y = rand() % game.height;

        int valid = 1;
        for (int i = 0; i < game.num_players; i++) {
            if (game.players[i].head_x == x && game.players[i].head_y == y) {
                valid = 0;
                break;
            }
        }

        if (valid && !is_obstacle(x, y) && game.num_obstacles < MAX_OBSTACLES) {
            game.obstacles[game.num_obstacles][0] = x;
            game.obstacles[game.num_obstacles][1] = y;
            game.num_obstacles++;
        }

        attempts++;
    }

    fprintf(stderr, "[SERVER] %d prekážok vygenerovaných\n", game.num_obstacles);
}

void init_game(int width, int height, GameMode mode, int time_limit, WorldType world_type) {
    game.id = rand() % 10000;
    game.width = width;
    game.height = height;
    game.num_players = 0;
    game.mode = mode;
    game.time_limit = time_limit;
    game.elapsed_time = 0;
    game.active = 0;
    game.game_over = 0;
    game.world_type = world_type;
    game.start_time = time(NULL);

    game.num_obstacles = 0;

    if (world_type == WORLD_WITH_OBSTACLES) {
        int obstacle_count = (width * height) / 8;
        generate_obstacles_random(obstacle_count);
    }

    int valid = 0;
    while (!valid) {
        game.fruit_x = rand() % width;
        game.fruit_y = rand() % height;
        valid = 1;
        if (is_obstacle(game.fruit_x, game.fruit_y)) {
            valid = 0;
        }
    }

    fprintf(stderr, "[SERVER] Hra inicializovaná: %dx%d, režim: %d, svet: %d\n",
            width, height, mode, world_type);
}

void init_snake(int player_id, const char* name) {
    if (game.num_players >= 10) return;

    Player* p = &game.players[game.num_players];
    p->id = player_id;
    p->alive = 1;
    p->score = 0;
    strcpy(p->name, name);
    p->direction = RIGHT;
    p->next_direction = RIGHT;
    p->head_x = 5 + game.num_players * 5;
    p->head_y = game.height / 2;
    p->body_len = INITIAL_SNAKE_LEN;

    for (int i = 0; i < INITIAL_SNAKE_LEN; i++) {
        p->body_x[i] = p->head_x - i;
        p->body_y[i] = p->head_y;
    }

    game.num_players++;
    if (game.num_players == 1) {
        game.active = 1;
        game.game_over = 0;
        game.start_time = time(NULL);
    }
    fprintf(stderr, "[SERVER] Hadík '%s' vytvorený (ID: %d)\n", name, player_id);
}

void spawn_fruit() {
    int valid = 0;
    while (!valid) {
        game.fruit_x = rand() % game.width;
        game.fruit_y = rand() % game.height;
        valid = 1;

        if (is_obstacle(game.fruit_x, game.fruit_y)) {
            valid = 0;
            continue;
        }

        for (int i = 0; i < game.num_players; i++) {
            if (!game.players[i].alive) continue;
            for (int j = 0; j < game.players[i].body_len; j++) {
                if (game.players[i].body_x[j] == game.fruit_x &&
                    game.players[i].body_y[j] == game.fruit_y) {
                    valid = 0;
                    break;
                }
            }
        }
    }
    fprintf(stderr, "[SERVER] Ovocie vygenerované: (%d, %d)\n", game.fruit_x, game.fruit_y);
}

void update_snake(Player* p) {
    if (!p->alive) return;

    p->direction = p->next_direction;

    int new_x = p->head_x;
    int new_y = p->head_y;

    switch (p->direction) {
        case UP: new_y--; break;
        case DOWN: new_y++; break;
        case LEFT: new_x--; break;
        case RIGHT: new_x++; break;
        case NONE: break;
    }

    if (game.world_type == WORLD_NO_OBSTACLES) {
        if (new_x < 0) new_x = game.width - 1;
        if (new_x >= game.width) new_x = 0;
        if (new_y < 0) new_y = game.height - 1;
        if (new_y >= game.height) new_y = 0;
    } else { //tu niekde bude treba zmenit to ze sa zaobrazi na druhej strane ale este si to premyslim
        if (new_x < 0 || new_x >= game.width || new_y < 0 || new_y >= game.height) {
            p->alive = 0;
            fprintf(stderr, "[SERVER] Hadík '%s' narazil do okraja!\n", p->name);
            return;
        }
    }

    if (is_obstacle(new_x, new_y)) {
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

    for (int i = 0; i < game.num_players; i++) {
        if (i == p->id || !game.players[i].alive) continue;
        for (int j = 0; j < game.players[i].body_len; j++) {
            if (game.players[i].body_x[j] == new_x && game.players[i].body_y[j] == new_y) {
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

    if (p->head_x == game.fruit_x && p->head_y == game.fruit_y) {
        p->score += 10;
        p->body_len++;
        fprintf(stderr, "[SERVER] Hadík '%s' zjedol ovocie! Body: %d\n", p->name, p->score);
        spawn_fruit();
    }
}

void* game_loop(void* arg) {
    while (running) {
        pthread_mutex_lock(&game_mutex);

        if (!game.active || game.num_players == 0) {
            pthread_mutex_unlock(&game_mutex);
            usleep(1000000 / FPS);
            continue;
        }

        if (game.active) {
            game.elapsed_time = (int)(time(NULL) - game.start_time);

            if (game.mode == MODE_TIMED && game.elapsed_time >= game.time_limit) {
                game.active = 0;
                game.game_over = 1;
                fprintf(stderr, "[SERVER] Čas vypršal! KONIEC HRY!\n"); 
                //vypisat stats
            } else {
                for (int i = 0; i < game.num_players; i++) {
                    if (game.players[i].alive) {
                        update_snake(&game.players[i]);
                    }
                }
            }
        }

        pthread_mutex_unlock(&game_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].in_use) {
                send_game_state(clients[i].socket);
            }
        }
        usleep(1000000 / FPS);

    }
    return NULL;
}

static void build_map(char *out) {
    // out musí mať aspoň WORLD_WIDTH*WORLD_HEIGHT + 1
    int k = 0;

    // '.' = prázdne (nepoužívaj ' ' kvôli parsing/debug)
    for (int y = 0; y < game.height; y++)
        for (int x = 0; x < game.width; x++)
            out[k++] = '.';

    // prekážky
    for (int i = 0; i < game.num_obstacles; i++) {
        int x = game.obstacles[i][0], y = game.obstacles[i][1];
        if (x>=0 && x<game.width && y>=0 && y<game.height)
            out[y*game.width + x] = '#';
    }

    // ovocie
    if (game.fruit_x>=0 && game.fruit_x<game.width && game.fruit_y>=0 && game.fruit_y<game.height)
        out[game.fruit_y*game.width + game.fruit_x] = '*';

    // hady (server má body_x/body_y, tam je pravda)
    for (int i = 0; i < game.num_players; i++) {
        Player *p = &game.players[i];
        if (!p->alive) continue;

        // telo
        for (int j = 1; j < p->body_len; j++) {
            int x = p->body_x[j], y = p->body_y[j];
            if (x>=0 && x<game.width && y>=0 && y<game.height)
                out[y*game.width + x] = '~';
        }

        // hlava
        if (p->head_x>=0 && p->head_x<game.width && p->head_y>=0 && p->head_y<game.height)
            out[p->head_y*game.width + p->head_x] = '@';
    }

    out[k] = '\0';
}

void send_game_state(int client_socket) {
    pthread_mutex_lock(&game_mutex);

    char response[BUFFER_SIZE];
    snprintf(response, BUFFER_SIZE,
             "STATE|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|",
             game.id, game.width, game.height, game.num_players,
             game.fruit_x, game.fruit_y, game.active, game.game_over,
             game.num_obstacles, game.mode, game.world_type, game.elapsed_time);

    char mapbuf[WORLD_WIDTH * WORLD_HEIGHT + 1];
    build_map(mapbuf);

    if (strlen(response) + 2 + strlen(mapbuf) + 2 < BUFFER_SIZE) {
        strcat(response, "M|");
        strcat(response, mapbuf);
        strcat(response, "|");
    }

    for (int i = 0; i < game.num_obstacles; i++) {
        int x = game.obstacles[i][0];
        int y = game.obstacles[i][1];
        char obs_str[30];
        snprintf(obs_str, 30, "O|%d|%d|", x, y);
        if (strlen(response) + strlen(obs_str) + 1 < BUFFER_SIZE) strcat(response, obs_str);
    }

    for (int i = 0; i < game.num_players; i++) {
        Player* p = &game.players[i];
        char player_str[200];
        snprintf(player_str, 200,
                 "P|%d|%s|%d|%d|%d|%d|%d|%d|",
                 p->id, p->name, p->alive, p->score,
                 p->head_x, p->head_y, p->body_len, p->direction);
        if (strlen(response) + strlen(player_str) + 1 < BUFFER_SIZE)
            strcat(response, player_str);
    }

    send(client_socket, response, strlen(response), 0);
    pthread_mutex_unlock(&game_mutex);
}

void handle_client_message(const char* buffer) {
    if (strncmp(buffer, "NEW_GAME", 8) == 0) {
        int mode, world_type, time_limit;
        sscanf(buffer, "NEW_GAME|%d|%d|%d", &mode, &world_type, &time_limit);
        init_game(WORLD_WIDTH, WORLD_HEIGHT, (GameMode)mode, time_limit, (WorldType)world_type);
        fprintf(stderr, "[SERVER] Nová hra: režim=%d, svet=%d, čas=%d\n",
                mode, world_type, time_limit);
    } else if (strncmp(buffer, "PLAYER", 6) == 0) {
        char name[50];
        sscanf(buffer, "PLAYER|%49[^|]", name);
        init_snake(game.num_players, name);

        for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].in_use) {
            send_game_state(clients[i].socket);
        }
    }
    } else if (strncmp(buffer, "MOVE", 4) == 0) {
        int pid, dir;
        sscanf(buffer, "MOVE|%d|%d", &pid, &dir);
        if (pid < game.num_players && game.players[pid].alive)
            game.players[pid].next_direction = (Direction)dir;
    } else if (strncmp(buffer, "QUIT", 4) == 0) {
        int pid;
        sscanf(buffer, "QUIT|%d", &pid);
        if (pid < game.num_players) {
            game.players[pid].alive = 0;
            fprintf(stderr, "[SERVER] Hráč %d opustil hru\n", pid);
        }
    }
}

void* client_handler(void* arg) {
    int client_socket = *(int*)arg;
    free(arg);

    char buffer[BUFFER_SIZE];

    while (running) {
        int n = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        if (n <= 0) break;

        buffer[n] = '\0';

        pthread_mutex_lock(&game_mutex);
        handle_client_message(buffer);
        pthread_mutex_unlock(&game_mutex);

        send_game_state(client_socket);
    }

        close(client_socket);

    pthread_mutex_lock(&game_mutex);
    // označ slot ako voľný a zníž počítadlo klientov
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].socket == client_socket && clients[i].in_use) {
            clients[i].in_use = 0;
            clients[i].socket = -1;
            if (num_clients > 0) num_clients--;
            break;
        }
    }
    pthread_mutex_unlock(&game_mutex);

    fprintf(stderr, "[SERVER] Klient odpojený, aktívni klienti: %d\n", num_clients);
    return NULL;
}

int main() {
    fprintf(stderr, "SERVER HADIK - port %d\n", PORT);

    srand(time(NULL));
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].socket = -1;
        clients[i].in_use = 0;
    }

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_sock, MAX_CLIENTS) < 0) {
        perror("listen");
        return 1;
    }

    fprintf(stderr, "[SERVER] Čaká sa na klientov...\n");

    pthread_t game_thread;
    pthread_create(&game_thread, NULL, game_loop, NULL);

    while (running) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_socket = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);

    if (client_socket < 0) continue;

    pthread_mutex_lock(&game_mutex);

    if (num_clients >= MAX_CLIENTS) {
        // server je plný – pošli jednoduchú správu a zavri socket
        const char *full_msg = "SERVER_FULL\n";
        send(client_socket, full_msg, strlen(full_msg), 0);
        close(client_socket);
        pthread_mutex_unlock(&game_mutex);

        fprintf(stderr, "[SERVER] Pripojenie odmietnuté, MAX_CLIENTS=%d\n", MAX_CLIENTS);
        continue;
    }

    // nájdi voľný slot v poli clients
    int idx = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].in_use) {
            idx = i;
            break;
        }
    }

    if (idx == -1) {
        // nemalo by sa stať, ale pre istotu
        const char *full_msg = "SERVER_FULL\n";
        send(client_socket, full_msg, strlen(full_msg), 0);
        close(client_socket);
        pthread_mutex_unlock(&game_mutex);
        continue;
    }

    clients[idx].socket = client_socket;
    clients[idx].in_use = 1;
    num_clients++;

    fprintf(stderr, "[SERVER] Klient #%d sa pripojil: %s:%d (aktívni: %d)\n",
            idx,
            inet_ntoa(client_addr.sin_addr),
            ntohs(client_addr.sin_port),
            num_clients);

    pthread_mutex_unlock(&game_mutex);

    int* sock = malloc(sizeof(int));
    *sock = client_socket;

    pthread_t thread;
    pthread_create(&thread, NULL, client_handler, sock);
    pthread_detach(thread);
}


    running = 0;
    pthread_join(game_thread, NULL);
    close(server_sock);

    fprintf(stderr, "[SERVER] Server sa vypína...\n");
    return 0;
}
